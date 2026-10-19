#include "test.h"
#include "games/warcraft-3/renderer/mdx/r_mdx.h"
#include "renderer/r_game.h"

extern _Atomic uint32_t MDLX_TestPoseEvaluations, MDLX_TestNodeEvaluations;
void MDLX_TestResetPoseCache(void);

typedef struct {
    mdxNode_t nodes[4];
    mdxModel_t model;
    mdxSequence_t sequences[2];
    mdxGlobalSequence_t global;
    uint32_t tracks[3][sizeof(mdxKeyTrack_t) / 4 + 3 * 5];
    vec3_t pivots[128];
} poseFixture_t;

static void PoseFixture(poseFixture_t *f, bool billboard) {
    *f = (poseFixture_t){.sequences = {{.interval = {0, 1000}}, {.interval = {1000, 2000}}},
        .global = {.value = 700}, .pivots = {[3] = {1, 2, 3}, [51] = {4, 5, 6}}};
    f->model = (mdxModel_t){.num_nodes = 4, .num_sequences = 2, .sequences = f->sequences,
        .num_pivots = 128, .pivots = f->pivots, .num_globalSequences = 1, .globalSequences = &f->global};
    mdxKeyTrack_t *tracks[3];
    FOR_LOOP(t, 3) {
        mdxKeyTrack_t *track = tracks[t] = (mdxKeyTrack_t *)f->tracks[t];
        *track = (mdxKeyTrack_t){.keyframeCount = 3, .globalSeqId = t == 0 ? 0 : (uint32_t)-1,
            .datatype = t == 1 ? TDATA_FLOAT4 : TDATA_FLOAT3, .linetype = TRACK_LINEAR};
        uint32_t stride = 4 + (t == 1 ? 16 : 12);
        FOR_LOOP(i, 3) {
            mdxKeyFrame_t *key = (mdxKeyFrame_t *)((char *)track->values + i * stride);
            float values[4] = {i * 0.73f, i * -0.31f, i * 0.19f, 1}; key->time = i * 450;
            if (t == 1) { values[0] = values[1] = 0; values[2] = sinf(i * .17f); values[3] = cosf(i * .17f); }
            if (t == 2) values[0] = values[1] = values[2] = 1 + i * .13f;
            memcpy(key->data, values, stride - 4);
        }
    }
    uint32_t ids[] = {3, 51, 95, 127};
    FOR_LOOP(i, 4) {
        f->nodes[i] = (mdxNode_t){.node_id = ids[i], .parent_id = i ? ids[i - 1] : (uint32_t)-1,
            .flags = billboard && i == 1 ? MDLXNODE_Billboarded : 0,
            .translation = tracks[0], .rotation = tracks[1], .scale = tracks[2]};
        f->model.nodes[ids[i]] = f->model.node_list[i] = &f->nodes[i];
    }
}

static mat4_t PosePlacement(uint32_t actor) {
    mat4_t m; Matrix4_identity(&m);
    Matrix4_rotate(&m, &(vec3_t){0, 0, actor * 1.37f}, ROTATE_XYZ);
    Matrix4_translate(&m, &(vec3_t){actor * 13.7f, actor * -9.3f, actor * .5f});
    return m;
}

static void PoseCopy(poseFixture_t const *f, mat4_t out[4]) {
    FOR_LOOP(i, 4) out[i] = node_matrices[f->nodes[i].node_id];
}

TEST(renderer_model, mdx_prepared_poses_restore_exact_independent_actors_and_cache_hits) {
    poseFixture_t f; PoseFixture(&f, true);
    viewDef_t saved = tr.viewDef;
    tr.viewDef = (viewDef_t){.time = 437, .lerpfrac = .37f};
    mat4_t expected[64][4];
    FOR_LOOP(i, 64) {
        mat4_t m = PosePlacement(i);
        MDLX_BindBoneMatrices(&f.model, &m, 100 + i * 11, 50 + i * 11);
        PoseCopy(&f, expected[i]);
    }
    FOR_LOOP(w, 3) {
        MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(w + 1));
        FOR_LOOP(i, 64) {
            mat4_t m = PosePlacement(i);
            T_ASSERT(MDLX_QueuePose(&f.model, &m, 100 + i * 11, 50 + i * 11, NULL));
        }
        T_EQ(MDLX_TestPoseEvaluations, 0);
        MDLX_EndPosePreparation();
        T_EQ(MDLX_TestPoseEvaluations, 64); T_EQ(MDLX_TestNodeEvaluations, 64 * 4);
        mdxPoseStats_t stats = MDLX_PosePreparationStats();
        T_EQ(stats.jobs, 64); T_ASSERT(stats.worker_jobs >= (w + 1) * 4);
        FOR_LOOP(i, 64) {
            mat4_t m = PosePlacement(i), actual[4];
            MDLX_BindBoneMatrices(&f.model, &m, 100 + i * 11, 50 + i * 11);
            PoseCopy(&f, actual);
            T_EQ(memcmp(actual, expected[i], sizeof(actual)), 0);
        }
        T_EQ(MDLX_TestPoseEvaluations, 64); T_EQ(MDLX_TestNodeEvaluations, 64 * 4);
        T_EQ(MDLX_PosePreparationStats().hits, 64);
    }
    MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache(); tr.viewDef = saved;
}

static bool pose_flush;
static cstring_t PoseCvars(cstring_t name, cstring_t fallback) {
    if (!strcmp(name, "r_mdx_pose_workers")) return "2";
    if (!strcmp(name, "r_mdx_instancing")) return "0";
    if (!strcmp(name, "r_mdx_pose_flush")) return pose_flush ? "1" : "0";
    return fallback;
}

TEST(renderer_model, mdx_world_entry_submits_terrain_before_preparing_entity_poses_and_preserves_effect_ownership) {
    poseFixture_t f; PoseFixture(&f, true);
    viewDef_t saved = tr.viewDef; render_phase_t phase = tr.render_phase;
    cstring_t (*cvar)(cstring_t, cstring_t) = ri.CvarString;
    /* An emitter makes the real draw path request a complete pose even without geometry.
     * Preparing its bones must not execute the emitter or advance its accumulator. */
    mdxParticleEmitter_t emitter = {.node = f.nodes[0], .accumulator = .73f, .EmissionRate = 5,
        .LifeSpan = 1, .Rows = 1, .Columns = 1, .Alpha = {255, 255, 255},
        .ParticleScaling = {1, 1, 1}, .SegmentColor = {1,1,1, 1,1,1, 1,1,1}};
    emitter.node.flags |= MDLXNODE_ParticleEmitter;
    f.model.nodes[3] = f.model.node_list[0] = &emitter.node;
    f.model.emitters = &emitter;
    model_t model = {.modeltype = ID_MDLX, .mdx = &f.model};
    renderEntity_t entities[35] = {0};
    FOR_LOOP(i, 35) entities[i] = (renderEntity_t){.model = &model, .scale = 1,
        .angle = i * 1.37f, .origin = {i * 13.7f, i * -9.3f, i * .5f}, .frame = 100 + i * 11, .oldframe = 50 + i * 11};
    entities[32].flags = RF_HIDDEN; entities[33].model = NULL;
    entities[34].model = &model; /* This actor has the same exact pose as actor0 and must share its job. */
    entities[34] = entities[0];
    tr.viewDef = (viewDef_t){.time = 437, .lerpfrac = .37f, .deltaTime = 100, .rdflags = RDF_NOFRUSTUMCULL,
        .entities = entities, .num_entities = 35};
    tr.render_phase = RENDER_PHASE_SOLID; ri.CvarString = PoseCvars;
    extern uint32_t R_TestPoseFlushes;
    extern void R_TestResetParticles(void);
    extern uint32_t R_TestParticleCount(void);
    R_TestResetParticles();
    pose_flush = false; R_TestPoseFlushes = 0;
    MDLX_TestResetPoseCache();
    R_DrawWorld();
    extern uint32_t R_TestWorldPoseEvaluations;
    T_EQ(R_TestWorldPoseEvaluations, 0); /* Let GPU terrain work overlap CPU preparation. */
    T_EQ(R_TestPoseFlushes, 0);
    mdxPoseStats_t stats = MDLX_PosePreparationStats();
    T_EQ(stats.jobs, 32); T_ASSERT(stats.worker_jobs >= 8); T_EQ(stats.nodes, 32 * 4);
    T_FEQ(emitter.accumulator, .73f, .00001f); T_EQ(tr.render_phase, RENDER_PHASE_SOLID);
    T_EQ(MDLX_TestPoseEvaluations, 32);
    FOR_LOOP(i, 32) {
        mat4_t matrix; R_GetEntityMatrix(&entities[i], &matrix);
        MDLX_BindBoneMatrices(&f.model, &matrix, entities[i].frame, entities[i].oldframe);
    }
    T_EQ(MDLX_TestPoseEvaluations, 32); T_EQ(MDLX_PosePreparationStats().hits, 32);
    pose_flush = true; tr.viewDef.time++; MDLX_TestResetPoseCache();
    R_DrawWorld(); T_EQ(R_TestPoseFlushes, 1); T_EQ(MDLX_PosePreparationStats().jobs, 32);
    T_EQ(R_TestWorldPoseEvaluations, 0); T_FEQ(emitter.accumulator, .73f, .00001f);
    R_DrawWorld(); T_EQ(R_TestPoseFlushes, 1); T_EQ(MDLX_PosePreparationStats().jobs, 0);
    tr.viewDef.num_entities = 2; tr.viewDef.time++; MDLX_TestResetPoseCache();
    R_DrawWorld(); T_EQ(R_TestPoseFlushes, 1); T_EQ(MDLX_PosePreparationStats().jobs, 0);
    T_EQ(R_TestParticleCount(), 0);
    /* The real owner must still be able to emit; a zero-rate fixture could hide accidental dispatch. */
    mat4_t matrix; R_GetEntityMatrix(&entities[0], &matrix);
    MDLX_BindBoneMatrices(&f.model, &matrix, entities[0].frame, entities[0].oldframe);
    MDLX_RenderParticleEmitters(&entities[0], &f.model, &matrix);
    T_EQ(R_TestParticleCount(), 1); T_FEQ(emitter.accumulator, .23f, .00001f);
    pose_flush = false;
    MDLX_EndModelBatch(); MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache();
    ri.CvarString = cvar; tr.viewDef = saved; tr.render_phase = phase;
}

TEST(renderer_model, mdx_preparation_merges_duplicate_subsets_and_skips_already_complete_poses) {
    poseFixture_t f; PoseFixture(&f, false);
    viewDef_t saved = tr.viewDef;
    tr.viewDef = (viewDef_t){.time = 500, .lerpfrac = .61f};
    mdxPoseMask_t mask = {[0] = 1u << 3};
    MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(2));
    FOR_LOOP(i, 32) {
        mat4_t m = PosePlacement(i);
        T_ASSERT(MDLX_QueuePose(&f.model, &m, 150 + i * 7, 100 + i * 7, mask));
        T_ASSERT(MDLX_QueuePose(&f.model, &m, 150 + i * 7, 100 + i * 7, NULL));
    }
    MDLX_EndPosePreparation(); T_EQ(MDLX_TestPoseEvaluations, 32); T_EQ(MDLX_TestNodeEvaluations, 128);
    T_ASSERT(MDLX_BeginPosePreparation(1));
    FOR_LOOP(i, 32) {
        mat4_t m = PosePlacement(i);
        T_ASSERT(MDLX_QueuePose(&f.model, &m, 150 + i * 7, 100 + i * 7, NULL));
    }
    MDLX_EndPosePreparation(); T_EQ(MDLX_TestPoseEvaluations, 32); T_EQ(MDLX_TestNodeEvaluations, 128);
    MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache(); tr.viewDef = saved;
}

TEST(renderer_model, mdx_preparation_cancels_stale_inputs_release_and_small_or_disabled_batches) {
    poseFixture_t f; PoseFixture(&f, true);
    viewDef_t saved = tr.viewDef;
    tr.viewDef = (viewDef_t){.time = 500, .lerpfrac = .61f};
    mat4_t m = PosePlacement(1);
    MDLX_TestResetPoseCache(); T_ASSERT(!MDLX_BeginPosePreparation(0));
    T_ASSERT(MDLX_BeginPosePreparation(1));
    T_ASSERT(MDLX_QueuePose(&f.model, &m, 150, 100, NULL));
    MDLX_EndPosePreparation(); T_EQ(MDLX_TestPoseEvaluations, 0);
    MDLX_BindBoneMatrices(&f.model, &m, 150, 100); T_EQ(MDLX_TestPoseEvaluations, 1);
    FOR_LOOP(change, 4) {
        MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(1));
        FOR_LOOP(i, 32) {
            m = PosePlacement(i);
            T_ASSERT(MDLX_QueuePose(&f.model, &m, 150 + i * 7, 100 + i * 7, NULL));
        }
        if (change == 0) tr.viewDef.time++;
        if (change == 1) tr.viewDef.lerpfrac = .41f;
        if (change == 2) tr.viewDef.viewProjectionMatrix.v[0]++;
        if (change == 3) MDLX_ClearPoseCache(); /* Same invalidation used before a model is freed. */
        MDLX_EndPosePreparation(); T_EQ(MDLX_TestPoseEvaluations, 0);
    }
    MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache(); tr.viewDef = saved;
}

TEST(renderer_model, mdx_preparation_capacity_keeps_excess_actors_correct_on_demand) {
    poseFixture_t f; PoseFixture(&f, true);
    viewDef_t saved = tr.viewDef;
    tr.viewDef = (viewDef_t){.time = 571, .lerpfrac = .43f};
    mat4_t expected[544][4];
    FOR_LOOP(i, 544) {
        mat4_t m = PosePlacement(i);
        MDLX_BindBoneMatrices(&f.model, &m, 100 + i * 3, 50 + i * 3);
        PoseCopy(&f, expected[i]);
    }
    MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(99));
    FOR_LOOP(i, 544) {
        mat4_t m = PosePlacement(i);
        T_EQ(MDLX_QueuePose(&f.model, &m, 100 + i * 3, 50 + i * 3, NULL), i < 512);
    }
    MDLX_EndPosePreparation(); T_EQ(MDLX_PosePreparationStats().jobs, 512);
    FOR_LOOP(i, 544) {
        mat4_t m = PosePlacement(i), actual[4];
        MDLX_BindBoneMatrices(&f.model, &m, 100 + i * 3, 50 + i * 3);
        PoseCopy(&f, actual); T_EQ(memcmp(actual, expected[i], sizeof(actual)), 0);
    }
    T_EQ(MDLX_TestPoseEvaluations, 544); T_EQ(MDLX_PosePreparationStats().hits, 512);
    MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache(); tr.viewDef = saved;
}

TEST(renderer_model, mdx_preparation_job_indices_are_owned_by_the_current_batch_and_release_cancels_work) {
    poseFixture_t f; PoseFixture(&f, false);
    viewDef_t saved = tr.viewDef;
    tr.viewDef = (viewDef_t){.time = 571, .lerpfrac = .43f};
    mdxPoseMask_t root = {[0] = 1u << 3};
    MDLX_TestResetPoseCache();
    FOR_LOOP(pass, 2) {
        T_ASSERT(MDLX_BeginPosePreparation(2));
        FOR_LOOP(i, 32) {
            uint32_t actor = pass ? 31 - i : i;
            mat4_t m = PosePlacement(actor);
            T_ASSERT(MDLX_QueuePose(&f.model, &m, 100 + actor * 3, 50 + actor * 3, pass ? NULL : root));
        }
        MDLX_EndPosePreparation(); T_EQ(MDLX_PosePreparationStats().jobs, 32);
    }
    T_EQ(MDLX_TestPoseEvaluations, 64); T_EQ(MDLX_TestNodeEvaluations, 128);
    MDLX_TestResetPoseCache();
    mdxNode_t node = {.parent_id = (uint32_t)-1};
    mdxModel_t model = {.num_nodes = 1, .nodes = {&node}, .node_list = {&node}};
    T_ASSERT(MDLX_BeginPosePreparation(1));
    FOR_LOOP(i, 32) {
        mat4_t m = PosePlacement(i);
        T_ASSERT(MDLX_QueuePose(&model, &m, 100 + i, 50 + i, NULL));
    }
    MDLX_Release(&model); /* Drive the actual owner before making its old pointer unusable. */
    model.node_list[0] = NULL;
    MDLX_EndPosePreparation(); T_EQ(MDLX_TestPoseEvaluations, 0);
    T_EQ(MDLX_PosePreparationStats().jobs, 0);
    tr.viewDef.time = 0; T_ASSERT(!MDLX_BeginPosePreparation(1));
    MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache(); tr.viewDef = saved;
}

/* Held actors share authored frames but retain distinct global clocks in cached and worker poses. */
TEST(renderer_model, mdx_prepared_poses_preserve_each_entities_held_global_clock) {
    poseFixture_t f; PoseFixture(&f, false);
    viewDef_t saved = tr.viewDef;
    renderEntity_t actors[32];
    mat4_t matrix; Matrix4_identity(&matrix);
    mdxParticleEmitter_t emitter = {0}; f.model.emitters = &emitter;
    FOR_LOOP(i, 4) f.nodes[i].rotation = f.nodes[i].scale = NULL;
    tr.viewDef = (viewDef_t){.time = 1, .rdflags = RDF_NOFRUSTUMCULL};
    FOR_LOOP(i, 32) {
        actors[i] = (renderEntity_t){.number = MAX_GAME_ENTITIES - 100 - i, .generation = 0x574335u,
            .flags = RF_MDX_ANIMATION_PAUSED, .frame = 100, .oldframe = 100, .scale = 1};
        tr.viewDef.time = 100 + i * 5;
        MDLX_UpdateEntityAnimationPause(&actors[i]);
    }
    tr.viewDef.time = 400;
    MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(2));
    FOR_LOOP(i, 32) MDLX_QueueEntityPose(&actors[i], &f.model, &matrix);
    MDLX_EndPosePreparation(); T_EQ(MDLX_PosePreparationStats().jobs, 32);
    T_ASSERT(MDLX_PosePreparationStats().worker_jobs >= 8);
    FOR_LOOP(pass, 2) {
        FOR_LOOP(i, 32) {
            mdxAnimationTimeScope_t scope;
            MDLX_PushEntityAnimationTime(&actors[i], &scope);
            MDLX_BindBoneMatrices(&f.model, &matrix, 100, 100);
            T_FEQ(node_matrices[3].v[12], .73f * (100 + i * 5) / 450, .00001f);
            MDLX_PopEntityAnimationTime(&scope);
        }
        T_EQ(MDLX_TestPoseEvaluations, 32);
    }
    actors[0].flags = 0;
    tr.viewDef.time = 500; MDLX_UpdateEntityAnimationPause(&actors[0]);
    tr.viewDef.time = 550;
    mdxAnimationTimeScope_t scope; MDLX_PushEntityAnimationTime(&actors[0], &scope);
    MDLX_BindBoneMatrices(&f.model, &matrix, 100, 100);
    T_FEQ(node_matrices[3].v[12], .73f * 150 / 450, .00001f);
    MDLX_PopEntityAnimationTime(&scope);
    MDLX_ShutdownPoseWorkers(); MDLX_TestResetPoseCache(); tr.viewDef = saved;
}
