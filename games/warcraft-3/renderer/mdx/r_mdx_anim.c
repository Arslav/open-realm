#include "r_mdx.h"
#include "renderer/r_local.h"
#include <ctype.h>

/* Shared state used across anim, geoset, and render translation units.
   Must live in the first file included by the unity build (alphabetically). */
mdlx_state_t mdlx;

/* Forward declarations for functions defined in r_mdx_interpolation.c */
uint32_t GetModelKeyTrackDataTypeSize(MODELKEYTRACKDATATYPE dataType);
uint32_t GetModelKeyTrackTypeSize(MODELKEYTRACKTYPE keyTrackType);
uint32_t GetModelKeyFrameSize(MODELKEYTRACKDATATYPE dataType, MODELKEYTRACKTYPE keyTrackType);
void R_GetKeyframeValue(mdxKeyFrame_t const *left, mdxKeyFrame_t const *right, mdxKeyTrack_t const *keytrack, uint32_t time, handle_t out);
void R_EvalKeyframeValue(void const *left, void const *right, float t, MODELKEYTRACKDATATYPE datatype, MODELKEYTRACKTYPE linetype, handle_t out);

mat4_t node_matrices[MDX_MAX_NODES];

#define MDLX_POSE_SLOTS 1024 // entries; bounds lookup work while covering hundreds of visible model instances
#define MDLX_POSE_MATRICES 65536 // matrices; 4 MiB scratch ceiling, retaining only nodes actually present in each pose
#define MDLX_POSE_HASH_PRIME 16777619u // FNV multiplier; mixes exact pose inputs into the bounded hash table
#ifdef __ANDROID__
#define BZ_MDX_PACKET_PREPARE_DEFAULT "2" // mode; Android build enables joined packet preparation at the owner's request
#else
#define BZ_MDX_PACKET_PREPARE_DEFAULT "0" // mode; other platforms retain on-demand preparation
#endif

typedef struct {
    mdxModel_t const *model;
    mat4_t matrix;
    uint32_t frame0, frame1, offset, epoch, time;
    bool prepared;
    uint16_t queued_job;
    mdxDrawPacket_t *packet;
} mdxPoseSlot_t;

static mdxPoseSlot_t pose_slots[MDLX_POSE_SLOTS];
static mat4_t pose_matrices[MDLX_POSE_MATRICES];
static uint32_t pose_epoch, pose_used, pose_time;
static float pose_lerp;
static mat4_t pose_view;

/* Back-to-back node samples share sequence and key-range searches only for this stack-bound pose. */
typedef struct {
    mdxModel_t const *model;
    mdxSequence_t const *sequences[2];
    uint32_t frames[2], interval[2], first, end;
    mdxKeyTrack_t const *track;
} mdxSampleScope_t;
typedef struct {
    mat4_t local[MDX_MAX_NODES], *global;
    mdxSampleScope_t *scope;
    uint32_t time;
    float lerp;
} mdxPoseContext_t;
static mdxPoseContext_t draw_pose = {.global = node_matrices};

#define MDLX_POSE_JOBS 512 // reservations; excess actors retain on-demand evaluation
#define MDLX_POSE_CHUNK 4 // independent poses claimed together to bound queue traffic
#define MDLX_POSE_MIN_JOBS 16 // small views retain the existing demand-driven path
typedef struct {
    mdxModel_t const *model;
    mat4_t matrix;
    mdxPoseMask_t needed;
    uint32_t frames[2], offset, nodes, lane, time;
    mdxPoseSlot_t *slot;
} mdxPoseJob_t;
static struct {
    mdxPoseJob_t jobs[MDLX_POSE_JOBS];
    uint32_t count, workers, epoch, time, packets;
    float lerp;
    mat4_t view;
    bool collecting;
    Uint64 start;
    mdxPoseStats_t stats;
} pose_prepare;
typedef struct {
    SDL_Thread *thread;
    mdxPoseContext_t context;
    mat4_t global[MDX_MAX_NODES];
    uint32_t lane, executed;
} mdxPoseWorker_t;
static struct {
    SDL_mutex *mutex;
    SDL_cond *start, *done;
    mdxPoseWorker_t *workers;
    uint32_t count, epoch, remaining;
    SDL_atomic_t next;
    bool stop;
} pose_pool;

/* Release/reload can reuse a model address within one render clock; no cached pointer may survive it. */
void MDLX_ClearPoseCache(void) {
    pose_prepare.count = 0; /* Reservations cannot survive a released/reloaded model or view. */
    MDLX_ClearDrawPackets();
    pose_used = pose_time = 0;
    if (!++pose_epoch) {
        memset(pose_slots, 0, sizeof(pose_slots));
        pose_epoch = 1;
    }
}

/* Exact inputs permit reuse across solid/alpha and socket consumers without sharing different billboard bases. */
static mdxPoseSlot_t *MDLX_PoseSlot(mdxModel_t const *model, mat4_t const *matrix, uint32_t frame1, uint32_t frame0) {
    if (!tr.viewDef.time || !model->num_nodes) return NULL;
    if (pose_time != tr.viewDef.time || pose_lerp != tr.viewDef.lerpfrac ||
        memcmp(&pose_view, &tr.viewDef.viewProjectionMatrix, sizeof(pose_view))) {
        MDLX_ClearPoseCache();
        pose_time = tr.viewDef.time; pose_lerp = tr.viewDef.lerpfrac; pose_view = tr.viewDef.viewProjectionMatrix;
    }
    uint64_t id = (uintptr_t)model;
    uint32_t hash = ((uint32_t)(id ^ (id >> 32)) ^ frame1) * MDLX_POSE_HASH_PRIME;
    hash = (hash ^ frame0) * MDLX_POSE_HASH_PRIME;
    uint32_t time = model->num_globalSequences ? MDLX_GlobalAnimationTime() : 0;
    hash = (hash ^ time) * MDLX_POSE_HASH_PRIME;
    FOR_LOOP(i, 16) {
        uint32_t bits;
        memcpy(&bits, &matrix->v[i], sizeof(bits));
        hash = (hash ^ bits) * MDLX_POSE_HASH_PRIME;
    }
    hash = (hash ^ (hash >> 16)) * MDLX_POSE_HASH_PRIME;
    hash ^= hash >> 16;
    FOR_LOOP(i, MDLX_POSE_SLOTS) {
        mdxPoseSlot_t *slot = &pose_slots[(hash + i) & (MDLX_POSE_SLOTS - 1)];
        if (slot->epoch != pose_epoch)
            return pose_used + model->num_nodes <= MDLX_POSE_MATRICES ? slot : NULL;
        if (slot->model == model && slot->frame0 == frame0 && slot->frame1 == frame1 && slot->time == time &&
            !memcmp(&slot->matrix, matrix, sizeof(*matrix)))
            return slot;
    }
    return NULL; /* A full optional cache still evaluates the complete pose normally. */
}

#ifdef BZ_TESTS
_Atomic uint32_t MDLX_TestPoseEvaluations;
_Atomic uint32_t MDLX_TestNodeEvaluations;
_Atomic uint32_t MDLX_TestSequenceLookups, MDLX_TestTrackBounds;
void MDLX_TestResetPoseCache(void) { MDLX_ClearPoseCache(); MDLX_TestPoseEvaluations = MDLX_TestNodeEvaluations = 0; }
#endif

mdxSequence_t const *R_FindSequenceAtTime(mdxModel_t const *model, uint32_t time) {
#ifdef BZ_TESTS
    MDLX_TestSequenceLookups++;
#endif
    FOR_LOOP(seqIndex, model->num_sequences) {
        mdxSequence_t const *seq = &model->sequences[seqIndex];
        if (seq->interval[0] <= time && seq->interval[1] > time) {
            return seq;
        }
    }
    return NULL;
}

bool MDLX_EventKeyCrossed(mdxModel_t const *model, mdxEvent_t const *event, uint32_t key,
                          uint32_t previous_frame, uint32_t current_frame,
                          uint32_t previous_time, uint32_t current_time) {
    mdxSequence_t const *previous_seq, *current_seq;

    if (!model || !event) return false;
    if (event->globalSeqId != (uint32_t)-1) {
        uint32_t duration, previous, current;
        if (event->globalSeqId >= (uint32_t)model->num_globalSequences || !model->globalSequences) return false;
        duration = model->globalSequences[event->globalSeqId].value;
        if (!duration) return false;
        if (current_time >= previous_time && current_time - previous_time >= duration) return true;
        previous = previous_time % duration;
        current = current_time % duration;
        return current >= previous ? (key > previous && key <= current)
                                   : (key > previous || key <= current);
    }

    previous_seq = R_FindSequenceAtTime(model, previous_frame);
    current_seq = R_FindSequenceAtTime(model, current_frame);
    if (!current_seq || key < current_seq->interval[0] || key > current_seq->interval[1]) return false;
    if (previous_seq != current_seq) return key >= current_seq->interval[0] && key <= current_frame;
    if (current_frame >= previous_frame) return key > previous_frame && key <= current_frame;
    return key > previous_frame || key <= current_frame;
}

bool MDLX_EventObjectId(mdxEvent_t const *event, cstring_t type, char *out, uint32_t out_size) {
    size_t name_len, offset, len;
    if (!event || !type || !out || out_size < 2 || strlen(type) != 3) return false;
    name_len = strnlen(event->node.name, sizeof(event->node.name));
    if (name_len <= 4 || strncmp(event->node.name, type, 3)) return false;
    offset = 4;
    while (offset < name_len && isspace((unsigned char)event->node.name[offset])) offset++;
    len = name_len - offset;
    while (len && isspace((unsigned char)event->node.name[offset + len - 1])) len--;
    if (!len) return false;
    len = MIN(len, (size_t)out_size - 1);
    memcpy(out, event->node.name + offset, len); out[len] = '\0';
    return true;
}

/* Keep the animated node basis and place the child at its transformed pivot. */
bool MDLX_EventWorldTransform(mdxModel_t const *model, mdxEvent_t const *event,
                              renderEntity_t const *entity, mat4_t const *model_transform,
                              mat4_t *out) {
    vec3_t pivot = {0}, local, world;
    if (!model || !event || !entity || !model_transform || !out) return false;
    MDLX_BindBoneMatrices(model, model_transform, entity->frame, entity->oldframe);
    if (event->node.node_id < (uint32_t)model->num_pivots) pivot = model->pivots[event->node.node_id];
    local = pivot; *out = *model_transform;
    if (event->node.node_id < MDX_MAX_NODES && model->nodes[event->node.node_id]) {
        local = Matrix4_multiply_vector3(&node_matrices[event->node.node_id], &pivot);
        Matrix4_multiply(model_transform, &node_matrices[event->node.node_id], out);
    }
    world = Matrix4_multiply_vector3(model_transform, &local);
    out->v[12] = world.x; out->v[13] = world.y; out->v[14] = world.z;
    return true;
}

static mdxKeyFrame_t *R_KeyFrameAt(mdxKeyTrack_t const *track, uint32_t stride, uint32_t index) {
    return (mdxKeyFrame_t *)((string_t)track->values + stride * index);
}

typedef struct {
    uint32_t generation;
    uint32_t last_time;
    uint32_t pause_start;
    uint32_t paused_total;
    bool valid;
    bool paused;
} mdxGlobalSequencePauseState_t;

static mdxGlobalSequencePauseState_t global_sequence_pause[MAX_GAME_ENTITIES];
static _Thread_local bool entity_global_time_valid;
static _Thread_local uint32_t entity_global_time;

/* Worker packet samplers and owner draws must use the same captured entity clock. */
uint32_t MDLX_GlobalAnimationTime(void) {
    return entity_global_time_valid ? entity_global_time : (tr.viewDef.time ? tr.viewDef.time : SDL_GetTicks());
}

static void MDLX_PushGlobalAnimationTime(uint32_t time, mdxAnimationTimeScope_t *scope) {
    *scope = (mdxAnimationTimeScope_t){.previous_valid = entity_global_time_valid, .previous_time = entity_global_time};
    entity_global_time = time; entity_global_time_valid = true;
}

/* MDX global sequences usually follow view time, but held construction must
 * keep per-entity global tracks (for example a Farm's globally animated saw)
 * at their current phase. Subtract accumulated paused time on resume so the
 * track continues from that phase instead of jumping forward. */
void MDLX_UpdateEntityAnimationPause(renderEntity_t const *entity) {
    uint32_t now = tr.viewDef.time ? tr.viewDef.time : SDL_GetTicks();
    if (entity && entity->number < MAX_GAME_ENTITIES) {
        mdxGlobalSequencePauseState_t *state = &global_sequence_pause[entity->number];
        bool const paused = (entity->flags & RF_MDX_ANIMATION_PAUSED) != 0;
        if (!state->valid || state->generation != entity->generation) {
            *state = (mdxGlobalSequencePauseState_t){ .generation = entity->generation,
                .last_time = now, .pause_start = now, .valid = true, .paused = paused };
        } else {
            if (now < state->last_time) {
                state->paused_total = 0;
                state->pause_start = now;
            }
            state->last_time = now;
            if (paused && !state->paused) {
                state->pause_start = now;
                state->paused = true;
            } else if (!paused && state->paused) {
                state->paused_total += now - state->pause_start;
                state->paused = false;
            }
        }
    }
}

void MDLX_PushEntityAnimationTime(renderEntity_t const *entity, mdxAnimationTimeScope_t *scope) {
    uint32_t now = tr.viewDef.time ? tr.viewDef.time : SDL_GetTicks();
    uint32_t effective = now;
    if (!scope) return;
    MDLX_UpdateEntityAnimationPause(entity);
    if (entity && entity->number < MAX_GAME_ENTITIES) {
        mdxGlobalSequencePauseState_t const *state = &global_sequence_pause[entity->number];
        effective = state->paused ? state->pause_start - state->paused_total : now - state->paused_total;
    }
    MDLX_PushGlobalAnimationTime(effective, scope);
}

void MDLX_PopEntityAnimationTime(mdxAnimationTimeScope_t const *scope) {
    if (!scope) return;
    entity_global_time = scope->previous_time;
    entity_global_time_valid = scope->previous_valid;
}

/* MDX key times are authored in ascending order; binary bounds avoid rescanning every track for every model instance. */
static uint32_t R_KeyFrameBound(mdxKeyTrack_t const *track, uint32_t stride, uint32_t time, bool upper) {
#ifdef BZ_TESTS
    MDLX_TestTrackBounds++;
#endif
    uint32_t lo = 0, hi = track->keyframeCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t keytime = R_KeyFrameAt(track, stride, mid)->time;
        if (keytime < time || (upper && keytime == time)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void MDLX_SampleTrack(mdxPoseContext_t *context, mdxModel_t const *model, mdxKeyTrack_t const *keytrack, uint32_t time, handle_t output) {
    mdxSampleScope_t *sample_scope = context ? context->scope : NULL;
    uint32_t interval[2] = { 0, 0 };
    uint32_t stride;

    if (!model || !keytrack || !output || !keytrack->keyframeCount)
        return;
    stride = GetModelKeyFrameSize(keytrack->datatype, keytrack->linetype);
    if (keytrack->globalSeqId != (uint32_t)-1) {
        mdxKeyFrame_t *first_authored;

        if (keytrack->globalSeqId >= (uint32_t)model->num_globalSequences || !model->globalSequences)
            return;
        interval[0] = 0;
        interval[1] = model->globalSequences[keytrack->globalSeqId].value;

        /* Warsmash treats a global sequence whose first authored key lies
         * beyond the declared duration as a constant equal to that first key.
         * Stock DNC models use this odd contract for their light-node rotation:
         * a zero-duration global sequence points at a later quaternion key. */
        first_authored = R_KeyFrameAt(keytrack, stride, 0);
        if (first_authored->time >= 0 && (uint32_t)first_authored->time > interval[1]) {
            memcpy(output, first_authored->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
            return;
        }

        /* Global sequences follow the render clock instead of entity->frame,
           which loops within the selected sequence. This preserves their full
           range while keeping fixed-time renderer captures deterministic. */
        {
            uint32_t gs_len = interval[1] + 1;
            uint32_t global_time = context ? context->time : MDLX_GlobalAnimationTime();
            time = gs_len > 0 ? (global_time % gs_len) : 0;
        }
    } else {
        mdxSequence_t const *seq;
        if (sample_scope && sample_scope->model == model && time == sample_scope->frames[0])
            seq = sample_scope->sequences[0];
        else if (sample_scope && sample_scope->model == model && time == sample_scope->frames[1])
            seq = sample_scope->sequences[1];
        else
            seq = R_FindSequenceAtTime(model, time);
        if (!seq)
            return;
        interval[0] = seq->interval[0];
        interval[1] = seq->interval[1];
    }
    if (keytrack->keyframeCount == 1) {
        mdxKeyFrame_t const *key = keytrack->values;
        if ((uint32_t)key->time >= interval[0] && (uint32_t)key->time <= interval[1])
            memcpy(output, key->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
        return;
    }
    uint32_t right_index = R_KeyFrameBound(keytrack, stride, time, false);
    mdxKeyFrame_t *right = right_index < keytrack->keyframeCount ? R_KeyFrameAt(keytrack, stride, right_index) : NULL;
    if (right && (uint32_t)right->time == time) {
        bool has_next = right_index + 1 < keytrack->keyframeCount;
        uint32_t next_time = has_next ? R_KeyFrameAt(keytrack, stride, right_index + 1)->time : 0;
        if ((has_next && next_time > time && next_time <= interval[1]) ||
            ((!has_next || next_time > interval[1]) &&
             (!right_index || (uint32_t)R_KeyFrameAt(keytrack, stride, right_index - 1)->time < interval[0]))) {
            memcpy(output, right->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
            return;
        }
    }
    /* Interior neighbours prove sequence membership without searching both interval bounds.
     * Exact keys retain the tail path below, including duplicate keys at a sequence endpoint. */
    if (right && time < (uint32_t)right->time && (uint32_t)right->time <= interval[1]) {
        mdxKeyFrame_t const *left = right_index ? R_KeyFrameAt(keytrack, stride, right_index - 1) : NULL;
        if (left && (uint32_t)left->time >= interval[0])
            R_GetKeyframeValue(left, right, keytrack, time, output);
        else
            memcpy(output, right->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
        return;
    }
    /* A track can have many keys overall but only one in this sequence. */
    if (!right || (uint32_t)right->time > interval[1]) {
        if (!right_index) return;
        uint32_t last_index = right_index - 1;
        mdxKeyFrame_t const *last = R_KeyFrameAt(keytrack, stride, last_index);
        if ((uint32_t)last->time < interval[0]) return;
        if ((uint32_t)last->time <= interval[1] &&
            (!last_index || (uint32_t)R_KeyFrameAt(keytrack, stride, last_index - 1)->time < interval[0])) {
            memcpy(output, last->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
            return;
        }
    }
    uint32_t first_index, end_index;
    if (sample_scope && sample_scope->model == model && sample_scope->track == keytrack &&
        sample_scope->interval[0] == interval[0] && sample_scope->interval[1] == interval[1]) {
        first_index = sample_scope->first; end_index = sample_scope->end;
    } else {
        first_index = R_KeyFrameBound(keytrack, stride, interval[0], false);
        end_index = R_KeyFrameBound(keytrack, stride, interval[1], true);
        if (sample_scope && sample_scope->model == model) {
            sample_scope->track = keytrack; sample_scope->interval[0] = interval[0]; sample_scope->interval[1] = interval[1];
            sample_scope->first = first_index; sample_scope->end = end_index;
        }
    }
    if (first_index >= end_index)
        return;
    mdxKeyFrame_t *first = R_KeyFrameAt(keytrack, stride, first_index);
    mdxKeyFrame_t *last = R_KeyFrameAt(keytrack, stride, end_index - 1);
    if (time >= last->time) {
        /* The interval tail blends back to its first key; keys outside this sequence never participate. */
        uint32_t span = (interval[1] - last->time) + (first->time - interval[0]);
        if (first != last && span && time < interval[1]) {
            float t = (float)(time - last->time) / (float)span;
            R_EvalKeyframeValue(last->data, first->data, t, keytrack->datatype, keytrack->linetype, output);
        } else {
            memcpy(output, last->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
        }
        return;
    }
    if (right_index == first_index || right->time == time)
        memcpy(output, right->data, GetModelKeyTrackDataTypeSize(keytrack->datatype));
    else
        R_GetKeyframeValue(R_KeyFrameAt(keytrack, stride, right_index - 1), right, keytrack, time, output);
}

void MDLX_GetModelKeytrackValue(mdxModel_t const *model, mdxKeyTrack_t const *keytrack, uint32_t time, handle_t output) {
    MDLX_SampleTrack(NULL, model, keytrack, time, output);
}

/* Warcraft animated MDX color tracks use BGR component semantics in the file.
 * Convert the float vector to renderer RGB after interpolation. This is not a
 * texture byte-order conversion, so it is identical on GL, GLES, and hosts with
 * or without native BGRA texture-upload support. */
void MDLX_GetAnimatedColorTrackValue(mdxModel_t const *model,
                                     mdxKeyTrack_t const *keytrack,
                                     uint32_t time,
                                     vec3_t *output)
{
    float red;

    if (!model || !keytrack || !output) return;
    MDLX_GetModelKeytrackValue(model, keytrack, time, output);
    red = output->x;
    output->x = output->z;
    output->z = red;
}

/* Static GEOA colors are already RGB. Warsmash swaps them into its BGR shader input, then swaps back in
 * MdxShaders; copying only its first swap here turned the Orc campaign's orange sky blue. */
void MDLX_GetGeosetAnimationStaticColor(mdxGeosetAnim_t const *geosetAnim, vec3_t *output) {
    if (!geosetAnim || !output) return;
    *output = geosetAnim->staticColor;
}

static void R_CalculateNodeMatrix(mdxPoseContext_t *context, mdxModel_t const *model, mdxNode_t *node, uint32_t frame1, uint32_t frame0, mat4_t *matrix) {
#ifdef BZ_TESTS
    MDLX_TestNodeEvaluations++;
#endif
    vec3_t vTranslation = { 0, 0, 0 };
    quaternion_t vRotation = { 0, 0, 0, 1 };
    vec3_t vScale = { 1, 1, 1 };
    vec3_t zero_pivot = { 0, 0, 0 };
    vec3_t const *pivot = &zero_pivot;
    if (node->node_id < (uint32_t)model->num_pivots) {
        pivot = (vec3_t const *)&model->pivots[node->node_id];
    }
    if (frame0 != frame1) {
        if (node->translation) {
            vec3_t t0 = vTranslation, t1 = vTranslation;
            MDLX_SampleTrack(context, model, node->translation, frame0, &t0);
            MDLX_SampleTrack(context, model, node->translation, frame1, &t1);
            vTranslation = Vector3_lerp(&t0, &t1, context->lerp);
        }
        if (node->rotation) {
            quaternion_t r0 = vRotation, r1 = vRotation;
            MDLX_SampleTrack(context, model, node->rotation, frame0, &r0);
            MDLX_SampleTrack(context, model, node->rotation, frame1, &r1);
            vRotation = Quaternion_slerp(&r0, &r1, context->lerp);
        }
        if (node->scale) {
            vec3_t s0 = vScale, s1 = vScale;
            MDLX_SampleTrack(context, model, node->scale, frame0, &s0);
            MDLX_SampleTrack(context, model, node->scale, frame1, &s1);
            vScale = Vector3_lerp(&s0, &s1, context->lerp);
        }
    } else {
        if (node->translation) {
            MDLX_SampleTrack(context, model, node->translation, frame1, &vTranslation);
        }
        if (node->rotation) {
            MDLX_SampleTrack(context, model, node->rotation, frame1, &vRotation);
        }
        if (node->scale) {
            MDLX_SampleTrack(context, model, node->scale, frame1, &vScale);
        }
    }
    if (!node->translation && !node->rotation && !node->scale) {
        Matrix4_identity(matrix);
    } else if (node->translation && !node->rotation && !node->scale) {
        Matrix4_from_translation(matrix, &vTranslation);
    } else if (!node->translation && node->rotation && !node->scale) {
        Matrix4_from_rotation_origin(matrix,  &vRotation, pivot);
    } else {
        Matrix4_from_rotation_translation_scale_origin(matrix, &vRotation, &vTranslation, &vScale, pivot);
    }
}

static mat4_t const *MDLX_NodeGlobal(mdxPoseContext_t *context, mdxModel_t const *model, mat4_t const *model_matrix, mdxNode_t const *node) {
    if (!node || node->node_id >= MDX_MAX_NODES) {
        return NULL;
    }
    mat4_t *global_matrix = context->global + node->node_id;
    mat4_t *local_matrix = context->local + node->node_id;
    if (global_matrix->v[15] == 0) {
        if (node->parent_id != -1 && node->parent_id < MDX_MAX_NODES && model->nodes[node->parent_id]) {
            mat4_t const *parent_matrix = MDLX_NodeGlobal(context, model, model_matrix, model->nodes[node->parent_id]);
            if (!parent_matrix) {
                return NULL;
            }
            Matrix4_multiply(parent_matrix, local_matrix, global_matrix);
        } else {
            *global_matrix = *local_matrix;
        }
        if (node->flags & MDLXNODE_Billboarded) {
            mat4_t tmp1, tmp2;
            vec3_t pivot = { 0, 0, 0 };
            if (node->node_id < (uint32_t)model->num_pivots) {
                pivot = *(vec3_t const *)(&model->pivots[node->node_id]);
            }
            vec3_t tmppvt = Matrix4_multiply_vector3(global_matrix, &pivot);
            if (node->parent_id != -1 && node->parent_id < MDX_MAX_NODES && model->nodes[node->parent_id]) {
                mat4_t const *parent_matrix = MDLX_NodeGlobal(context, model, model_matrix, model->nodes[node->parent_id]);
                if (!parent_matrix) {
                    return NULL;
                }
                /* Parent animation may include breathing scale. Extract rotation from unit axes;
                 * scaled columns skewed billboard cancellation even with the correct camera. */
                mat4_t rotation = *parent_matrix;
                FOR_LOOP(i, 3) Vector3_normalize((vec3_t *)rotation.m[i]);
                quaternion_t tmprot = Quaternion_fromMatrix(&rotation);
                tmprot = Quaternion_unm(&tmprot);
                Matrix4_from_rotation_origin(&tmp1, &tmprot, &tmppvt);
                Matrix4_multiply(&tmp1, global_matrix, &tmp2);
                *global_matrix = tmp2;
            }
            
            /* Projection rows retain camera right/up even for authored cameras and camera roll.
             * MDX billboard quads lie in YZ, with their normal along +X. */
            mat4_t const *vp = &tr.viewDef.viewProjectionMatrix;
            vec3_t right = {vp->v[0], vp->v[4], vp->v[8]};
            vec3_t up = {vp->v[1], vp->v[5], vp->v[9]};
            Vector3_normalize(&right);
            Vector3_normalize(&up);
            vec3_t front = Vector3_cross(&right, &up);
            Matrix4_identity(&tmp1);
            FOR_LOOP(i, 3) {
                tmp1.m[i][0] = ((float *)&front)[i];
                tmp1.m[i][1] = ((float *)&right)[i];
                tmp1.m[i][2] = ((float *)&up)[i];
            }
            Matrix4_multiply(&tmp1, model_matrix, &tmp2);
            FOR_LOOP(i, 3) Vector3_normalize((vec3_t *)tmp2.m[i]);

            quaternion_t viewrot = Quaternion_fromMatrix(&tmp2);
            viewrot = Quaternion_unm(&viewrot);
            Matrix4_from_rotation_origin(&tmp1, &viewrot, &tmppvt);
            Matrix4_multiply(&tmp1, global_matrix, &tmp2);
            *global_matrix = tmp2;
        }
    }
    return global_matrix;
}

mat4_t const *R_GetNodeGlobalMatrix(mdxModel_t const *model, mat4_t const *matrix, mdxNode_t const *node) {
    return MDLX_NodeGlobal(&draw_pose, model, matrix, node);
}

void AddSkin(vec3_t *pos, mat4_t const *mat, vec3_t const *org, float weight) {
    if (weight == 0) return;
    vec3_t val = Matrix4_multiply_vector3(mat, org);
    val = Vector3_scale(&val, weight);
    *pos = Vector3_add(pos, &val);
}

/* Resolve the exact dependency closure before reserving or evaluating a pose. */
static bool MDLX_RequiredNodes(mdxModel_t const *model, uint32_t const *mask, mdxPoseMask_t needed) {
    memset(needed, 0, sizeof(mdxPoseMask_t));
    if (mask) memcpy(needed, mask, sizeof(mdxPoseMask_t));
    else FOR_LOOP(i, model->num_nodes) {
        uint32_t id = model->node_list[i]->node_id;
        if (id < MDX_MAX_NODES) needed[id / 32] |= 1u << (id % 32);
    }
    if (mask) FOR_LOOP(i, model->num_nodes) {
        mdxNode_t const *node = model->node_list[i];
        if (node->node_id >= MDX_MAX_NODES) continue;
        if (!(needed[node->node_id / 32] & (1u << (node->node_id % 32)))) continue;
        while (node->parent_id < MDX_MAX_NODES && model->nodes[node->parent_id]) {
            node = model->nodes[node->parent_id];
            if (node->node_id >= MDX_MAX_NODES) break;
            uint32_t bit = 1u << (node->node_id % 32);
            if (needed[node->node_id / 32] & bit) break;
            needed[node->node_id / 32] |= bit;
        }
    }
    FOR_LOOP(i, model->num_nodes) {
        mdxNode_t const *node = model->node_list[i];
        if (node->node_id < MDX_MAX_NODES && (needed[node->node_id / 32] & (1u << (node->node_id % 32))) && (node->flags & MDLXNODE_Billboarded)) return true;
    }
    return false;
}

/* All mutable evaluation state belongs to the caller; model tracks are immutable. */
static uint32_t MDLX_EvaluatePose(mdxPoseContext_t *context, mdxModel_t const *model, mat4_t const *matrix,
                                  uint32_t frame1, uint32_t frame0, mdxPoseMask_t const needed) {
    uint32_t changed = 0;
    mdxSampleScope_t scope = {.model = model, .frames = {frame0, frame1}};
    mdxSampleScope_t *previous = context->scope;
    FOR_LOOP(i, model->num_nodes) {
        mdxNode_t *node = model->node_list[i];
        if (node->node_id >= MDX_MAX_NODES || !(needed[node->node_id / 32] & (1u << (node->node_id % 32))) || context->global[node->node_id].v[15] != 0) continue;
        if (!changed) {
            scope.sequences[0] = R_FindSequenceAtTime(model, frame0);
            scope.sequences[1] = frame0 == frame1 ? scope.sequences[0] : R_FindSequenceAtTime(model, frame1);
            context->scope = &scope;
        }
        R_CalculateNodeMatrix(context, model, node, frame1, frame0, &context->local[node->node_id]);
        changed++;
    }
    context->scope = previous;
    if (!changed) return 0;
#ifdef BZ_TESTS
    MDLX_TestPoseEvaluations++;
#endif
    FOR_LOOP(i, model->num_nodes) {
        mdxNode_t const *node = model->node_list[i];
        if (node->node_id < MDX_MAX_NODES && (needed[node->node_id / 32] & (1u << (node->node_id % 32))))
            MDLX_NodeGlobal(context, model, matrix, node);
    }
    return changed;
}

static void MDLX_ReservePoseSlot(mdxPoseSlot_t *slot, mdxModel_t const *model, mat4_t const *key, uint32_t frame1, uint32_t frame0) {
    *slot = (mdxPoseSlot_t){.model = model, .matrix = *key, .frame0 = frame0, .frame1 = frame1,
        .offset = pose_used, .epoch = pose_epoch, .time = model->num_globalSequences ? MDLX_GlobalAnimationTime() : 0};
    pose_used += model->num_nodes;
}

static void MDLX_RestorePose(mdxPoseContext_t *context, mdxModel_t const *model, mat4_t const *packed) {
    FOR_LOOP(i, model->num_nodes) {
        uint32_t id = model->node_list[i]->node_id;
        if (id < MDX_MAX_NODES) context->global[id] = packed ? packed[i] : (mat4_t){0};
    }
}
static void MDLX_StorePose(mdxPoseContext_t const *context, mdxModel_t const *model, mat4_t *packed) {
    FOR_LOOP(i, model->num_nodes) {
        uint32_t id = model->node_list[i]->node_id;
        packed[i] = id < MDX_MAX_NODES ? context->global[id] : (mat4_t){0};
    }
}

static void MDLX_BindPose(mdxModel_t const *model, mat4_t const *matrix, uint32_t frame1, uint32_t frame0, uint32_t const *mask) {
    mdxPoseMask_t needed;
    bool billboard = MDLX_RequiredNodes(model, mask, needed);
    mat4_t key = billboard ? *matrix : (mat4_t){0};
    mdxPoseSlot_t *slot = MDLX_PoseSlot(model, &key, frame1, frame0);
    MDLX_RestorePose(&draw_pose, model, slot && slot->epoch == pose_epoch ? pose_matrices + slot->offset : NULL);
    draw_pose.time = MDLX_GlobalAnimationTime(); draw_pose.lerp = tr.viewDef.lerpfrac;
    if (!MDLX_EvaluatePose(&draw_pose, model, matrix, frame1, frame0, needed)) {
        if (slot && slot->epoch == pose_epoch && slot->prepared) pose_prepare.stats.hits++;
        return;
    }
    if (slot) {
        if (slot->epoch != pose_epoch) MDLX_ReservePoseSlot(slot, model, &key, frame1, frame0);
        slot->prepared = false;
        MDLX_StorePose(&draw_pose, model, pose_matrices + slot->offset);
    }
}

void MDLX_BindBoneMatrices(mdxModel_t const *model, mat4_t const *matrix, uint32_t frame1, uint32_t frame0) {
    MDLX_BindPose(model, matrix, frame1, frame0, NULL);
}
void MDLX_BindBoneSubset(mdxModel_t const *model, mat4_t const *matrix, uint32_t frame1, uint32_t frame0, mdxPoseMask_t const mask) {
    MDLX_BindPose(model, matrix, frame1, frame0, mask);
}

/* Slots and arena ranges are reserved only by the context owner, before any worker runs. */
bool MDLX_BeginPosePreparation(uint32_t workers) {
    workers = MIN(workers, MDLX_POSE_WORKERS);
    if (pose_pool.count != workers) MDLX_ShutdownPoseWorkers();
    pose_prepare.count = 0; pose_prepare.workers = workers;
    pose_prepare.stats = (mdxPoseStats_t){0};
    pose_prepare.packets = MAX(0, MIN(2, atoi(ri.CvarString("r_mdx_packet_prepare", BZ_MDX_PACKET_PREPARE_DEFAULT))));
    pose_prepare.time = tr.viewDef.time; pose_prepare.lerp = tr.viewDef.lerpfrac;
    pose_prepare.view = tr.viewDef.viewProjectionMatrix;
    pose_prepare.start = SDL_GetPerformanceCounter();
    return pose_prepare.collecting = workers && tr.viewDef.time;
}

bool MDLX_QueuePose(mdxModel_t const *model, mat4_t const *matrix, uint32_t frame1, uint32_t frame0, mdxPoseMask_t const mask) {
    if (!pose_prepare.collecting) return false;
    pose_prepare.stats.requests++;
    mdxPoseMask_t needed;
    bool billboard = MDLX_RequiredNodes(model, mask, needed);
    mat4_t key = billboard ? *matrix : (mat4_t){0};
    mdxPoseSlot_t *slot = MDLX_PoseSlot(model, &key, frame1, frame0);
    if (!slot) return false;
    if (slot->epoch != pose_epoch) {
        MDLX_ReservePoseSlot(slot, model, &key, frame1, frame0);
        memset(pose_matrices + slot->offset, 0, model->num_nodes * sizeof(mat4_t));
    }
    bool missing = false;
    FOR_LOOP(i, model->num_nodes) {
        uint32_t id = model->node_list[i]->node_id;
        missing |= id < MDX_MAX_NODES && (needed[id / 32] & (1u << (id % 32))) && !pose_matrices[slot->offset + i].v[15];
    }
    if (!missing) return true;
    uint32_t index = slot->queued_job;
    if (index < pose_prepare.count && pose_prepare.jobs[index].slot == slot) {
        FOR_LOOP(j, MDX_MAX_NODES / 32) pose_prepare.jobs[index].needed[j] |= needed[j];
        return true;
    }
    if (pose_prepare.count == MDLX_POSE_JOBS) return false;
    slot->queued_job = pose_prepare.count;
    mdxPoseJob_t *job = &pose_prepare.jobs[pose_prepare.count++];
    *job = (mdxPoseJob_t){.model = model, .matrix = *matrix, .frames = {frame0, frame1}, .offset = slot->offset, .slot = slot, .time = MDLX_GlobalAnimationTime()};
    slot->packet = pose_prepare.packets ? MDLX_ReserveDrawPacket(model, frame1) : NULL;
    memcpy(job->needed, needed, sizeof(needed));
    pose_prepare.epoch = pose_epoch;
    return true;
}

static void MDLX_RunPoseJob(mdxPoseContext_t *context, uint32_t index) {
    mdxPoseJob_t *job = &pose_prepare.jobs[index];
    mdxAnimationTimeScope_t scope; MDLX_PushGlobalAnimationTime(job->time, &scope);
    context->time = job->time;
    MDLX_RestorePose(context, job->model, pose_matrices + job->offset);
    job->nodes = MDLX_EvaluatePose(context, job->model, &job->matrix, job->frames[1], job->frames[0], job->needed);
    MDLX_StorePose(context, job->model, pose_matrices + job->offset);
    if (pose_prepare.packets == 2 && job->slot->packet) MDLX_PrepareDrawPacket(job->slot->packet, context->global);
    MDLX_PopEntityAnimationTime(&scope);
}

/* Each lane owns its first chunk, then claims remaining work dynamically. */
static uint32_t MDLX_RunPoseJobs(mdxPoseContext_t *context, uint32_t lane) {
    uint32_t executed = 0, first = lane * MDLX_POSE_CHUNK;
    context->time = pose_prepare.time; context->lerp = pose_prepare.lerp;
    do {
        for (uint32_t i = first; i < MIN(first + MDLX_POSE_CHUNK, pose_prepare.count); i++) {
            MDLX_RunPoseJob(context, i); executed++;
            pose_prepare.jobs[i].lane = lane;
        }
        first = SDL_AtomicAdd(&pose_pool.next, MDLX_POSE_CHUNK);
    } while (first < pose_prepare.count);
    return executed;
}

static void MDLX_PoseThreadCheck(int status) {
    if (status) { fprintf(stderr, "MDX pose worker: %s\n", SDL_GetError()); Sys_Exit(EXIT_FAILURE); }
}

static int MDLX_PoseWorker(void *arg) {
    mdxPoseWorker_t *worker = arg;
    uint32_t epoch = 0;
    MDLX_PoseThreadCheck(SDL_LockMutex(pose_pool.mutex));
    for (;;) {
        while (!pose_pool.stop && epoch == pose_pool.epoch)
            MDLX_PoseThreadCheck(SDL_CondWait(pose_pool.start, pose_pool.mutex));
        if (pose_pool.stop) break;
        epoch = pose_pool.epoch;
        MDLX_PoseThreadCheck(SDL_UnlockMutex(pose_pool.mutex));
        worker->executed = MDLX_RunPoseJobs(&worker->context, worker->lane);
        MDLX_PoseThreadCheck(SDL_LockMutex(pose_pool.mutex));
        if (!--pose_pool.remaining) MDLX_PoseThreadCheck(SDL_CondSignal(pose_pool.done));
    }
    MDLX_PoseThreadCheck(SDL_UnlockMutex(pose_pool.mutex));
    return 0;
}

void MDLX_ShutdownPoseWorkers(void) {
    pose_prepare.count = 0; pose_prepare.collecting = false;
    if (!pose_pool.count) return;
    MDLX_PoseThreadCheck(SDL_LockMutex(pose_pool.mutex));
    pose_pool.stop = true;
    MDLX_PoseThreadCheck(SDL_CondBroadcast(pose_pool.start));
    MDLX_PoseThreadCheck(SDL_UnlockMutex(pose_pool.mutex));
    FOR_LOOP(i, pose_pool.count) SDL_WaitThread(pose_pool.workers[i].thread, NULL);
    SDL_DestroyCond(pose_pool.start); SDL_DestroyCond(pose_pool.done); SDL_DestroyMutex(pose_pool.mutex);
    free(pose_pool.workers); memset(&pose_pool, 0, sizeof(pose_pool));
}

static void MDLX_StartPoseWorkers(void) {
    if (pose_pool.count) return;
    pose_pool.mutex = SDL_CreateMutex(); pose_pool.start = SDL_CreateCond(); pose_pool.done = SDL_CreateCond();
    pose_pool.workers = calloc(pose_prepare.workers, sizeof(*pose_pool.workers));
    if (!pose_pool.mutex || !pose_pool.start || !pose_pool.done || !pose_pool.workers) {
        fprintf(stderr, "MDX: unable to allocate pose workers: %s\n", SDL_GetError()); Sys_Exit(EXIT_FAILURE);
    }
    pose_pool.count = pose_prepare.workers;
    FOR_LOOP(i, pose_pool.count) {
        mdxPoseWorker_t *worker = pose_pool.workers + i;
        worker->lane = i + 1; worker->context.global = worker->global;
        worker->thread = SDL_CreateThread(MDLX_PoseWorker, "mdx-pose", worker);
        if (!worker->thread) { fprintf(stderr, "MDX: unable to start pose worker: %s\n", SDL_GetError()); Sys_Exit(EXIT_FAILURE); }
    }
}

bool MDLX_HasPoseWork(void) { return pose_prepare.count >= MDLX_POSE_MIN_JOBS && pose_prepare.epoch == pose_epoch; }

void MDLX_EndPosePreparation(void) {
    if (!pose_prepare.collecting) return;
    pose_prepare.collecting = false;
    if (pose_prepare.time != tr.viewDef.time || pose_prepare.lerp != tr.viewDef.lerpfrac ||
        memcmp(&pose_prepare.view, &tr.viewDef.viewProjectionMatrix, sizeof(pose_prepare.view))) {
        MDLX_ClearPoseCache(); return;
    }
    if (MDLX_HasPoseWork()) {
        MDLX_StartPoseWorkers();
        MDLX_PoseThreadCheck(SDL_LockMutex(pose_pool.mutex));
        pose_pool.remaining = pose_pool.count; pose_pool.epoch++;
        SDL_AtomicSet(&pose_pool.next, (pose_pool.count + 1) * MDLX_POSE_CHUNK);
        MDLX_PoseThreadCheck(SDL_CondBroadcast(pose_pool.start));
        MDLX_PoseThreadCheck(SDL_UnlockMutex(pose_pool.mutex));
        MDLX_RunPoseJobs(&draw_pose, 0);
        Uint64 wait = SDL_GetPerformanceCounter();
        MDLX_PoseThreadCheck(SDL_LockMutex(pose_pool.mutex));
        while (pose_pool.remaining) MDLX_PoseThreadCheck(SDL_CondWait(pose_pool.done, pose_pool.mutex));
        MDLX_PoseThreadCheck(SDL_UnlockMutex(pose_pool.mutex));
        pose_prepare.stats.wait_ms = (SDL_GetPerformanceCounter() - wait) * 1000.0 / SDL_GetPerformanceFrequency();
        pose_prepare.stats.jobs = pose_prepare.count;
        FOR_LOOP(i, pose_pool.count) {
            pose_prepare.stats.worker_jobs += pose_pool.workers[i].executed;
        }
        FOR_LOOP(i, pose_prepare.count) {
            mdxPoseJob_t *job = &pose_prepare.jobs[i];
            mdxDrawPacket_t *packet = job->slot->packet;
            if (pose_prepare.packets == 1 && packet) {
                mdxAnimationTimeScope_t scope; MDLX_PushGlobalAnimationTime(job->time, &scope);
                MDLX_RestorePose(&draw_pose, job->model, pose_matrices + job->offset);
                MDLX_PrepareDrawPacket(packet, draw_pose.global);
                MDLX_PopEntityAnimationTime(&scope);
            }
            pose_prepare.stats.nodes += job->nodes;
            pose_prepare.stats.packets += packet && packet->ready;
            pose_prepare.stats.worker_packets += packet && packet->ready && job->lane && pose_prepare.packets == 2;
            job->slot->prepared = true;
        }
    }
    pose_prepare.stats.prepare_ms = (SDL_GetPerformanceCounter() - pose_prepare.start) * 1000.0 / SDL_GetPerformanceFrequency();
    pose_prepare.count = 0;
}

mdxPoseStats_t MDLX_PosePreparationStats(void) { return pose_prepare.stats; }

/* A sparse non-billboard draw may share the zero placement key; billboard poses retain the exact actor basis. */
mdxDrawPacket_t const *MDLX_PreparedDraw(renderEntity_t const *entity, mdxModel_t const *model, mat4_t const *matrix) {
    if (!pose_prepare.packets) return NULL;
    uint32_t frame1 = entity->frame, frame0 = entity->oldframe;
    mat4_t key = {0};
    mdxPoseSlot_t *slot = MDLX_PoseSlot(model, &key, frame1, frame0);
    if (!slot || slot->epoch != pose_epoch || !slot->packet || !slot->packet->ready)
        slot = MDLX_PoseSlot(model, matrix, frame1, frame0);
    if (slot && slot->epoch == pose_epoch && slot->packet && slot->packet->ready) {
        pose_prepare.stats.packet_hits++;
        return slot->packet;
    }
    pose_prepare.stats.packet_misses++;
    return NULL;
}

/* Resolve authored attachment pivots from the same interpolated node pose used
 * for geometry. Callers can filter by a name prefix (for example "Sprite ")
 * without depending on list order in the MDX file. */
static uint32_t MDLX_CollectAttachments(mdxModel_t const *model, mat4_t const *model_matrix,
                                      uint32_t frame, uint32_t oldframe, cstring_t prefix,
                                      mdxAttachmentPosition_t *positions, uint32_t max_positions, bool models_only) {
    uint32_t count = 0;
    size_t const prefix_len = prefix ? strlen(prefix) : 0;
    mdxAttachment_t const *stack[32], **matches = stack; /* attachment pointers; common queries avoid allocation */
    mdxPoseMask_t mask = {0};

    if (!model || !model_matrix || !positions || !max_positions) {
        return 0;
    }

    FOR_EACH_LIST(mdxAttachment_t, attachment, model->attachments) {
        mdxNode_t const *node = &attachment->node;
        float visibility = 1.0f;

        if (models_only && !attachment->path[0]) continue;
        if (prefix_len && strncasecmp(node->name, prefix, prefix_len)) {
            continue;
        }
        if (attachment->Visibility) {
            MDLX_GetModelKeytrackValue(model, attachment->Visibility, frame, &visibility);
            if (visibility < EPSILON) {
                continue;
            }
        }
        if (count == sizeof(stack) / sizeof(*stack)) {
            size_t bytes = (size_t)max_positions * sizeof(*matches);
            matches = bytes / sizeof(*matches) == max_positions ? malloc(bytes) : NULL;
            if (!matches) { fprintf(stderr, "MDX: unable to allocate attachment query results\n"); Sys_Exit(EXIT_FAILURE); }
            memcpy(matches, stack, sizeof(stack));
        }
        matches[count++] = attachment;
        if (node->node_id < MDX_MAX_NODES && model->nodes[node->node_id])
            mask[node->node_id / 32] |= 1u << (node->node_id % 32);
        if (count == max_positions) break;
    }
    if (!count) return 0;
    MDLX_BindBoneSubset(model, model_matrix, frame, oldframe, mask);
    FOR_LOOP(i, count) {
        mdxAttachment_t const *attachment = matches[i];
        mdxNode_t const *node = &attachment->node;
        vec3_t pivot = {0}, local;
        if (node->node_id < (uint32_t)model->num_pivots) {
            pivot = model->pivots[node->node_id];
        }
        local = pivot;
        if (node->node_id < MDX_MAX_NODES && model->nodes[node->node_id]) {
            local = Matrix4_multiply_vector3(&node_matrices[node->node_id], &pivot);
            Matrix4_multiply(model_matrix, &node_matrices[node->node_id], &positions[i].transform);
        } else {
            positions[i].transform = *model_matrix;
        }
        positions[i].name = node->name;
        positions[i].path = attachment->path;
        positions[i].origin = Matrix4_multiply_vector3(model_matrix, &local);
    }
    if (matches != stack) free(matches);
    return count;
}

uint32_t MDLX_CollectAttachmentPositions(mdxModel_t const *model, mat4_t const *model_matrix,
                                      uint32_t frame, uint32_t oldframe, cstring_t prefix,
                                      mdxAttachmentPosition_t *positions, uint32_t max_positions) {
    return MDLX_CollectAttachments(model, model_matrix, frame, oldframe, prefix, positions, max_positions, false);
}

uint32_t MDLX_CollectAttachmentModels(mdxModel_t const *model, mat4_t const *model_matrix,
                                    uint32_t frame, uint32_t oldframe,
                                    mdxAttachmentPosition_t *positions, uint32_t max_positions) {
    return MDLX_CollectAttachments(model, model_matrix, frame, oldframe, NULL, positions, max_positions, true);
}
