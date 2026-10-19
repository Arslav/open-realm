#include "test.h"
#include <stdarg.h>
#include "games/warcraft-3/renderer/mdx/r_mdx.h"
#include "games/warcraft-3/renderer/w3m/r_war3map.h"

void MDLX_TestResetPoseCache(void);
static uint32_t poses, draws, emits;
static _Atomic uint32_t alpha_reads, color_reads, worker_reads;
static SDL_threadID sample_owner;
static cstring_t packet_mode = "2";
static bool packet_uniform = true;
typedef struct {
    uint32_t copies, bones, rows;
    float alpha;
    vec4_t color;
    mat3_t uv;
    mat4_t palette[BZ_BONE_PALETTE_MAX];
    vec4_t texels[1024];
} packetDraw_t;
static packetDraw_t packet_draws[16];
static uint32_t packet_draw_count;
static bool record_packets;
uint32_t GetModelKeyTrackDataTypeSize(MODELKEYTRACKDATATYPE type);
static cstring_t submit_cache_value = "2";
static bool submit_uniform;
static cstring_t block_mode = "2";
static struct {
    GLint limit, bytes;
    uint32_t binds, uploads, deletes, bound, counts[16];
    mat4_t bones[1024], palettes[16][1024], matrices[16][256];
} block;
static modelProg_t shader;
static mat4_t drawn_bone;
static vec4_t drawn_color;
void R_TestObserveGeosetDraw(void);
static struct {
    uint32_t draws, total, uploads, counts[16], divisors[4], divisor_calls;
    mat4_t matrices[256];
    uint32_t palettes, allocations, attrib_calls, bound_vao, bound_array, prepared[2], prepared_count;
    uint32_t upload_buffers[16], draw_vaos[16];
    vec4_t bones[16384];
    uint32_t bone_rows;
    mat4_t uniform_bones[BZ_BONE_PALETTE_MAX];
    uint32_t uniform_count;
    bool uniform_shader, numeric_highp, uniform_draws[16];
} inst;

/* Count the real pose evaluator; geometry, animation and emitter dispatch stay production code. */
static void test_pose(mdxModel_t const *model, mat4_t const *matrix, uint32_t frame, uint32_t old) {
    poses++;
    MDLX_BindBoneMatrices(model, matrix, frame, old);
}
static void test_emit(renderEntity_t const *ent, mdxModel_t const *model, mat4_t const *matrix) {
    emits++;
    MDLX_RenderParticleEmitters(ent, model, matrix);
}
static void test_track(mdxModel_t const *model, mdxKeyTrack_t const *track, uint32_t frame, void *out) {
    alpha_reads++;
    if (sample_owner && SDL_ThreadID() != sample_owner) worker_reads++;
    MDLX_GetModelKeytrackValue(model, track, frame, out);
}
static void test_color(mdxModel_t const *model, mdxKeyTrack_t const *track, uint32_t frame, vec3_t *out) {
    color_reads++;
    MDLX_GetAnimatedColorTrackValue(model, track, frame, out);
}
static void test_draw(GLenum mode, GLsizei count, GLenum type, void const *offset) {
    (void)mode; (void)count; (void)type; (void)offset;
    draws++; drawn_bone = mdlx.shader->state.bones[0]; drawn_color = mdlx.shader->state.geosetColor;
    if (record_packets) {
        T_ASSERT(packet_draw_count < 16);
        packetDraw_t *draw = &packet_draws[packet_draw_count++];
        *draw = (packetDraw_t){.copies = inst.counts[inst.draws - 1], .bones = mdlx.shader->state.boneCount,
            .alpha = mdlx.shader->state.layerAlpha, .color = drawn_color, .uv = mdlx.shader->state.uvMatrix};
        if (inst.uniform_draws[inst.draws - 1]) memcpy(draw->palette, mdlx.shader->state.bones, draw->bones * sizeof(mat4_t));
        else {
            draw->rows = inst.bone_rows; T_EQ(draw->rows, 1);
            memcpy(draw->texels, inst.bones, sizeof(draw->texels));
        }
    }
    R_TestObserveGeosetDraw();
}
static void test_draw_instances(GLenum mode, GLsizei count, GLenum type, void const *offset, GLsizei copies) {
    T_ASSERT(inst.draws < 16);
    FOR_LOOP(i, 4) T_EQ(inst.divisors[i], 1);
    inst.uniform_draws[inst.draws] = !strcmp(mdlx.shader->prog.desc->Name, "mdx_uniform_instances");
    inst.draw_vaos[inst.draws] = inst.bound_vao;
    inst.counts[inst.draws++] = copies; inst.total += copies;
    inst.uniform_count = mdlx.shader->state.boneCount;
    if (!strcmp(mdlx.shader->prog.desc->Name, "mdx_block_instances")) {
        T_ASSERT(block.bound); T_ASSERT(inst.uniform_count <= 1024);
        memcpy(block.palettes[inst.draws - 1], block.bones, inst.uniform_count * sizeof(mat4_t));
        memcpy(block.matrices[inst.draws - 1], inst.matrices, copies * sizeof(mat4_t));
        block.counts[inst.draws - 1] = inst.uniform_count;
    } else T_ASSERT(inst.uniform_count <= BZ_BONE_PALETTE_MAX);
    memcpy(inst.uniform_bones, mdlx.shader->state.bones, sizeof(inst.uniform_bones));
    test_draw(mode, count, type, offset);
}
static void test_instance_upload(GLenum target, GLsizeiptr size, void const *data, GLenum usage) {
    if (target == GL_UNIFORM_BUFFER) {
        T_EQ(usage, GL_STREAM_DRAW); T_EQ(size, block.bytes); T_NULL(data); return;
    }
    T_EQ(target, GL_ARRAY_BUFFER); T_EQ(usage, GL_STREAM_DRAW);
    T_ASSERT(size > 0 && size <= sizeof(inst.matrices) && !(size % sizeof(mat4_t)));
    T_ASSERT(inst.uploads < 16); inst.upload_buffers[inst.uploads] = inst.bound_array;
    memcpy(inst.matrices, data, size); inst.uploads++;
}
static void test_instance_divisor(GLuint index, GLuint value) {
    T_ASSERT(index >= attrib_instance && index < attrib_instance + 4);
    inst.divisors[index - attrib_instance] = value; inst.divisor_calls++;
}
/* Prepared IDs model immutable VAO state; the loader suite verifies how those records are actually built. */
static void test_instance_bindvao(GLuint vao) {
    inst.bound_vao = vao;
    FOR_LOOP(i, 4) inst.divisors[i] = 0;
    FOR_LOOP(i, inst.prepared_count) if (inst.prepared[i] == vao)
        FOR_LOOP(j, 4) inst.divisors[j] = 1;
}
static void test_instance_bindbuf(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) inst.bound_array = buffer;
}
static void test_instance_attr(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, void const *ptr) {
    (void)size; (void)type; (void)norm; (void)stride; (void)ptr;
    if (index >= attrib_instance && index < attrib_instance + 4) inst.attrib_calls++;
}
static void test_instance_gen(GLsizei count, GLuint *ids) { while (count--) *ids++ = 79; }
static void test_instance_delete(GLsizei count, GLuint const *ids) { block.deletes += count; (void)ids; }
static void test_block_upload(GLenum target, GLintptr offset, GLsizeiptr size, void const *data) {
    T_EQ(target, GL_UNIFORM_BUFFER); T_EQ(offset, 0); T_ASSERT(size > 0 && size <= block.bytes);
    memcpy(block.bones, data, size); block.uploads++;
}
static void test_block_bind(GLenum target, GLuint index, GLuint id) {
    T_EQ(target, GL_UNIFORM_BUFFER); T_EQ(index, 0); block.bound = id; block.binds++;
}
static GLuint test_block_index(GLuint prog, GLchar const *name) {
    T_EQ(prog, 91); T_STREQ(name, "MDXBoneBlock"); return 3;
}
static void test_block_size(GLuint prog, GLuint index, GLenum name, GLint *value) {
    T_EQ(prog, 91); T_EQ(index, 3); T_EQ(name, GL_UNIFORM_BLOCK_DATA_SIZE); *value = block.bytes;
}
static void test_block_point(GLuint prog, GLuint index, GLuint point) {
    T_EQ(prog, 91); T_EQ(index, 3); T_EQ(point, 0);
}
static void test_palette_upload(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void const *data) {
    T_EQ(target, GL_TEXTURE_2D); T_EQ(level, 0); T_EQ(x, 0); T_EQ(y, 0);
    T_EQ(fmt, GL_RGBA); T_EQ(type, GL_FLOAT);
    T_ASSERT(w > 0 && h > 0 && (uint64_t)w * h * 4 * sizeof(float) <= sizeof(inst.bones));
    memcpy(inst.bones, data, (size_t)w * h * 4 * sizeof(float)); inst.palettes++; inst.bone_rows = h;
}
static void test_instance_limits(GLenum name, GLint *value) {
    switch (name) {
    case GL_MAX_TEXTURE_SIZE: *value = 2048; break;
    case GL_MAX_UNIFORM_BLOCK_SIZE: *value = block.limit; break;
    case GL_MAX_VERTEX_UNIFORM_BLOCKS: case GL_MAX_UNIFORM_BUFFER_BINDINGS: *value = 12; break;
    default: T_ASSERT(false);
    }
}
static void test_palette_storage(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                                 GLint border, GLenum format, GLenum type, void const *data) {
    T_EQ(target, GL_TEXTURE_2D); T_EQ(level, 0); T_EQ(internal, GL_RGBA32F); T_EQ(border, 0);
    T_EQ(format, GL_RGBA); T_EQ(type, GL_FLOAT); T_NULL(data); T_ASSERT(w > 0 && h > 0);
    inst.allocations++;
}
bool R_TestBoneSamplerPrecision(shaderLoad_t const *load);
static void test_instance_shader(shaderLoad_t const *load) {
    inst.numeric_highp = R_TestBoneSamplerPrecision(load);
    T_ASSERT(load->state_size > sizeof(modelState_t));
    inst.uniform_shader = !strcmp(load->desc->Name, "mdx_uniform_instances");
    bool ubo = !strcmp(load->desc->Name, "mdx_block_instances");
    T_ASSERT(inst.uniform_shader || ubo || !strcmp(load->desc->Name, "mdx_instances"));
    if (ubo) {
        unsigned count = 0;
        char const *array = strstr(load->desc->VertexBody, "u_instanceBones["); T_NOT_NULL(array);
        T_EQ(sscanf(array, "u_instanceBones[%u]", &count), 1);
        block.bytes = count * sizeof(mat4_t); T_ASSERT(block.bytes <= block.limit);
        FOR_LOOP(i, MAX_SHADER_UNIFORMS) if (load->desc->Uniforms[i].name)
            T_ASSERT(strcmp(load->desc->Uniforms[i].name, "u_bones"));
        T_NOT_NULL(strstr(load->desc->VertexBody, "layout(std140, column_major) uniform MDXBoneBlock"));
        T_ASSERT(strstr(load->desc->VertexBody, sd_model.VertexBody));
        load->prog->progid = 91;
    }
    T_ASSERT(strstr(load->defines, "BZ_USE_INSTANCING"));
    load->prog->desc = load->desc; load->prog->state_size = load->state_size;
}

static uint32_t batch_ticks;
static char batch_log[8192];
static uint32_t test_batch_ticks(void) { return batch_ticks; }
static int test_batch_log(FILE *stream, char const *format, ...) {
    va_list args; va_start(args, format);
    int n = !strncmp(format, "[MDX_BATCH]", 11)
        ? vsnprintf(batch_log + strlen(batch_log), sizeof(batch_log) - strlen(batch_log), format, args)
        : vfprintf(stream, format, args);
    va_end(args); return n;
}

/* This suite drives MDX_RenderModel, replacing only GPU submission and the map texture lookup. */
#undef R_Call
#define R_Call(func, ...) do { \
    if (!strcmp(#func, "glDrawElements") || !strcmp(#func, "glDrawElementsInstanced") || \
        !strcmp(#func, "glBufferData") || !strcmp(#func, "glBufferSubData") || \
        !strcmp(#func, "glBindBufferBase") || !strcmp(#func, "glUniformBlockBinding") || \
        !strcmp(#func, "glGetActiveUniformBlockiv") || !strcmp(#func, "glGenBuffers") || \
        !strcmp(#func, "glDeleteBuffers") || !strcmp(#func, "glTexSubImage2D") || !strcmp(#func, "glTexImage2D") || \
        !strcmp(#func, "glGenTextures") || !strcmp(#func, "glDeleteTextures") || \
        !strcmp(#func, "glGetIntegerv") || !strcmp(#func, "glBindVertexArray") || \
        !strcmp(#func, "glBindBuffer") || !strcmp(#func, "glVertexAttribPointer") || \
        !strcmp(#func, "glVertexAttribDivisor")) func(__VA_ARGS__); \
} while (0)
#undef R_ApplyShader
#define R_ApplyShader(p) ((void)(p))
#define R_SetAlphaKeyState(enabled) ((void)(enabled))
#define glDrawElements test_draw
#define glDrawElementsInstanced test_draw_instances
#define glBufferData test_instance_upload
#define glBufferSubData test_block_upload
#define glBindBufferBase test_block_bind
#define glGetUniformBlockIndex test_block_index
#define glGetActiveUniformBlockiv test_block_size
#define glUniformBlockBinding test_block_point
#define glGenBuffers test_instance_gen
#define glDeleteBuffers test_instance_delete
#define glBindVertexArray test_instance_bindvao
#define glBindBuffer test_instance_bindbuf
#define glVertexAttribPointer test_instance_attr
#define glVertexAttribDivisor test_instance_divisor
#define glTexSubImage2D test_palette_upload
#define glTexImage2D test_palette_storage
#define glGenTextures test_instance_gen
#define glDeleteTextures test_instance_delete
#define glGetIntegerv test_instance_limits
#define R_LoadShaderState test_instance_shader
#define R_DeleteShader(p) memset((p), 0, sizeof(*(p)))
#define R_BindTexture(tex, unit) ((void)(tex), (void)(unit))
#define R_W3CliffReplacementAt(x, y) ((texture_t const *)NULL)
#define R_GetFogOfWarTexture() 13 // texture ID; isolated FOW handle for the headless draw
#define MDLX_BindBoneMatrices test_pose
#define MDLX_RenderParticleEmitters test_emit
#define MDLX_GetModelKeytrackValue test_track
#define MDLX_GetAnimatedColorTrackValue test_color
#define MDLX_GetTexture R_TestGeosetTexture
#define MDX_RenderModel R_TestGeosetModel
void R_TestGeosetModel(renderEntity_t const *entity, mdxModel_t const *model, mat4_t const *matrix);
#define SDL_GetTicks test_batch_ticks
#define fprintf test_batch_log
#include "games/warcraft-3/renderer/mdx/r_mdx_geoset.c"
#undef fprintf
#undef SDL_GetTicks

typedef struct {
    mdxModel_t model;
    mdxNode_t node;
    vec3_t pivot;
    mdxGeoset_t geo;
    mdxMaterial_t mat;
    mdxMaterialLayer_t layers[2];
    mdxTexture_t tex;
    uint32_t vao, inst_vao;
    int palette;
    renderEntity_t ent;
    mat4_t matrix;
} geosetFixture_t;

static cstring_t instance_defaults(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_effect_instancing") ? "1" : fallback;
}

static void setup_geoset(geosetFixture_t *f, BLEND_MODE mode) {
    MDLX_ShutdownModelBatches();
    memset(&inst, 0, sizeof(inst));
    memset(&batch_stats, 0, sizeof(batch_stats)); ri.CvarString = instance_defaults;
    *f = (geosetFixture_t){ .node.parent_id = (uint32_t)-1, .vao = 1, .ent.scale = 1 };
    f->model = (mdxModel_t){ .geosets = &f->geo, .materials = &f->mat, .textures = &f->tex,
        .num_textures = 1, .nodes = {&f->node}, .node_list = {&f->node}, .num_nodes = 1,
        .pivots = &f->pivot, .num_pivots = 1 };
    f->geo = (mdxGeoset_t){ .vaos = &f->vao, .vaos_count = 1, .num_triangles = 3,
        .matrixPalette = &f->palette, .num_matrixPalette = 1 };
    f->mat = (mdxMaterial_t){ .layers = f->layers, .num_layers = 1 };
    f->layers[0] = (mdxMaterialLayer_t){ .blendMode = mode, .staticAlpha = 1, .transformId = (uint32_t)-1 };
    f->layers[1] = f->layers[0];
    Matrix4_identity(&f->matrix);
    shader = (modelProg_t){0}; mdlx.shader = &shader;
    tr.viewDef = (viewDef_t){ .rdflags = RDF_NOFRUSTUMCULL, .lerpfrac = 1 };
}


/* Preparation must retain the draw path's sparse masks, including both material phases. */
TEST(mdx_geoset, prepared_geometry_unions_phases_without_evaluating_hidden_or_unused_nodes) {
    extern _Atomic uint32_t MDLX_TestNodeEvaluations;
    extern void MDLX_TestResetPoseCache(void);
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
    mdxNode_t nodes[] = {{.node_id = 3, .parent_id = 0}, {.node_id = 9, .parent_id = 0},
        {.node_id = 10, .parent_id = 0, .flags = MDLXNODE_Billboarded}};
    FOR_LOOP(i, 3) f.model.nodes[nodes[i].node_id] = f.model.node_list[i + 1] = &nodes[i];
    f.model.num_nodes = 4;
    int palettes[] = {3, 9, 10};
    mdxMaterialLayer_t alpha = f.layers[0]; alpha.blendMode = BLEND_MODE_BLEND;
    mdxMaterial_t material = {.layers = &alpha, .num_layers = 1}; f.mat.next = &material;
    mdxGeosetAnim_t hidden = {.staticAlpha = 0};
    mdxGeoset_t next = f.geo, last = f.geo;
    next.materialID = 1; next.matrixPalette = &palettes[1]; next.next = &last;
    last.matrixPalette = &palettes[2]; last.geosetAnim = &hidden;
    f.geo.next = &next; f.geo.matrixPalette = &palettes[0];
    tr.viewDef.time = 170; tr.render_phase = RENDER_PHASE_SOLID;
    MDLX_TestResetPoseCache(); MDLX_BeginModelBatch(); T_ASSERT(MDLX_BeginPosePreparation(2));
    poses = draws = emits = 0;
    FOR_LOOP(i, 32) {
        f.ent.frame = 100 + i; f.ent.oldframe = 50 + i; f.matrix.v[12] = i * 13.7f;
        MDLX_QueueEntityPose(&f.ent, &f.model, &f.matrix);
    }
    MDLX_EndPosePreparation();
    T_EQ(MDLX_PosePreparationStats().jobs, 32); T_EQ(MDLX_TestNodeEvaluations, 96);
    T_EQ(draws, 0); T_EQ(emits, 0); T_EQ(tr.render_phase, RENDER_PHASE_SOLID);
    FOR_LOOP(p, 2) {
        tr.render_phase = p ? RENDER_PHASE_ALPHA : RENDER_PHASE_SOLID;
        if (p) MDLX_BeginModelBatch();
        FOR_LOOP(i, 32) {
            f.ent.frame = 100 + i; f.ent.oldframe = 50 + i; f.matrix.v[12] = i * 13.7f;
            MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        }
        MDLX_EndModelBatch();
    }
    T_EQ(MDLX_TestNodeEvaluations, 96); T_EQ(MDLX_PosePreparationStats().hits, 64);
    T_EQ(node_matrices[10].v[15], 0); T_EQ(inst.total, 64);
    MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
}

static cstring_t instance_auto(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_uniform_auto") || !strcmp(name, "r_mdx_stats") ? "1" : fallback;
}

static cstring_t block_settings(cstring_t name, cstring_t fallback) {
    if (!strcmp(name, "r_mdx_palette_ubo")) return block_mode;
    if (!strcmp(name, "r_mdx_uniform_instancing")) return "1";
    if (!strcmp(name, "r_mdx_packet_prepare")) return "2";
    return fallback;
}

/* A 65-bone geoset currently forces singletons; a block must join complete independent actor palettes. */
TEST(mdx_geoset, uniform_block_packets_use_queried_capacity_and_preserve_sparse_palettes) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = block_settings;
    poses = draws = emits = 0;
    block_mode = "2"; block = (typeof(block)){.limit = 16384}; tr.render_phase = RENDER_PHASE_SOLID;
    int palette[65]; FOR_LOOP(i, 65) palette[i] = i == 64 ? -1 : 999;
    f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 65;
    f.node.node_id = 999; f.model.nodes[0] = NULL; f.model.nodes[999] = &f.node;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 2 * (sizeof(int) + sizeof(vec3_t))] = {0};
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){.keyframeCount = 2, .datatype = TDATA_FLOAT3, .linetype = TRACK_LINEAR,
        .globalSeqId = (uint32_t)-1};
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (void *)((uint8_t *)track->values + i * (sizeof(int) + sizeof(vec3_t)));
        vec3_t value = {17.13f * i, -23.71f * i, 31.3f * i};
        key->time = i * 500; memcpy(key->data, &value, sizeof(value));
    }
    mdxSequence_t seq = {.interval = {0, 1000}};
    f.node.translation = track; f.model.sequences = &seq; f.model.num_sequences = 1;
    mat4_t transforms[7]; MDLX_BeginModelBatch();
    FOR_LOOP(i, 7) {
        f.ent.frame = 80 + i * 40; f.ent.oldframe = 40 + i * 40; f.matrix.v[12] = i * 53.7f;
        transforms[i] = f.matrix;
        MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    }
    MDLX_EndModelBatch();
#ifdef BZ_GLSL_120
    T_EQ(draws, 7); T_EQ(inst.draws, 0); T_EQ(block.uploads, 0);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL; return;
#endif
    T_EQ(draws, 3); T_EQ(inst.draws, 2); T_EQ(inst.total, 6); T_EQ(inst.counts[0], 3); T_EQ(inst.counts[1], 3);
    T_EQ(block.uploads, 2); T_EQ(block.bound, 0); T_EQ(block.counts[0], 195); T_EQ(inst.palettes, 0);
    FOR_LOOP(p, 2) FOR_LOOP(i, 3) {
        uint32_t actor = p * 3 + i; f.matrix.v[12] = actor * 53.7f;
        MDLX_BindBoneMatrices(&f.model, &f.matrix, 80 + actor * 40, 40 + actor * 40);
        T_EQ(memcmp(block.palettes[p] + i * 65, node_matrices + 999, sizeof(mat4_t)), 0);
        mat4_t identity; Matrix4_identity(&identity);
        T_EQ(memcmp(block.palettes[p] + i * 65 + 64, &identity, sizeof(identity)), 0);
        transforms[actor].v[15] = i + 1;
        T_EQ(memcmp(&block.matrices[p][i], transforms + actor, sizeof(mat4_t)), 0);
    }
    uint32_t deleted = block.deletes; MDLX_ShutdownModelBatches(); T_EQ(block.deletes - deleted, 2);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, uniform_block_toggle_preserves_order_material_splits_and_original_path) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_BLEND); ri.CvarString = block_settings;
    poses = draws = emits = 0;
    block = (typeof(block)){.limit = 65536}; tr.render_phase = RENDER_PHASE_ALPHA;
    int palette[40] = {0}; f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 40;
    FOR_LOOP(mode, 3) {
        block_mode = mode == 0 ? "2" : mode == 1 ? "1" : "0";
        uint32_t before = inst.draws, uploads = block.uploads;
        MDLX_BeginModelBatch();
        FOR_LOOP(team, 2) {
            f.ent.team = team;
            FOR_LOOP(i, 7) { f.matrix.v[12] = team * 100 + i; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
        }
        MDLX_EndModelBatch();
        bool narrow = mode != 0, ordinary = mode == 2;
#ifdef BZ_GLSL_120
        narrow = ordinary = true;
#endif
        T_EQ(inst.draws - before, narrow ? 4 : 2);
        T_EQ(block.uploads - uploads, ordinary ? 0 : narrow ? 4 : 2);
        T_EQ(inst.counts[before], narrow ? 3 : 7); T_EQ(block.bound, 0);
        T_ASSERT(mdlx.shader == &shader);
    }
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, automatic_uniforms_keep_complete_packets_at_the_palette_boundary) {
    geosetFixture_t f;
    FOR_LOOP(mode, 3) {
        setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = instance_auto; tr.render_phase = RENDER_PHASE_SOLID;
        uint32_t count = mode == 2 ? 2 : 128 + mode;
        if (mode == 2) f.geo.num_matrixPalette = 0;
        MDLX_BeginModelBatch();
        FOR_LOOP(i, count) { f.matrix.v[12] = i; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
        MDLX_EndModelBatch();
        T_EQ(inst.draws, 1); T_EQ(inst.total, count); T_EQ(inst.counts[0], count);
        T_EQ(inst.uniform_draws[0], mode == 0); T_EQ(inst.palettes, mode == 0 ? 0 : 1);
        if (!mode) T_EQ(inst.uniform_count, 128);
        MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
    }
}

TEST(mdx_geoset, automatic_uniforms_check_every_geoset_before_selecting_a_packet_shader) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = instance_auto;
    tr.render_phase = RENDER_PHASE_SOLID;
    int palette[65] = {0}; mdxGeoset_t second = f.geo;
    second.matrixPalette = palette; second.num_matrixPalette = 65; f.geo.next = &second;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 2) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch();
    T_EQ(inst.draws, 2); T_EQ(inst.total, 4); T_EQ(inst.palettes, 1);
    FOR_LOOP(i, 2) T_ASSERT(!inst.uniform_draws[i]);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, automatic_uniforms_restore_transport_between_small_and_large_ordered_packets) {
    geosetFixture_t f;
    FOR_LOOP(alpha, 2) {
        setup_geoset(&f, alpha ? BLEND_MODE_BLEND : BLEND_MODE_NONE); ri.CvarString = instance_auto;
        tr.render_phase = alpha ? RENDER_PHASE_ALPHA : RENDER_PHASE_SOLID;
        batch_ticks = 100; batch_log[0] = 0; MDLX_BeginModelBatch();
        FOR_LOOP(packet, 3) {
            f.ent.team = packet;
            FOR_LOOP(i, packet == 1 ? 129 : 2) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        }
        batch_ticks = 1200; MDLX_EndModelBatch();
        T_EQ(inst.draws, 3); T_EQ(inst.total, 133); T_EQ(inst.palettes, 1);
        T_EQ(inst.counts[0], 2); T_EQ(inst.counts[1], 129); T_EQ(inst.counts[2], 2);
        T_ASSERT(inst.uniform_draws[0]); T_ASSERT(!inst.uniform_draws[1]); T_ASSERT(inst.uniform_draws[2]);
        T_NOT_NULL(strstr(batch_log, "uniform_packets=2 palette_uploads=1"));
        MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
    }
}

TEST(mdx_geoset, automatic_uniform_packets_retain_independent_sparse_bones_exactly) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = instance_auto;
    tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.lerpfrac = 0.37f;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 2 * (sizeof(int) + sizeof(vec3_t))] = {0};
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){.keyframeCount = 2, .datatype = TDATA_FLOAT3, .linetype = TRACK_LINEAR,
        .globalSeqId = (uint32_t)-1};
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)track->values + i * (sizeof(int) + sizeof(vec3_t)));
        vec3_t value = {17.123f * i, -23.71f * i, 31.3f * i};
        key->time = i * 500; memcpy(key->data, &value, sizeof(value));
    }
    mdxSequence_t seq = {.interval = {0, 1000}};
    f.node.translation = track; f.node.node_id = 999;
    f.model.nodes[0] = NULL; f.model.nodes[999] = &f.node; f.palette = 999;
    f.model.sequences = &seq; f.model.num_sequences = 1;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) {
        f.ent.frame = 100 + i * 100; f.ent.oldframe = 50 + i * 100; f.matrix.v[12] = i * 50;
        MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    }
    MDLX_EndModelBatch();
    T_EQ(inst.draws, 1); T_EQ(inst.total, 3); T_EQ(inst.palettes, 0); T_ASSERT(inst.uniform_draws[0]);
    FOR_LOOP(i, 3) {
        f.matrix.v[12] = i * 50;
        MDLX_BindBoneMatrices(&f.model, &f.matrix, 100 + i * 100, 50 + i * 100);
        T_EQ(memcmp(node_matrices + 999, inst.uniform_bones + i, sizeof(mat4_t)), 0);
        T_FEQ(inst.matrices[i].v[15], i + 1, 0); T_FEQ(inst.matrices[i].v[12], i * 50, 0);
    }
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

static cstring_t batch_stats_enabled(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_stats") ? "1" : fallback;
}

TEST(mdx_geoset, batch_diagnostics_count_real_packets_blockers_and_scope_boundaries) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    cstring_t (*old)(cstring_t, cstring_t) = ri.CvarString; ri.CvarString = batch_stats_enabled;
    batch_ticks = 100; batch_log[0] = 0;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    f.ent.team = 1; MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    f.ent.flags = RF_SELECTED; MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    batch_ticks = 1200; MDLX_EndModelBatch();
    T_NOT_NULL(strstr(batch_log, "phase=solid scopes=1 attempted=5 accepted=4 packets=2 singleton=1 instanced=3"));
    T_NOT_NULL(strstr(batch_log, "reject_selected=1"));
    T_NOT_NULL(strstr(batch_log, "flush_blocked=1"));
    T_NOT_NULL(strstr(batch_log, "flush_team=1"));
    T_EQ(inst.draws, 1); T_EQ(inst.total, 3);
    batch_log[0] = 0; MDLX_EndModelBatch(); T_EQ(batch_log[0], 0);
    ri.CvarString = old;
    MDLX_BeginModelBatch(); FOR_LOOP(i, 2) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    batch_ticks = 3000; MDLX_EndModelBatch(); T_EQ(batch_log[0], 0);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, batch_diagnostics_survive_interleaved_portrait_and_shadow_scopes) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    cstring_t (*old)(cstring_t, cstring_t) = ri.CvarString; ri.CvarString = batch_stats_enabled;
    batch_ticks = 100; batch_log[0] = 0;
    FOR_LOOP(i, 2) {
        tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.rdflags = RDF_NOFRUSTUMCULL;
        MDLX_BeginModelBatch(); FOR_LOOP(j, 2) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        batch_ticks = i ? 1200 : 500; MDLX_EndModelBatch();
        tr.viewDef.rdflags |= RDF_NOWORLDMODEL;
        MDLX_BeginModelBatch(); MDLX_EndModelBatch();
        tr.viewDef.rdflags = RDF_NOFRUSTUMCULL; tr.render_phase = RENDER_PHASE_LIGHTS;
        MDLX_BeginModelBatch(); MDLX_EndModelBatch();
    }
    T_NOT_NULL(strstr(batch_log, "phase=solid scopes=2 attempted=4 accepted=4 packets=2 singleton=0 instanced=4"));
    T_NULL(strstr(batch_log, "phase=lights"));
    ri.CvarString = old; MDLX_BeginModelBatch(); MDLX_EndModelBatch();
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

void R_TestResetParticles(void);
uint32_t R_TestParticleCount(void);
cparticle_t const *R_TestParticle(uint32_t index);

TEST(mdx_geoset, particle_geometry_batches_with_ordered_owner_emission) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    poses = draws = emits = 0; R_TestResetParticles(); tr.viewDef.deltaTime = 100;
    mdxParticleEmitter_t emitter = { .node.node_id = 1, .EmissionRate = 10, .LifeSpan = 1,
        .FrameFlags = 0, .Rows = 1, .Columns = 1 };
    f.model.emitters = &emitter;
    /* The emitter-only child is absent from the geometry palette. */
    mdxNode_t socket = { .node_id = 1, .parent_id = 0 }; vec3_t pivots[2] = {0};
    f.model.nodes[1] = f.model.node_list[1] = &socket; f.model.num_nodes = 2;
    f.model.pivots = pivots; f.model.num_pivots = 2;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 2 * (sizeof(int) + sizeof(vec3_t))] = {0};
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){ .keyframeCount = 2, .datatype = TDATA_FLOAT3, .linetype = TRACK_LINEAR,
        .globalSeqId = (uint32_t)-1 };
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)track->values + i * (sizeof(int) + sizeof(vec3_t)));
        vec3_t value = {10 * i, 0, 0}; key->time = i * 500; memcpy(key->data, &value, sizeof(value));
    }
    mdxSequence_t seq = { .interval = {0, 1000} };
    f.node.translation = track; f.model.sequences = &seq; f.model.num_sequences = 1;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) {
        f.matrix.v[12] = i * 100; f.ent.frame = f.ent.oldframe = i * 100;
        node_matrices[1].v[12] = 777;
        MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        T_EQ(draws, 0); T_EQ(R_TestParticleCount(), i+1);
        T_FEQ(R_TestParticle(i)->org.x, i * 102, 0.001f);
    }
    MDLX_EndModelBatch();
    T_EQ(draws, 1); T_EQ(inst.draws, 1); T_EQ(inst.total, 3); T_EQ(emits, 3);
    T_EQ(R_TestParticleCount(), 3); T_FEQ(emitter.accumulator, 0, 0.0001f);
    /* Selection and empty geometry retain one immediate emission per actor. */
    MDLX_BeginModelBatch(); MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    f.ent.flags = RF_SELECTED; MDX_RenderModel(&f.ent, &f.model, &f.matrix); MDLX_EndModelBatch();
    T_EQ(draws, 3); T_EQ(emits, 5); T_EQ(R_TestParticleCount(), 5);
    /* A model without geometry still owns one emission, through the immediate path. */
    f.ent.flags = 0; f.model.geosets = NULL;
    MDLX_BeginModelBatch(); MDX_RenderModel(&f.ent, &f.model, &f.matrix); MDLX_EndModelBatch();
    T_EQ(draws, 3); T_EQ(emits, 6); T_EQ(R_TestParticleCount(), 6);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL; R_TestResetParticles();
}

TEST(mdx_geoset, inactive_effects_allow_geometry_packets_without_advancing_emitters) {
    geosetFixture_t f;
    FOR_LOOP(alpha, 2) {
        setup_geoset(&f, alpha ? BLEND_MODE_BLEND : BLEND_MODE_NONE);
        tr.render_phase = alpha ? RENDER_PHASE_ALPHA : RENDER_PHASE_SOLID; draws = emits = 0;
        mdxParticleEmitter_t emitter = { .EmissionRate = 5, .accumulator = 0.375f };
        mdxRibbonEmitter_t ribbon = {0};
        if (alpha) f.model.emitters = &emitter; else f.model.ribbons = &ribbon;
        MDLX_BeginModelBatch();
        FOR_LOOP(i, 3) { MDX_RenderModel(&f.ent, &f.model, &f.matrix); T_EQ(draws, 0); }
        MDLX_EndModelBatch(); T_EQ(draws, 1); T_EQ(inst.draws, 1); T_EQ(inst.total, 3);
        T_FEQ(emitter.accumulator, 0.375f, 0.00001f); T_NULL(f.model.ribbon_states);
        MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
    }
}

static cstring_t instance_effects_disabled(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_effect_instancing") ? "0" : fallback;
}

/* Compare observable particle state and RNG with the same real actor calls, including packet overflow. */
TEST(mdx_geoset, effect_packets_preserve_particles_rng_and_singleton_capacity_flush) {
    geosetFixture_t f;
    vec3_t position[257], velocity[257]; int next_random = 0;
    FOR_LOOP(enabled, 2) {
        setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
        ri.CvarString = enabled ? instance_defaults : instance_effects_disabled;
        tr.viewDef.deltaTime = 100; tr.viewDef.time = 100;
        mdxParticleEmitter_t emitter = {.EmissionRate = 10, .LifeSpan = 1, .Speed = 17,
            .Variation = 7, .Latitude = 35, .Length = 3, .Width = 5, .Rows = 1, .Columns = 1};
        f.model.emitters = &emitter;
        R_TestResetParticles(); poses = draws = emits = 0; srand(421);
        MDLX_ClearPoseCache(); MDLX_BeginModelBatch();
        FOR_LOOP(i, 257) {
            f.matrix.v[12] = i * 13.7f; f.ent.frame = f.ent.oldframe = i;
            MDX_RenderModel(&f.ent, &f.model, &f.matrix);
            T_EQ(R_TestParticleCount(), i+1); T_EQ(emits, i+1);
            cparticle_t const *part = R_TestParticle(i);
            if (!enabled) { position[i] = part->org; velocity[i] = part->vel; }
            else {
                T_EQ(memcmp(&position[i], &part->org, sizeof(vec3_t)), 0);
                T_EQ(memcmp(&velocity[i], &part->vel, sizeof(vec3_t)), 0);
            }
        }
        MDLX_EndModelBatch();
        T_EQ(R_TestParticleCount(), 257); T_EQ(emits, 257); T_FEQ(emitter.accumulator, 0, 0.0001f);
        T_EQ(draws, enabled ? 2 : 257); T_EQ(inst.draws, enabled ? 1 : 0);
        if (enabled) { T_EQ(inst.counts[0], 256); T_EQ(inst.total, 256); T_EQ(rand(), next_random); }
        else next_random = rand();
        /* Alpha geometry does not advance particle state a second time. */
        tr.render_phase = RENDER_PHASE_ALPHA; f.layers[0].blendMode = BLEND_MODE_BLEND;
        MDLX_BeginModelBatch(); MDX_RenderModel(&f.ent, &f.model, &f.matrix); MDLX_EndModelBatch();
        T_EQ(R_TestParticleCount(), 257); T_EQ(emits, 257);
        MDLX_ShutdownModelBatches(); MDLX_ClearPoseCache(); mdlx.shader = NULL; R_TestResetParticles();
    }
}

static cstring_t instance_effect_uniform(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_effect_instancing") ? "2" : fallback;
}

/* Preserve the automatic trial and expose uniform-split packets as a separate selectable mode. */
TEST(mdx_geoset, effect_uniform_mode_splits_complete_palettes_without_duplicate_particles) {
    FOR_LOOP(uniform, 2) {
        geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
        ri.CvarString = uniform ? instance_effect_uniform : instance_defaults;
        int palette[16] = {0}; f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 16;
        mdxParticleEmitter_t emitter = {.EmissionRate = 10, .LifeSpan = 1, .Rows = 1, .Columns = 1};
        f.model.emitters = &emitter; tr.viewDef.deltaTime = 100;
        R_TestResetParticles(); poses = draws = emits = 0;
        MDLX_BeginModelBatch();
        FOR_LOOP(i, 9) {
            f.matrix.v[12] = i * 13.7f; MDX_RenderModel(&f.ent, &f.model, &f.matrix);
            T_EQ(R_TestParticleCount(), i+1); T_EQ(emits, i+1);
            T_FEQ(R_TestParticle(i)->org.x, i * 13.7f, 0.0001f);
        }
        MDLX_EndModelBatch();
        T_EQ(draws, uniform ? 2 : 1); T_EQ(inst.draws, 1);
        T_EQ(inst.counts[0], uniform ? 8 : 9); T_EQ(inst.palettes, uniform ? 0 : 1);
        T_EQ(inst.uniform_draws[0], uniform); T_EQ(emits, 9); T_EQ(R_TestParticleCount(), 9);
        MDLX_ShutdownModelBatches(); mdlx.shader = NULL; R_TestResetParticles();
    }
}

TEST(mdx_geoset, active_alpha_ribbons_keep_immediate_actor_order) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_BLEND); tr.render_phase = RENDER_PHASE_ALPHA; draws = 0;
    mdxRibbonEmitter_t ribbon = {0}; f.model.ribbons = &ribbon;
    f.ent.flags = RF_NOT_SELECTABLE; /* Held death frame suppresses new trail allocation. */
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) { MDX_RenderModel(&f.ent, &f.model, &f.matrix); T_EQ(draws, i+1); }
    MDLX_EndModelBatch(); T_EQ(inst.draws, 0);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, numeric_bone_sampler_uses_high_precision_before_es_uniform_declarations) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    MDLX_BeginModelBatch(); FOR_LOOP(i, 2) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch(); T_ASSERT(inst.numeric_highp);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

static cstring_t submit_cache_cvar(cstring_t name, cstring_t fallback) {
    if (!strcmp(name, "r_mdx_uniform_instancing")) return submit_uniform ? "1" : "0";
    return !strcmp(name, "r_mdx_submit_cache") ? submit_cache_value : instance_defaults(name, fallback);
}

/* Prepared vertex bindings should survive repeated packets without changing their per-instance divisors. */
TEST(mdx_geoset, cached_submission_does_no_per_draw_attribute_mutations) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = submit_cache_cvar;
    f.inst_vao = 2; f.geo.inst_vaos = &f.inst_vao; ARRAY_COUNT(f.geo.inst_vaos) = 1;
    f.model.buffers[BZ_MDX_INSTANCE_BUFFER] = 83; inst.prepared[0] = 2; inst.prepared_count = 1;
    FOR_LOOP(i, 4) inst.divisors[i] = 1; /* Fixture's prepared instance VAO. */
    tr.render_phase = RENDER_PHASE_SOLID; draws = poses = emits = 0;
    FOR_LOOP(packet, 2) {
        MDLX_BeginModelBatch();
        FOR_LOOP(i, 3) {
            f.matrix.v[12] = packet * 50 + i * 10;
            MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        }
        MDLX_EndModelBatch();
    }
    T_EQ(inst.draws, 2); T_EQ(inst.total, 6); T_EQ(inst.uploads, 2);
    T_EQ(inst.divisor_calls, 0); T_EQ(inst.attrib_calls, 0);
    FOR_LOOP(i, 2) { T_EQ(inst.upload_buffers[i], 83); T_EQ(inst.draw_vaos[i], 2); }
    FOR_LOOP(i, 3) T_FEQ(inst.matrices[i].v[12], 50 + i * 10, 0.001f);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

/* Alternating models retain packet order in every submission mode and restore ordinary drawing afterward. */
TEST(mdx_geoset, model_streams_and_cached_arrays_preserve_packet_order_and_inverse) {
    cstring_t modes[] = {"0", "1", "2"};
    FOR_LOOP(transport, 2) FOR_LOOP(phase, 2) FOR_LOOP(mode, 3) {
        submit_uniform = transport != 0;
        geosetFixture_t a, b;
        setup_geoset(&a, phase ? BLEND_MODE_BLEND : BLEND_MODE_NONE);
        setup_geoset(&b, phase ? BLEND_MODE_BLEND : BLEND_MODE_NONE);
        a.vao = 1; b.vao = 3; a.inst_vao = 2; b.inst_vao = 4;
        a.geo.inst_vaos = &a.inst_vao; ARRAY_COUNT(a.geo.inst_vaos) = 1;
        b.geo.inst_vaos = &b.inst_vao; ARRAY_COUNT(b.geo.inst_vaos) = 1;
        a.model.buffers[BZ_MDX_INSTANCE_BUFFER] = 83; b.model.buffers[BZ_MDX_INSTANCE_BUFFER] = 87;
        inst.prepared[0] = 2; inst.prepared[1] = 4; inst.prepared_count = 2;
        submit_cache_value = modes[mode]; ri.CvarString = submit_cache_cvar;
        tr.render_phase = phase ? RENDER_PHASE_ALPHA : RENDER_PHASE_SOLID; draws = 0;
        MDLX_BeginModelBatch();
        geosetFixture_t *order[] = {&a, &b, &a};
        FOR_LOOP(packet, 3) FOR_LOOP(i, 2) {
            geosetFixture_t *f = order[packet]; f->matrix.v[12] = packet * 100 + i * 10;
            MDX_RenderModel(&f->ent, &f->model, &f->matrix);
        }
        MDLX_EndModelBatch();
        T_EQ(inst.draws, 3); T_EQ(inst.total, 6); T_EQ(inst.uploads, 3);
        FOR_LOOP(i, 3) {
            T_EQ(inst.counts[i], 2);
            T_EQ(inst.upload_buffers[i], mode ? (i == 1 ? 87 : 83) : 79);
            T_EQ(inst.draw_vaos[i], (i == 1 ? 3 : 1) + (mode == 2));
        }
        T_EQ(inst.attrib_calls == 0, mode == 2); T_EQ(inst.divisor_calls == 0, mode == 2);
        MDX_RenderModel(&a.ent, &a.model, &a.matrix);
        T_EQ(inst.bound_vao, 1); FOR_LOOP(i, 4) T_EQ(inst.divisors[i], 0);
        MDLX_BeginModelBatch();
        FOR_LOOP(i, 2) MDX_RenderModel(&a.ent, &a.model, &a.matrix);
        MDLX_EndModelBatch(); T_EQ(inst.draws, 4); T_EQ(inst.total, 8);
        T_EQ(inst.draw_vaos[3], mode == 2 ? 2 : 1);
        MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
    }
    submit_cache_value = "2"; submit_uniform = false;
}

TEST(mdx_geoset, adjacent_equal_poses_draw_in_order_with_one_instance_upload) {
    geosetFixture_t f;
    FOR_LOOP(phase, 2) {
        setup_geoset(&f, phase ? BLEND_MODE_BLEND : BLEND_MODE_NONE);
        tr.render_phase = phase ? RENDER_PHASE_ALPHA : RENDER_PHASE_SOLID;
        poses = draws = emits = 0;
        MDLX_BeginModelBatch();
        FOR_LOOP(i, 3) {
            f.matrix.v[12] = i * 10;
            MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        }
        T_EQ(draws, 0);
        MDLX_EndModelBatch();
        T_EQ(draws, 1); T_EQ(inst.draws, 1); T_EQ(inst.total, 3); T_EQ(inst.uploads, 1);
        FOR_LOOP(i, 3) T_FEQ(inst.matrices[i].v[12], i * 10, 0.001f);
        FOR_LOOP(i, 4) T_EQ(inst.divisors[i], 0);
        T_EQ(mdlx.shader, &shader);
        MDLX_ShutdownModelBatches();
    }
    mdlx.shader = NULL;
}

TEST(mdx_geoset, independently_animated_actors_keep_their_own_gpu_bones_in_one_draw) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID; poses = draws = 0;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 2 * (sizeof(int) + sizeof(vec3_t))] = {0};
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){ .keyframeCount = 2, .datatype = TDATA_FLOAT3, .linetype = TRACK_LINEAR,
        .globalSeqId = (uint32_t)-1 };
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)track->values + i * (sizeof(int) + sizeof(vec3_t)));
        vec3_t value = {10 * i, 20 * i, 30 * i};
        key->time = i * 500; memcpy(key->data, &value, sizeof(value));
    }
    mdxSequence_t seq = { .interval = {0, 1000} };
    f.node.translation = track; f.model.sequences = &seq; f.model.num_sequences = 1;
    /* A sparse ID must not create hundreds of unused GPU matrices per actor. */
    f.node.node_id = 999; f.model.nodes[0] = NULL; f.model.nodes[999] = &f.node; f.palette = 999;
    uint8_t rs[sizeof(mdxKeyTrack_t) + sizeof(int) + sizeof(quaternion_t)] = {0};
    uint8_t ss[sizeof(mdxKeyTrack_t) + sizeof(int) + sizeof(vec3_t)] = {0};
    f.node.rotation = (mdxKeyTrack_t *)rs; f.node.scale = (mdxKeyTrack_t *)ss;
    *f.node.rotation = (mdxKeyTrack_t){.keyframeCount = 1, .datatype = TDATA_FLOAT4,
        .linetype = TRACK_NO_INTERP, .globalSeqId = (uint32_t)-1};
    *f.node.scale = (mdxKeyTrack_t){.keyframeCount = 1, .datatype = TDATA_FLOAT3,
        .linetype = TRACK_NO_INTERP, .globalSeqId = (uint32_t)-1};
    mdxKeyFrame_t *rk = (mdxKeyFrame_t *)f.node.rotation->values, *sk = (mdxKeyFrame_t *)f.node.scale->values;
    quaternion_t rotation = {0, 0, 0.70710678f, 0.70710678f}; vec3_t scale = {2, 3, 4};
    memcpy(rk->data, &rotation, sizeof(rotation)); memcpy(sk->data, &scale, sizeof(scale));
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) {
        f.ent.frame = f.ent.oldframe = 100 + i * 100; f.matrix.v[12] = 50 * i;
        MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    }
    T_EQ(draws, 0);
    MDLX_EndModelBatch();
    T_EQ(draws, 1); T_EQ(inst.draws, 1); T_EQ(inst.total, 3); T_EQ(inst.palettes, 1);
    T_EQ(inst.bone_rows, 1);
    FOR_LOOP(i, 3) {
        uint32_t base = (uint32_t)inst.matrices[i].v[15] - 1;
        T_EQ(base, i * 2);
        T_ASSERT(base < sizeof(inst.bones) / sizeof(*inst.bones) / 3 - 1);
        if (base >= sizeof(inst.bones) / sizeof(*inst.bones) / 3 - 1) continue;
        /* Three rows retain the full affine transform: XYZ translation, rotation and nonuniform bone scale. */
        vec4_t const *rows = inst.bones + base * 3;
        T_FEQ(rows[0].x, 1, 0.001f); T_FEQ(rows[1].y, 1, 0.001f); T_FEQ(rows[2].z, 1, 0.001f);
        T_FEQ(rows[3].x, 0, 0.001f); T_FEQ(rows[3].y, -3, 0.001f); T_FEQ(rows[3].z, 0, 0.001f);
        T_FEQ(rows[4].x, 2, 0.001f); T_FEQ(rows[4].y, 0, 0.001f); T_FEQ(rows[4].z, 0, 0.001f);
        T_FEQ(rows[5].x, 0, 0.001f); T_FEQ(rows[5].y, 0, 0.001f); T_FEQ(rows[5].z, 4, 0.001f);
        T_FEQ(rows[3].w, 2 + i * 2, 0.001f);
        T_FEQ(rows[4].w, 4 + i * 4, 0.001f); T_FEQ(rows[5].w, 6 + i * 6, 0.001f);
        T_FEQ(inst.matrices[i].v[12], i * 50, 0.001f);
    }
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

static cstring_t instance_cvar_off(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_instancing") ? "0" : fallback;
}

TEST(mdx_geoset, instance_palette_compacts_sparse_nodes_and_maps_missing_entries_to_identity) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    mdxNode_t second = {.node_id = 10, .parent_id = (uint32_t)-1};
    f.node.node_id = 3; f.model.nodes[0] = NULL; f.model.nodes[3] = &f.node; f.model.nodes[10] = &second;
    f.model.node_list[0] = &second; f.model.node_list[1] = &f.node; f.model.num_nodes = 2;
    int palette[] = {3, -1, 10, 127}; f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 4;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) { f.matrix.v[12] = i * 10; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
    MDLX_EndModelBatch();
    T_EQ(instance_prog.offsets[0], 2); T_EQ(instance_prog.offsets[1], 0);
    T_EQ(instance_prog.offsets[2], 1); T_EQ(instance_prog.offsets[3], 0);
    FOR_LOOP(i, 3) T_FEQ(inst.matrices[i].v[15], i * 3 + 1, 0.001f);
    T_EQ(inst.total, 3); T_EQ(inst.bone_rows, 1);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

static cstring_t instance_orphan_enabled(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_bone_orphan") ? "1" : fallback;
}

static cstring_t instance_uniform_enabled(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "r_mdx_uniform_instancing") ? "1" : fallback;
}

TEST(mdx_geoset, uniform_instance_packets_keep_individual_poses_and_split_before_palette_capacity) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    cstring_t (*old)(cstring_t, cstring_t) = ri.CvarString; ri.CvarString = instance_uniform_enabled;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 2 * (sizeof(int) + sizeof(vec3_t))] = {0};
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){.keyframeCount = 2, .datatype = TDATA_FLOAT3,
        .linetype = TRACK_LINEAR, .globalSeqId = (uint32_t)-1};
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)track->values + i * (sizeof(int) + sizeof(vec3_t)));
        key->time = i * 500; vec3_t v = {10 * i, 0, 0}; memcpy(key->data, &v, sizeof(v));
    }
    mdxSequence_t seq = {.interval = {0, 1000}};
    f.node.translation = track; f.model.sequences = &seq; f.model.num_sequences = 1;
    int palette[] = {999, -1};
    f.node.node_id = 999; f.model.nodes[0] = NULL; f.model.nodes[999] = &f.node;
    f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 2;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) { f.ent.frame = f.ent.oldframe = 100 + i * 100; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
    MDLX_EndModelBatch();
    T_ASSERT(inst.uniform_shader); T_EQ(inst.palettes, 0); T_EQ(inst.allocations, 0); T_EQ(inst.uniform_count, 6);
    FOR_LOOP(i, 3) {
        T_FEQ(inst.uniform_bones[i * 2].v[12], 2 + i * 2, 0.001f);
        T_FEQ(inst.uniform_bones[i * 2 + 1].v[0], 1, 0.001f); T_FEQ(inst.uniform_bones[i * 2 + 1].v[12], 0, 0.001f);
        T_FEQ(inst.matrices[i].v[15], i + 1, 0.001f);
    }
    MDLX_ShutdownModelBatches();
    memset(&inst, 0, sizeof(inst));
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 130) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch();
    T_EQ(inst.draws, 3); T_EQ(inst.counts[0], 64); T_EQ(inst.counts[1], 64); T_EQ(inst.counts[2], 2); T_EQ(inst.total, 130);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL; ri.CvarString = old;
}

TEST(mdx_geoset, bone_orphaning_renames_storage_before_every_packet_upload) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID;
    cstring_t (*old)(cstring_t, cstring_t) = ri.CvarString; ri.CvarString = instance_orphan_enabled;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 4) { f.ent.flags = i < 2 ? 0 : RF_NO_LIGHTING; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
    MDLX_EndModelBatch();
    T_EQ(inst.total, 4); T_EQ(inst.palettes, 2); T_EQ(inst.allocations, 2);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL; ri.CvarString = old;
}

TEST(mdx_geoset, instance_toggle_and_ui_scopes_preserve_immediate_submission) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID; poses = draws = 0;
    refImport_t saved = ri;
    ri.CvarString = instance_cvar_off;
    MDLX_BeginModelBatch();
    MDX_RenderModel(&f.ent, &f.model, &f.matrix); T_EQ(draws, 1);
    MDLX_EndModelBatch();
    ri = saved;
    tr.viewDef.rdflags |= RDF_NOWORLDMODEL;
    MDLX_BeginModelBatch();
    MDX_RenderModel(&f.ent, &f.model, &f.matrix); T_EQ(draws, 2);
    MDLX_EndModelBatch(); T_EQ(inst.draws, 0);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, shader_switch_fills_both_program_states_and_nonuniform_basis_stays_immediate) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID; poses = draws = 0;
    Matrix4_identity(&tr.viewDef.viewProjectionMatrix);
    MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_BeginModelBatch();
    MDX_RenderModel(&f.ent, &f.model, &f.matrix); MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch();
    T_EQ(memcmp(&instance_prog.base.state.viewProjection, &tr.viewDef.viewProjectionMatrix, sizeof(mat4_t)), 0);
    T_EQ(memcmp(&shader.state.viewProjection, &tr.viewDef.viewProjectionMatrix, sizeof(mat4_t)), 0);
    uint32_t old = draws;
    f.matrix.v[0] = 2;
    MDLX_BeginModelBatch();
    MDX_RenderModel(&f.ent, &f.model, &f.matrix); T_EQ(draws, old + 1);
    MDLX_EndModelBatch(); T_EQ(inst.draws, 1);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, batch_state_change_flushes_previous_instances_and_singletons_draw_normally) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID; poses = draws = 0;
    MDLX_BeginModelBatch();
    MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    f.matrix.v[12] = 10; MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    f.ent.team = 1; MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    T_EQ(draws, 1); T_EQ(inst.total, 2);
    MDLX_EndModelBatch();
    T_EQ(draws, 2); T_EQ(inst.draws, 1);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, multi_layer_alpha_keeps_immediate_order_and_billboards_use_independent_palettes) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_BLEND); tr.render_phase = RENDER_PHASE_ALPHA; poses = draws = 0;
    f.mat.num_layers = 2;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch();
    T_EQ(draws, 6); T_EQ(inst.draws, 0);
    f.mat.num_layers = 1; f.node.flags = MDLXNODE_Billboarded;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 3) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch();
    T_EQ(draws, 7); T_EQ(inst.draws, 1); T_EQ(inst.total, 3); T_EQ(inst.palettes, 1);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, layered_model_mixes_instanced_solid_with_immediate_alpha) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
    /* Headhunter-shaped material: two geosets share an opaque base and a blended overlay. */
    f.mat.num_layers = 2; f.layers[1].blendMode = BLEND_MODE_BLEND;
    mdxGeoset_t next = f.geo; f.geo.next = &next;
    draws = poses = 0; tr.render_phase = RENDER_PHASE_SOLID;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 2) { f.matrix.v[12] = i * 100; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
    MDLX_EndModelBatch();
    T_EQ(inst.draws, 2); T_EQ(inst.total, 4); T_EQ(draws, 2); T_EQ(poses, 0);
    tr.render_phase = RENDER_PHASE_ALPHA; MDLX_BeginModelBatch();
    FOR_LOOP(i, 2) { f.matrix.v[12] = i * 100; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
    MDLX_EndModelBatch();
    T_EQ(inst.draws, 2); T_EQ(draws, 6); T_EQ(poses, 2);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, instance_packet_capacity_flushes_every_actor) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE); tr.render_phase = RENDER_PHASE_SOLID; poses = draws = 0;
    MDLX_BeginModelBatch();
    FOR_LOOP(i, 300) { f.matrix.v[12] = i; MDX_RenderModel(&f.ent, &f.model, &f.matrix); }
    MDLX_EndModelBatch();
    T_EQ(draws, 2); T_EQ(inst.draws, 2); T_EQ(inst.total, 300); T_EQ(inst.uploads, 2);
    T_EQ(inst.counts[0], 256); T_EQ(inst.counts[1], 44);
    T_FEQ(inst.matrices[0].v[12], 256, 0.001f); T_FEQ(inst.matrices[43].v[12], 299, 0.001f);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

static void draw_geoset(geosetFixture_t *f, render_phase_t phase, uint32_t want_pose, uint32_t want_draw) {
    poses = draws = emits = 0; tr.render_phase = phase;
    Matrix4_identity(&node_matrices[0]); node_matrices[0].v[12] = 777;
    MDX_RenderModel(&f->ent, &f->model, &f->matrix);
    T_EQ(poses, want_pose); T_EQ(draws, want_draw);
    T_FEQ(node_matrices[0].v[12], want_pose ? 0 : 777, 0.001f);
    if (want_draw) T_FEQ(drawn_bone.v[12], 0, 0.001f);
}

TEST(mdx_geoset, empty_material_phases_skip_pose_and_keep_real_geometry) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_ALPHAKEY);
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    f.layers[0].blendMode = BLEND_MODE_BLEND;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    f.layers[1].blendMode = BLEND_MODE_NONE; f.mat.num_layers = 2;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    BLEND_MODE const modes[] = {BLEND_MODE_ADD, BLEND_MODE_ADDALPHA, BLEND_MODE_MODULATE, BLEND_MODE_MODULATE_2X};
    f.mat.num_layers = 1;
    FOR_LOOP(i, sizeof(modes) / sizeof(*modes)) {
        f.layers[0].blendMode = modes[i];
        draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
        draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    }
    f.mat.num_layers = 0;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, instance_alpha_moves_opaque_geometry_to_alpha_pass) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE);
    f.ent.tint = (color32_t){255, 255, 255, 93};
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    f.ent.tint.a = 0; f.ent.tint_valid = true;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    f.ent.tint_valid = false;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, later_geoset_can_supply_the_phase_missing_from_the_first) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE);
    mdxGeoset_t next = f.geo;
    mdxMaterialLayer_t layer = f.layers[0];
    mdxMaterial_t mat = { .layers = &layer, .num_layers = 1 };
    layer.blendMode = BLEND_MODE_BLEND;
    next.materialID = 1; f.geo.next = &next; f.mat.next = &mat;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    mdxGeosetAnim_t hidden = { .staticAlpha = 0 };
    next.geosetAnim = &hidden;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    hidden.staticAlpha = 1;
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, invisible_geosets_skip_pose_then_reappear_at_authored_frame) {
    geosetFixture_t f;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 2 * (sizeof(int) + sizeof(float))] = {0};
    mdxKeyTrack_t *alpha = (mdxKeyTrack_t *)storage;
    *alpha = (mdxKeyTrack_t){ .keyframeCount = 2, .linetype = TRACK_LINEAR,
        .datatype = TDATA_FLOAT1, .globalSeqId = (uint32_t)-1 };
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)alpha->values + i * (sizeof(int) + sizeof(float)));
        float value = i;
        key->time = i * 500; memcpy(key->data, &value, sizeof(value));
    }
    mdxSequence_t seq = { .interval = {0, 1000} };
    mdxGeosetAnim_t anim = { .staticAlpha = 1, .alphas = alpha };
    setup_geoset(&f, BLEND_MODE_NONE);
    f.model.sequences = &seq; f.model.num_sequences = 1; f.geo.geosetAnim = &anim;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    f.ent.frame = f.ent.oldframe = 500;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    f.ent.frame = f.ent.oldframe = 0;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, empty_geometry_keeps_particle_and_ribbon_pose_in_their_phase) {
    geosetFixture_t f;
    mdxParticleEmitter_t emitter = { .EmissionRate = 5 };
    mdxRibbonEmitter_t ribbon = {0};
    setup_geoset(&f, BLEND_MODE_NONE);
    tr.viewDef.deltaTime = 100;
    f.model.geosets = NULL; f.model.emitters = &emitter;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 0); T_EQ(emits, 1);
    T_FEQ(emitter.accumulator, 0.5f, 0.001f);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0); T_EQ(emits, 0);
    T_FEQ(emitter.accumulator, 0.5f, 0.001f);
    f.model.emitters = NULL; f.model.ribbons = &ribbon;
    f.ent.flags = RF_NOT_SELECTABLE; /* A held death frame has no live ribbon vertices. */
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 0);
    f.model.ribbons = NULL;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, carried_resource_animation_is_remapped_before_visibility_rejection) {
    geosetFixture_t f;
    uint8_t storage[sizeof(mdxKeyTrack_t) + 3 * (sizeof(int) + sizeof(float))] = {0};
    mdxKeyTrack_t *alpha = (mdxKeyTrack_t *)storage;
    *alpha = (mdxKeyTrack_t){ .keyframeCount = 3, .linetype = TRACK_LINEAR,
        .datatype = TDATA_FLOAT1, .globalSeqId = (uint32_t)-1 };
    FOR_LOOP(i, 3) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)alpha->values + i * (sizeof(int) + sizeof(float)));
        float value = i == 1;
        key->time = i * 1000; memcpy(key->data, &value, sizeof(value));
    }
    mdxSequence_t seqs[] = {{ .name = "Stand", .interval = {0, 1000} },
        { .name = "Stand Lumber", .interval = {1000, 2000} }, { .name = "Stand Gold", .interval = {2000, 3000} }};
    mdxGeosetAnim_t anim = { .staticAlpha = 1, .alphas = alpha };
    setup_geoset(&f, BLEND_MODE_NONE);
    f.model.sequences = seqs; f.model.num_sequences = 3; f.geo.geosetAnim = &anim;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    f.ent.flags = RF_HAS_LUMBER;
    draw_geoset(&f, RENDER_PHASE_SOLID, 1, 1);
    f.ent.flags = RF_HAS_GOLD;
    draw_geoset(&f, RENDER_PHASE_SOLID, 0, 0);
    T_EQ(f.ent.frame, 0); T_EQ(f.ent.oldframe, 0);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, shadow_pass_keeps_opaque_pose_and_filters_blended_draws) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_NONE);
    draw_geoset(&f, RENDER_PHASE_LIGHTS, 1, 1);
    f.layers[0].blendMode = BLEND_MODE_BLEND;
#ifdef USE_SHADOWMAPS
    draw_geoset(&f, RENDER_PHASE_LIGHTS, 1, 0);
#else
    draw_geoset(&f, RENDER_PHASE_LIGHTS, 1, 1);
#endif
    mdlx.shader = NULL;
}

/* Visibility needs only alpha; colour animation belongs to an actual draw. */
TEST(mdx_geoset, alpha_pass_samples_color_only_for_drawn_geosets) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_BLEND);
    uint32_t as[sizeof(mdxKeyTrack_t) / 4 + 4] = {0}, cs[sizeof(mdxKeyTrack_t) / 4 + 8] = {0};
    mdxKeyTrack_t *alpha = (mdxKeyTrack_t *)as, *rgb = (mdxKeyTrack_t *)cs;
    *alpha = (mdxKeyTrack_t){ .keyframeCount = 2, .datatype = TDATA_FLOAT1,
        .linetype = TRACK_LINEAR, .globalSeqId = (uint32_t)-1 };
    *rgb = *alpha; rgb->datatype = TDATA_FLOAT3;
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *ak = (mdxKeyFrame_t *)((uint8_t *)alpha->values + i * (sizeof(int) + sizeof(float)));
        mdxKeyFrame_t *ck = (mdxKeyFrame_t *)((uint8_t *)rgb->values + i * (sizeof(int) + sizeof(vec3_t)));
        float a = i; vec3_t c = {0.2f + 0.4f * i, 0.4f, 0.8f - 0.4f * i};
        ak->time = ck->time = i * 500;
        memcpy(ak->data, &a, sizeof(a)); memcpy(ck->data, &c, sizeof(c));
    }
    mdxSequence_t seq = { .interval = {0, 1000} };
    mdxGeosetAnim_t anim = { .flags = 2, .staticAlpha = 1, .alphas = alpha, .colors = rgb };
    mdxGeoset_t geos[8];
    FOR_LOOP(i, 8) {
        geos[i] = f.geo; geos[i].geosetAnim = &anim; geos[i].next = i < 7 ? &geos[i + 1] : NULL;
    }
    f.model.geosets = geos; f.model.sequences = &seq; f.model.num_sequences = 1;
    f.ent.frame = f.ent.oldframe = 250;
    alpha_reads = color_reads = 0;
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 8);
    T_ASSERT(alpha_reads <= 17); T_EQ(color_reads, 8);
    /* KGAC is BGR, and instance tint still multiplies all four channels. */
    T_FEQ(drawn_color.x, 0.6f, 0.0001f); T_FEQ(drawn_color.y, 0.4f, 0.0001f);
    T_FEQ(drawn_color.z, 0.4f, 0.0001f); T_FEQ(drawn_color.w, 0.5f, 0.0001f);
    f.ent.frame = f.ent.oldframe = 0; alpha_reads = color_reads = 0;
    draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    T_EQ(alpha_reads, 8); T_EQ(color_reads, 0);
    f.ent.frame = f.ent.oldframe = 500; f.ent.tint = (color32_t){255, 128, 64, 128};
    alpha_reads = color_reads = 0;
    draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 8);
    T_ASSERT(alpha_reads <= 17); T_EQ(color_reads, 8);
    T_FEQ(drawn_color.x, 0.4f, 0.0001f); T_FEQ(drawn_color.y, 0.4f * 128 / 255, 0.0001f);
    T_FEQ(drawn_color.z, 0.6f * 64 / 255, 0.0001f); T_FEQ(drawn_color.w, 128.0f / 255, 0.0001f);
    mdlx.shader = NULL;
}

TEST(mdx_geoset, visibility_preserves_static_and_global_alpha_semantics) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_BLEND);
    mdxGeosetAnim_t anim = { .staticAlpha = 1 };
    f.geo.geosetAnim = &anim;
    float const values[] = {-1, 0, EPSILON / 2, EPSILON, 0.5f, 1, 2, NAN, INFINITY, -INFINITY};
    FOR_LOOP(i, sizeof(values) / sizeof(*values)) {
        anim.staticAlpha = values[i];
        float want = isfinite(values[i]) ? MIN(MAX(values[i], 0), 1) : 1;
        T_FEQ(MDLX_EvaluateGeosetColor(&f.model, &f.geo, 0).w, want, 0.000001f);
        T_EQ(MDLX_IsGeosetVisible(&f.model, &f.geo, 0), want >= EPSILON);
    }
    uint32_t storage[sizeof(mdxKeyTrack_t) / 4 + 4] = {0};
    mdxKeyTrack_t *alpha = (mdxKeyTrack_t *)storage;
    *alpha = (mdxKeyTrack_t){ .keyframeCount = 2, .datatype = TDATA_FLOAT1,
        .linetype = TRACK_LINEAR, .globalSeqId = 0 };
    mdxGlobalSequence_t global = { .value = 1000 };
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (mdxKeyFrame_t *)((uint8_t *)alpha->values + i * (sizeof(int) + sizeof(float)));
        float value = i;
        key->time = i * 500; memcpy(key->data, &value, sizeof(value));
    }
    f.model.globalSequences = &global; f.model.num_globalSequences = 1;
    anim.staticAlpha = 0; anim.alphas = alpha;
    tr.viewDef.time = 500; draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    tr.viewDef.time = 1001; draw_geoset(&f, RENDER_PHASE_ALPHA, 0, 0);
    tr.viewDef.time = 1251; draw_geoset(&f, RENDER_PHASE_ALPHA, 1, 1);
    T_FEQ(drawn_color.w, 0.5f, 0.0001f);
    alpha->keyframeCount = 1;
    mdxKeyFrame_t *key = (mdxKeyFrame_t *)alpha->values;
    FOR_LOOP(i, sizeof(values) / sizeof(*values)) {
        memcpy(key->data, &values[i], sizeof(float));
        float want = MIN(MAX(values[i], 0), 1);
        T_FEQ(MDLX_EvaluateGeosetColor(&f.model, &f.geo, 0).w, want, 0.000001f);
        T_EQ(MDLX_IsGeosetVisible(&f.model, &f.geo, 0), want >= EPSILON);
    }
    mdlx.shader = NULL;
}

static cstring_t packet_cvars(cstring_t name, cstring_t fallback) {
    if (!strcmp(name, "r_mdx_packet_prepare")) return packet_mode;
    if (!strcmp(name, "r_mdx_uniform_instancing")) return packet_uniform ? "1" : "0";
    if (!strcmp(name, "r_mdx_uniform_auto")) return "0";
    return instance_defaults(name, fallback);
}

TEST(mdx_geoset, prepared_packets_sample_materials_on_workers_and_draw_without_resampling) {
    extern void MDLX_TestResetPoseCache(void);
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
    packet_mode = "2"; packet_uniform = true;
    uint32_t storage[sizeof(mdxKeyTrack_t) / 4 + 4] = {0};
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){.keyframeCount = 2, .datatype = TDATA_FLOAT1,
        .linetype = TRACK_LINEAR, .globalSeqId = 0};
    mdxKeyFrame_t *left = track->values, *right = (mdxKeyFrame_t *)((char *)left + 8);
    float low = .35f, high = .85f;
    left->time = 0; right->time = 600;
    memcpy(left->data, &low, sizeof(low)); memcpy(right->data, &high, sizeof(high));
    mdxGlobalSequence_t global = {.value = 700};
    f.model.globalSequences = &global; f.model.num_globalSequences = 1;
    f.layers[0].alpha = track;
    ri.CvarString = packet_cvars; tr.viewDef.time = 317; tr.render_phase = RENDER_PHASE_SOLID;
    MDLX_TestResetPoseCache(); MDLX_BeginModelBatch(); T_ASSERT(MDLX_BeginPosePreparation(2));
    sample_owner = SDL_ThreadID(); worker_reads = 0; poses = draws = emits = 0;
    FOR_LOOP(i, 32) {
        f.ent.frame = 100 + i; f.ent.oldframe = 50 + i;
        MDLX_QueueEntityPose(&f.ent, &f.model, &f.matrix);
    }
    mat3_t uv = shader.state.uvMatrix;
    MDLX_EndPosePreparation();
    T_EQ(memcmp(&uv, &shader.state.uvMatrix, sizeof(uv)), 0);
    T_EQ(draws, 0); T_EQ(emits, 0); T_ASSERT(worker_reads > 0);
    T_EQ(inst.uploads, 0); T_EQ(inst.palettes, 0); T_EQ(inst.allocations, 0);
    alpha_reads = color_reads = 0;
    FOR_LOOP(i, 32) {
        f.ent.frame = 100 + i; f.ent.oldframe = 50 + i;
        MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    }
    MDLX_EndModelBatch();
    T_EQ(inst.total, 32); T_EQ(inst.draws, 1); T_EQ(alpha_reads, 0); T_EQ(color_reads, 0);
    sample_owner = 0;
    MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
}

/* Non-stock local/global tracks expose stale clocks, tint, flipbook, UV and palette contamination. */
static mdxKeyTrack_t *packet_track(uint32_t *storage, MODELKEYTRACKDATATYPE type, bool global) {
    mdxKeyTrack_t *track = (mdxKeyTrack_t *)storage;
    *track = (mdxKeyTrack_t){.keyframeCount = 2, .datatype = type, .linetype = TRACK_LINEAR,
        .globalSeqId = global ? 0 : (uint32_t)-1};
    uint32_t size = GetModelKeyTrackDataTypeSize(type);
    FOR_LOOP(i, 2) {
        mdxKeyFrame_t *key = (void *)((char *)track->values + i * (4 + size));
        float values[4] = {.2f + i * .6f, .35f + i * .13f, .71f - i * .42f, 1};
        key->time = i * 900;
        if (type == TDATA_INT1) memcpy(key->data, &i, sizeof(i));
        else memcpy(key->data, values, size);
    }
    return track;
}

TEST(mdx_geoset, wide_blocks_consume_worker_packets_beyond_the_original_uniform_range) {
    extern void MDLX_TestResetPoseCache(void);
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = block_settings;
    block_mode = "2"; block = (typeof(block)){.limit = 65536};
    tr.viewDef.time = 317; tr.viewDef.lerpfrac = .41f; tr.render_phase = RENDER_PHASE_SOLID;
    uint32_t storage[sizeof(mdxKeyTrack_t) / 4 + 10] = {0};
    f.node.translation = packet_track(storage, TDATA_FLOAT3, false);
    f.node.node_id = 51; f.node.flags = MDLXNODE_Billboarded;
    f.model.nodes[0] = NULL; f.model.nodes[51] = &f.node;
    mdxSequence_t seq = {.interval = {0, 1000}};
    f.model.sequences = &seq; f.model.num_sequences = 1;
    int palette[] = {51, -1, 51, 999, 51, 51, 51, 51, 51};
    f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 9;
    mat4_t expected[32][9], matrices[32];
    FOR_LOOP(i, 32) {
        f.matrix.v[12] = i * 13.7f; matrices[i] = f.matrix;
        MDLX_BindBoneMatrices(&f.model, &f.matrix, 100 + i * 20, 50 + i * 20);
        FOR_LOOP(j, 9) {
            if (palette[j] == 51) expected[i][j] = node_matrices[51];
            else Matrix4_identity(&expected[i][j]);
        }
    }
    MDLX_TestResetPoseCache(); MDLX_BeginModelBatch(); T_ASSERT(MDLX_BeginPosePreparation(2));
    FOR_LOOP(i, 32) {
        f.ent.frame = 100 + i * 20; f.ent.oldframe = 50 + i * 20;
        MDLX_QueueEntityPose(&f.ent, &f.model, &matrices[i]);
    }
    MDLX_EndPosePreparation(); T_ASSERT(MDLX_PosePreparationStats().worker_packets > 0);
    FOR_LOOP(i, 32) {
        f.ent.frame = 100 + i * 20; f.ent.oldframe = 50 + i * 20;
        MDX_RenderModel(&f.ent, &f.model, &matrices[i]);
    }
    MDLX_EndModelBatch();
#ifdef BZ_GLSL_120
    T_EQ(inst.draws, 3); T_EQ(inst.total, 32); T_EQ(block.uploads, 0); T_EQ(inst.uniform_count, 36);
    T_EQ(memcmp(expected[28], inst.uniform_bones, 36 * sizeof(mat4_t)), 0);
#else
    T_EQ(inst.draws, 1); T_EQ(inst.total, 32); T_EQ(block.uploads, 1); T_EQ(block.counts[0], 288);
    T_EQ(memcmp(expected, block.palettes[0], sizeof(expected)), 0); T_EQ(block.bound, 0);
#endif
    MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
}

TEST(mdx_geoset, insufficient_block_capacity_retains_complete_original_draws) {
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE); ri.CvarString = block_settings;
    block_mode = "2"; block = (typeof(block)){.limit = 64}; tr.render_phase = RENDER_PHASE_SOLID;
    MDLX_BeginModelBatch(); FOR_LOOP(i, 4) MDX_RenderModel(&f.ent, &f.model, &f.matrix);
    MDLX_EndModelBatch();
    T_EQ(inst.draws, 1); T_EQ(inst.total, 4); T_EQ(block.uploads, 0); T_ASSERT(inst.uniform_draws[0]);
    MDLX_ShutdownModelBatches(); mdlx.shader = NULL;
}

TEST(mdx_geoset, prepared_packet_transports_match_original_draws_with_sparse_billboards_and_animated_materials) {
    extern void MDLX_TestResetPoseCache(void);
    static packetDraw_t expected[2][16]; uint32_t counts[2] = {0};
    cstring_t modes[] = {"0", "1", "2"};
    FOR_LOOP(transport, 2) FOR_LOOP(mode, 3) {
        geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
        uint32_t storage[4][sizeof(mdxKeyTrack_t) / 4 + 10] = {0};
        mdxKeyTrack_t *alpha = packet_track(storage[0], TDATA_FLOAT1, true);
        mdxKeyTrack_t *color = packet_track(storage[1], TDATA_FLOAT3, false);
        mdxKeyTrack_t *flip = packet_track(storage[2], TDATA_INT1, false);
        mdxKeyTrack_t *motion = packet_track(storage[3], TDATA_FLOAT3, true);
        mdxSequence_t seq = {.interval = {0, 1000}};
        mdxGlobalSequence_t global = {.value = 950};
        mdxTexture_t textures[2] = {0};
        f.model.sequences = &seq; f.model.num_sequences = 1;
        f.model.globalSequences = &global; f.model.num_globalSequences = 1;
        f.model.textures = textures; f.model.num_textures = 2;
        f.node.node_id = 3; f.node.translation = color;
        mdxNode_t child = {.node_id = 51, .parent_id = 3, .flags = MDLXNODE_Billboarded, .translation = motion};
        f.model.nodes[0] = NULL; f.model.nodes[3] = &f.node; f.model.nodes[51] = &child;
        f.model.node_list[1] = &child; f.model.num_nodes = 2;
        int palette[] = {51, -1, MDX_MAX_NODES + 3, 3};
        f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 4;
        mdxGeosetAnim_t anim = {.staticAlpha = 1, .flags = 2, .alphas = alpha, .colors = color};
        f.geo.geosetAnim = &anim;
        mdxTextureAnim_t uv = {.translation = motion, .scale = color}; f.model.textureAnims = &uv;
        f.layers[0].alpha = alpha; f.layers[0].flipbook = flip; f.layers[0].transformId = 0;
        mdxMaterialLayer_t blended = f.layers[0]; blended.blendMode = BLEND_MODE_BLEND;
        mdxMaterial_t mat = {.layers = &blended, .num_layers = 1}; f.mat.next = &mat;
        mdxGeoset_t geo = f.geo; geo.materialID = 1; f.geo.next = &geo;
        packet_mode = modes[mode]; packet_uniform = transport; ri.CvarString = packet_cvars;
        tr.viewDef.time = 317; tr.viewDef.lerpfrac = .41f; tr.render_phase = RENDER_PHASE_SOLID;
        MDLX_TestResetPoseCache(); MDLX_BeginModelBatch(); T_ASSERT(MDLX_BeginPosePreparation(2));
        FOR_LOOP(i, 32) {
            f.ent.frame = 100 + i / 8 * 220; f.ent.oldframe = f.ent.frame - 30; f.matrix.v[12] = i * 13.7f;
            MDLX_QueueEntityPose(&f.ent, &f.model, &f.matrix);
        }
        MDLX_EndPosePreparation();
        if (mode) {
            T_EQ(MDLX_PosePreparationStats().packets, 32);
            T_EQ(MDLX_PosePreparationStats().worker_packets > 0, mode == 2);
            mdxDrawPacket_t const *packet = MDLX_PreparedDraw(&f.ent, &f.model, &f.matrix); T_NOT_NULL(packet);
            T_EQ(packet->layers[0].texture, MDLX_SampleLayerTextureId(&f.model, &f.layers[0], f.ent.frame));
        }
        packet_draw_count = 0; record_packets = true;
        FOR_LOOP(phase, 2) {
            tr.render_phase = phase ? RENDER_PHASE_ALPHA : RENDER_PHASE_SOLID;
            if (phase) MDLX_BeginModelBatch();
            FOR_LOOP(i, 32) {
                f.ent.frame = 100 + i / 8 * 220; f.ent.oldframe = f.ent.frame - 30; f.matrix.v[12] = i * 13.7f;
                MDX_RenderModel(&f.ent, &f.model, &f.matrix);
            }
            MDLX_EndModelBatch();
        }
        record_packets = false; T_EQ(packet_draw_count, 8); T_EQ(inst.total, 64);
        if (!mode) {
            counts[transport] = packet_draw_count; memcpy(expected[transport], packet_draws, sizeof(packet_draws));
        } else {
            T_EQ(packet_draw_count, counts[transport]);
            T_EQ(memcmp(expected[transport], packet_draws, packet_draw_count * sizeof(packetDraw_t)), 0);
        }
        MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
    }
}

TEST(mdx_geoset, packet_cache_keeps_partial_small_excess_and_invalidated_draws_on_demand) {
    extern void MDLX_TestResetPoseCache(void);
    geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
    packet_mode = "2"; packet_uniform = true; ri.CvarString = packet_cvars;
    tr.viewDef.time = 337; tr.render_phase = RENDER_PHASE_SOLID;
    mdxNode_t child = {.node_id = 1, .parent_id = 0};
    f.model.nodes[1] = f.model.node_list[1] = &child; f.model.num_nodes = 2; f.palette = 1;
    mdxPoseMask_t root = {1};
    MDLX_TestResetPoseCache();
    FOR_LOOP(pass, 2) {
        T_ASSERT(MDLX_BeginPosePreparation(2));
        FOR_LOOP(i, 32) T_ASSERT(MDLX_QueuePose(&f.model, &f.matrix, 100 + i, 50 + i, pass ? NULL : root));
        MDLX_EndPosePreparation();
        T_EQ(MDLX_PosePreparationStats().jobs, 32); T_EQ(MDLX_PosePreparationStats().packets, pass ? 32 : 0);
        f.ent.frame = 131; f.ent.oldframe = 81;
        T_EQ(MDLX_PreparedDraw(&f.ent, &f.model, &f.matrix) != NULL, pass != 0);
    }
    packet_mode = "0"; T_ASSERT(MDLX_BeginPosePreparation(2)); MDLX_EndPosePreparation();
    T_NULL(MDLX_PreparedDraw(&f.ent, &f.model, &f.matrix));
    packet_mode = "2";
    FOR_LOOP(change, 5) {
        MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(2));
        FOR_LOOP(i, 32) T_ASSERT(MDLX_QueuePose(&f.model, &f.matrix, 100 + i, 50 + i, NULL));
        if (change == 0) tr.viewDef.time++;
        if (change == 1) tr.viewDef.lerpfrac = .41f;
        if (change == 2) tr.viewDef.viewProjectionMatrix.v[0]++;
        if (change == 3) MDLX_ClearPoseCache();
        if (change == 4) MDLX_ShutdownModelBatches();
        MDLX_EndPosePreparation(); T_EQ(MDLX_PosePreparationStats().packets, 0);
        T_NULL(MDLX_PreparedDraw(&f.ent, &f.model, &f.matrix));
    }
    MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(2));
    FOR_LOOP(i, 8) T_ASSERT(MDLX_QueuePose(&f.model, &f.matrix, 100 + i, 50 + i, NULL));
    MDLX_EndPosePreparation(); T_EQ(MDLX_PosePreparationStats().jobs, 0);
    f.ent.frame = 107; f.ent.oldframe = 57; T_NULL(MDLX_PreparedDraw(&f.ent, &f.model, &f.matrix));
    int palette[128] = {0}; f.geo.matrixPalette = palette; f.geo.num_matrixPalette = 128;
    MDLX_TestResetPoseCache(); T_ASSERT(MDLX_BeginPosePreparation(2));
    FOR_LOOP(i, 544) T_EQ(MDLX_QueuePose(&f.model, &f.matrix, 100 + i, 50 + i, NULL), i < 512);
    MDLX_EndPosePreparation(); mdxPoseStats_t stats = MDLX_PosePreparationStats();
    T_EQ(stats.jobs, 512); T_ASSERT(stats.packets > 0 && stats.packets < 512);
    poses = draws = emits = 0;
    uint32_t actors[] = {0, 511, 543};
    FOR_LOOP(i, 3) {
        f.ent.frame = 100 + actors[i]; f.ent.oldframe = 50 + actors[i];
        T_EQ(MDLX_PreparedDraw(&f.ent, &f.model, &f.matrix) != NULL, i == 0);
        MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        T_EQ(shader.state.boneCount, 128); T_FEQ(drawn_bone.v[15], 1, .00001f);
    }
    T_EQ(draws, 3);
    MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
}

TEST(mdx_geoset, packet_workers_preserve_owner_particle_positions_velocities_and_rng) {
    extern void MDLX_TestResetPoseCache(void);
    vec3_t position[32], velocity[32]; int random = 0;
    cstring_t modes[] = {"0", "1", "2"};
    FOR_LOOP(mode, 3) {
        geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
        packet_mode = modes[mode]; packet_uniform = true; ri.CvarString = packet_cvars;
        tr.viewDef.deltaTime = 100; tr.viewDef.time = 317; tr.render_phase = RENDER_PHASE_SOLID;
        mdxParticleEmitter_t emitter = {.EmissionRate = 10, .LifeSpan = 1, .Speed = 17,
            .Variation = 7, .Latitude = 35, .Length = 3, .Width = 5, .Rows = 1, .Columns = 1};
        f.model.emitters = &emitter;
        R_TestResetParticles(); poses = draws = emits = 0; srand(421);
        MDLX_TestResetPoseCache(); MDLX_BeginModelBatch(); T_ASSERT(MDLX_BeginPosePreparation(2));
        FOR_LOOP(i, 32) {
            f.ent.frame = f.ent.oldframe = i; f.matrix.v[12] = i * 13.7f;
            MDLX_QueueEntityPose(&f.ent, &f.model, &f.matrix);
        }
        MDLX_EndPosePreparation(); T_EQ(R_TestParticleCount(), 0); T_FEQ(emitter.accumulator, 0, .00001f);
        FOR_LOOP(i, 32) {
            f.ent.frame = f.ent.oldframe = i; f.matrix.v[12] = i * 13.7f;
            MDX_RenderModel(&f.ent, &f.model, &f.matrix);
            T_EQ(R_TestParticleCount(), i + 1);
            cparticle_t const *part = R_TestParticle(i);
            if (!mode) { position[i] = part->org; velocity[i] = part->vel; }
            else {
                T_EQ(memcmp(&position[i], &part->org, sizeof(part->org)), 0);
                T_EQ(memcmp(&velocity[i], &part->vel, sizeof(part->vel)), 0);
            }
        }
        MDLX_EndModelBatch(); T_EQ(emits, 32); T_EQ(inst.total, 32); T_EQ(inst.draws, 1);
        if (!mode) random = rand(); else T_EQ(rand(), random);
        MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
    }
    R_TestResetParticles();
}

/* Opt-in CPU diagnostic: real MDX entry point and samplers, GPU submission mocked above. */
TEST(stress_mdx_cpu, animated_geosets) {
    geosetFixture_t f;
    setup_geoset(&f, BLEND_MODE_BLEND);
    uint32_t as[sizeof(mdxKeyTrack_t) / 4 + 64] = {0}, cs[sizeof(mdxKeyTrack_t) / 4 + 128] = {0};
    mdxKeyTrack_t *alpha = (mdxKeyTrack_t *)as, *rgb = (mdxKeyTrack_t *)cs;
    *alpha = (mdxKeyTrack_t){ .keyframeCount = 32, .datatype = TDATA_FLOAT1,
        .linetype = TRACK_LINEAR, .globalSeqId = (uint32_t)-1 };
    *rgb = *alpha; rgb->datatype = TDATA_FLOAT3;
    FOR_LOOP(i, 32) {
        mdxKeyFrame_t *ak = (mdxKeyFrame_t *)((uint8_t *)alpha->values + i * (sizeof(int) + sizeof(float)));
        mdxKeyFrame_t *ck = (mdxKeyFrame_t *)((uint8_t *)rgb->values + i * (sizeof(int) + sizeof(vec3_t)));
        float a = i % 5 ? 1 : 0; vec3_t c = {0.1f + 0.1f * (i % 7), 0.4f, 0.6f};
        ak->time = ck->time = i * 32;
        memcpy(ak->data, &a, sizeof(a)); memcpy(ck->data, &c, sizeof(c));
    }
    mdxSequence_t seq = { .interval = {0, 1024} };
    mdxGeosetAnim_t anim = { .flags = 2, .staticAlpha = 1, .alphas = alpha, .colors = rgb };
    mdxGeoset_t geos[8];
    FOR_LOOP(i, 8) {
        geos[i] = f.geo; geos[i].geosetAnim = &anim; geos[i].next = i < 7 ? &geos[i + 1] : NULL;
    }
    f.model.geosets = geos; f.model.sequences = &seq; f.model.num_sequences = 1;
    tr.render_phase = RENDER_PHASE_ALPHA; poses = draws = alpha_reads = color_reads = 0;
    T_BENCH("200 actors, 8 animated geosets, alpha CPU", 1000, {
        FOR_LOOP(i, 200) {
            f.ent.frame = f.ent.oldframe = (i * 13 + _bi) % 1024;
            MDX_RenderModel(&f.ent, &f.model, &f.matrix);
        }
    });
    T_ASSERT(draws > 0); printf("MDX samples: alpha=%u color=%u draws=%u\n", alpha_reads, color_reads, draws);
    mdlx.shader = NULL;
}

/* Equal authored frames cannot batch actors whose construction holds changed their global-track phase. */
TEST(mdx_geoset, held_global_clocks_survive_packet_workers_and_batch_flushes) {
    cstring_t modes[] = {"0", "1", "2"};
    FOR_LOOP(mode, 3) {
        geosetFixture_t f; setup_geoset(&f, BLEND_MODE_NONE);
        uint32_t values[sizeof(mdxKeyTrack_t) / 4 + 4] = {0};
        mdxKeyTrack_t *track = (mdxKeyTrack_t *)values;
        *track = (mdxKeyTrack_t){.keyframeCount = 2, .globalSeqId = 0,
            .datatype = TDATA_FLOAT1, .linetype = TRACK_LINEAR};
        FOR_LOOP(i, 2) {
            mdxKeyFrame_t *key = (void *)((uint8_t *)track->values + i * (sizeof(int) + sizeof(float)));
            float value = i; key->time = i * 1000; memcpy(key->data, &value, sizeof(value));
        }
        mdxGlobalSequence_t global = {.value = 1000};
        f.model.globalSequences = &global; f.model.num_globalSequences = 1;
        f.layers[0].alpha = track;
        renderEntity_t actors[32];
        FOR_LOOP(i, 32) {
            actors[i] = f.ent; actors[i].number = MAX_GAME_ENTITIES - 200 - i;
            actors[i].generation = 0x574336u + mode; actors[i].flags = RF_MDX_ANIMATION_PAUSED;
            actors[i].frame = actors[i].oldframe = 100 + i;
            tr.viewDef.time = i < 16 ? 200 : 400; MDLX_UpdateEntityAnimationPause(&actors[i]);
        }
        tr.viewDef.time = 600; tr.render_phase = RENDER_PHASE_SOLID;
        packet_mode = modes[mode]; packet_uniform = true; ri.CvarString = packet_cvars;
        MDLX_TestResetPoseCache(); MDLX_BeginModelBatch(); T_ASSERT(MDLX_BeginPosePreparation(2));
        FOR_LOOP(i, 32) MDLX_QueueEntityPose(&actors[i], &f.model, &f.matrix);
        MDLX_EndPosePreparation(); T_EQ(MDLX_PosePreparationStats().jobs, 32);
        if (mode == 2) T_ASSERT(MDLX_PosePreparationStats().worker_packets >= 8);
        record_packets = true; packet_draw_count = 0;
        FOR_LOOP(i, 32) MDX_RenderModel(&actors[i], &f.model, &f.matrix);
        MDLX_EndModelBatch(); record_packets = false;
        T_EQ(packet_draw_count, 2); T_EQ(inst.total, 32);
        T_EQ(packet_draws[0].copies, 16); T_EQ(packet_draws[1].copies, 16);
        T_FEQ(packet_draws[0].alpha, .2f, .00001f); T_FEQ(packet_draws[1].alpha, .4f, .00001f);
        mdxAnimationTimeScope_t scope; MDLX_PushEntityAnimationTime(NULL, &scope);
        float alpha = 0; MDLX_GetModelKeytrackValue(&f.model, track, 100, &alpha);
        T_FEQ(alpha, .6f, .00001f); MDLX_PopEntityAnimationTime(&scope);
        MDLX_ShutdownPoseWorkers(); MDLX_ShutdownModelBatches(); MDLX_TestResetPoseCache(); mdlx.shader = NULL;
    }
}
