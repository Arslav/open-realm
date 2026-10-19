#include "r_mdx.h"
#include "../w3m/r_war3map.h"
#include "renderer/r_emit.h"
#include "renderer/r_local.h"
#include "renderer/r_shader.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>

#define MDLX_STACK_DRAW_ORDER 64 // layers; cover typical models without a per-draw heap allocation
#define MDLX_INSTANCE_LIMIT 256 // matrices; caps each ordered packet at 16 KiB without dropping overflow actors
#define MDLX_BONE_TEX_WIDTH 1024 // texels; within GL3/GLES3 texture-size limits
#define MDLX_BONE_TEXELS 3 // RGBA32F rows; an affine matrix's final row is always (0, 0, 0, 1)
#define BZ_MDX_BLOCK_MATRICES 1024 // matrices; caps optional std140 storage at 64 KiB, further bounded by GL limits
#define BZ_MDX_BLOCK_BINDING 0 // uniform-buffer slot; WC3 owns this binding during a model packet and clears it afterward
#ifdef __ANDROID__
#define BZ_MDX_UNIFORM_AUTO_DEFAULT "1"
#define BZ_MDX_EFFECT_INSTANCE_DEFAULT "2"
#define BZ_MDX_PALETTE_UBO_DEFAULT "2" // mode; measured Honor gain enables larger ordered palettes on Android
#else
#define BZ_MDX_UNIFORM_AUTO_DEFAULT "0"
#define BZ_MDX_EFFECT_INSTANCE_DEFAULT "0"
#define BZ_MDX_PALETTE_UBO_DEFAULT "0" // mode; other platforms retain their unmeasured original bone transport
#endif
typedef struct {
    modelProg_t base;
    int offsets[BZ_BONE_PALETTE_MAX], texture;
    vec2_t size;
} mdxInstanceShader_t;
static mdxInstanceShader_t instance_prog;
static shader_desc_t instance_desc;
static char *instance_body;
typedef struct {
    modelProg_t base;
    int stride;
} mdxUniformShader_t;
static mdxUniformShader_t uniform_prog;
static shader_desc_t uniform_desc;
static char *uniform_body;
typedef enum { MDX_PALETTE_TEXTURE, MDX_PALETTE_UNIFORM, MDX_PALETTE_BLOCK } mdxPalette_t;
static struct {
    mdxUniformShader_t shader;
    shader_desc_t desc;
    char *body;
    GLuint vbo;
    GLint limit;
    mat4_t *bones;
} block_palette;
static vec4_t *instance_bones;
static uint32_t instance_bone_capacity;
#define BZ_MDX_PACKET_BYTES (4u * 1024u * 1024u) // bytes; bounded optional CPU packet arena, reserved before workers run
static struct { uint8_t *data; uint32_t used; bool failed; } draw_packets;
static mdxDrawPacket_t const *draw_packet;
typedef struct {
    mdxModel_t const *model;
    mdxMaterial_t const *material;
    mdxMaterialLayer_t const *layer;
    uint32_t frame;
} mdxLayerQuery_t;
static float MDLX_EvaluateGeosetAlpha(mdxModel_t const *model, mdxGeoset_t const *geoset, uint32_t frame);
static uint32_t MDLX_EvaluateLayerTextureId(mdxModel_t const *model, mdxMaterialLayer_t const *layer, uint32_t frame);

static char const instance_skin[] =
    "#define BZ_MODEL_BONE(i) instance_bone(i)\n"
    "vec4 bone_texel(float i) {\n"
    "  vec2 uv = (vec2(mod(i, u_boneSize.x), floor(i / u_boneSize.x)) + vec2(0.5)) / u_boneSize;\n"
    "#if __VERSION__ < 130\n"
    "  return texture2DLod(u_boneTexture, uv, 0.0);\n"
    "#else\n"
    "  return textureLod(u_boneTexture, uv, 0.0);\n"
    "#endif\n"
    "}\n"
    "mat4 instance_bone(int i) {\n"
    "  float at = (a_instance[3].w - 1.0 + float(u_boneOffsets[i])) * 3.0;\n"
    "  vec4 x = bone_texel(at), y = bone_texel(at+1.0), z = bone_texel(at+2.0);\n"
    "  return mat4(vec4(x.x,y.x,z.x,0.0), vec4(x.y,y.y,z.y,0.0),\n"
    "              vec4(x.z,y.z,z.z,0.0), vec4(x.w,y.w,z.w,1.0));\n"
    "}\n";

/* The game-owned variant extends the typed model prefix; shared MDX/M2/M3 uniform state stays unchanged. */
static modelProg_t *MDLX_InstanceShader(void) {
    if (instance_body) return &instance_prog.base;
    instance_desc = sd_model; instance_desc.Name = "mdx_instances";
    size_t base = offsetof(mdxInstanceShader_t, base.state), n = 0;
    while (instance_desc.Uniforms[n].name) n++;
    instance_desc.Uniforms[n++] = (shaderUniform_t){ .name = "u_boneOffsets", .type = UT_INT,
        .precision = PRECISION_HIGH, .offset = offsetof(mdxInstanceShader_t, offsets) - base,
        .count = BZ_BONE_PALETTE_MAX, .count_offset = offsetof(modelState_t, boneCount), .counted = true };
    instance_desc.Uniforms[n++] = (shaderUniform_t){ .name = "u_boneTexture", .type = UT_SAMPLER_2D,
        .precision = PRECISION_HIGH, .offset = offsetof(mdxInstanceShader_t, texture) - base };
    instance_desc.Uniforms[n++] = (shaderUniform_t){ .name = "u_boneSize", .type = UT_FLOAT_VEC2,
        .precision = PRECISION_HIGH, .offset = offsetof(mdxInstanceShader_t, size) - base };
    size_t bytes = sizeof(instance_skin) + strlen(sd_model.VertexBody);
    instance_body = malloc(bytes);
    if (!instance_body) { fprintf(stderr, "MDX: unable to allocate instanced shader source\n"); Sys_Exit(EXIT_FAILURE); }
    snprintf(instance_body, bytes, "%s%s", instance_skin, sd_model.VertexBody);
    instance_desc.VertexBody = instance_body;
    /* Numeric RGBA32F bones need highp sampling too. The portable generator deliberately
     * uses lowp sampler defaults and does not emit per-uniform precision metadata. */
    char defines[384];
    snprintf(defines, sizeof(defines), "%s#ifdef GL_ES\nprecision highp sampler2D;\n#endif\n", R_ShaderDefines(true));
    R_LoadShaderState(&(shaderLoad_t){ .desc = &instance_desc, .defines = defines,
        .prog = &instance_prog.base.prog, .state = &instance_prog.base.state,
        .state_size = sizeof(instance_prog) - base });
    instance_prog.base.state.alphaCutoff = 0.5f; instance_prog.texture = 3;
    return &instance_prog.base;
}

/* Small packets fit complete independent palettes in the existing 128-matrix uniform transport. */
static modelProg_t *MDLX_UniformInstanceShader(void) {
    if (uniform_body) return &uniform_prog.base;
    static char const prefix[] = "#define BZ_MODEL_BONE(i) u_bones[int(a_instance[3].w-1.0)*u_instanceBoneStride+(i)]\n";
    uniform_desc = sd_model; uniform_desc.Name = "mdx_uniform_instances";
    size_t base = offsetof(mdxUniformShader_t, base.state), n = 0;
    while (uniform_desc.Uniforms[n].name) n++;
    uniform_desc.Uniforms[n] = (shaderUniform_t){.name = "u_instanceBoneStride", .type = UT_INT,
        .precision = PRECISION_HIGH, .offset = offsetof(mdxUniformShader_t, stride) - base};
    size_t bytes = sizeof(prefix) + strlen(sd_model.VertexBody);
    uniform_body = malloc(bytes);
    if (!uniform_body) { fprintf(stderr, "MDX: unable to allocate uniform instance shader source\n"); Sys_Exit(EXIT_FAILURE); }
    snprintf(uniform_body, bytes, "%s%s", prefix, sd_model.VertexBody);
    uniform_desc.VertexBody = uniform_body;
    R_LoadShaderState(&(shaderLoad_t){.desc = &uniform_desc, .defines = R_ShaderDefines(true),
        .prog = &uniform_prog.base.prog, .state = &uniform_prog.base.state,
        .state_size = sizeof(uniform_prog) - base});
    uniform_prog.base.state.alphaCutoff = 0.5f;
    return &uniform_prog.base;
}

/* Query once per context; GLSL 120 has no core block interface, so explicitly retain the original transport. */
static uint32_t MDLX_BlockPaletteLimit(void) {
    if (block_palette.limit) return MAX(0, block_palette.limit);
#ifdef BZ_GLSL_120
    fprintf(stderr, "MDX: r_mdx_palette_ubo requires GLSL 140/GLES3; retaining the original bone transport\n");
#else
    GLint bytes = 0, blocks = 0, bindings = 0;
    R_Call(glGetIntegerv, GL_MAX_UNIFORM_BLOCK_SIZE, &bytes);
    R_Call(glGetIntegerv, GL_MAX_VERTEX_UNIFORM_BLOCKS, &blocks);
    R_Call(glGetIntegerv, GL_MAX_UNIFORM_BUFFER_BINDINGS, &bindings);
    block_palette.limit = MIN(BZ_MDX_BLOCK_MATRICES, MAX(0, bytes) / sizeof(mat4_t));
    if (block_palette.limit >= BZ_BONE_PALETTE_MAX && blocks > 0 && bindings > BZ_MDX_BLOCK_BINDING)
        return block_palette.limit;
    fprintf(stderr, "MDX: uniform blocks insufficient (bytes=%d blocks=%d bindings=%d); retaining the original bone transport\n", bytes, blocks, bindings);
#endif
    block_palette.limit = -1;
    return 0;
}

/* Keep the shared vertex/depth math verbatim; only the palette source moves out of the default uniform block. */
static modelProg_t *MDLX_BlockInstanceShader(void) {
    if (block_palette.body) return &block_palette.shader.base;
    block_palette.desc = sd_model; block_palette.desc.Name = "mdx_block_instances";
    uint32_t n = 0;
    FOR_LOOP(i, MAX_SHADER_UNIFORMS) {
        shaderUniform_t field = sd_model.Uniforms[i];
        if (!field.name) break;
        if (strcmp(field.name, "u_bones")) block_palette.desc.Uniforms[n++] = field;
    }
    size_t base = offsetof(mdxUniformShader_t, base.state);
    block_palette.desc.Uniforms[n++] = (shaderUniform_t){.name = "u_instanceBoneStride", .type = UT_INT,
        .precision = PRECISION_HIGH, .offset = offsetof(mdxUniformShader_t, stride) - base};
    memset(block_palette.desc.Uniforms + n, 0, (MAX_SHADER_UNIFORMS - n) * sizeof(shaderUniform_t));
    char prefix[384];
    snprintf(prefix, sizeof(prefix), "layout(std140, column_major) uniform MDXBoneBlock { mat4 u_instanceBones[%d]; };\n#define BZ_MODEL_BONE(i) u_instanceBones[int(a_instance[3].w-1.0)*u_instanceBoneStride+(i)]\n", block_palette.limit);
    size_t bytes = strlen(prefix) + strlen(sd_model.VertexBody) + 1;
    block_palette.body = malloc(bytes);
    block_palette.bones = calloc(block_palette.limit, sizeof(mat4_t));
    if (!block_palette.body || !block_palette.bones) {
        fprintf(stderr, "MDX: unable to allocate uniform block palette/source\n"); Sys_Exit(EXIT_FAILURE);
    }
    snprintf(block_palette.body, bytes, "%s%s", prefix, sd_model.VertexBody);
    block_palette.desc.VertexBody = block_palette.body;
    modelProg_t *shader = &block_palette.shader.base;
    R_LoadShaderState(&(shaderLoad_t){.desc = &block_palette.desc, .defines = R_ShaderDefines(true),
        .prog = &shader->prog, .state = &shader->state, .state_size = sizeof(mdxUniformShader_t) - base});
    GLuint index = glGetUniformBlockIndex(shader->prog.progid, "MDXBoneBlock");
    GLint size = 0;
    if (index != GL_INVALID_INDEX) R_Call(glGetActiveUniformBlockiv, shader->prog.progid, index, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
    if (index == GL_INVALID_INDEX || size != block_palette.limit * sizeof(mat4_t)) {
        fprintf(stderr, "MDX: invalid linked bone block (index=%u bytes=%d expected=%zu)\n", index, size, block_palette.limit * sizeof(mat4_t));
        Sys_Exit(EXIT_FAILURE);
    }
    R_Call(glUniformBlockBinding, shader->prog.progid, index, BZ_MDX_BLOCK_BINDING);
    shader->state.alphaCutoff = 0.5f;
    return shader;
}

typedef enum {
    MDX_INSTANCE_OK, MDX_INSTANCE_BONES, MDX_INSTANCE_EMITTERS, MDX_INSTANCE_RIBBONS,
    MDX_INSTANCE_LIGHTS, MDX_INSTANCE_EFFECT, MDX_INSTANCE_SELECTED, MDX_INSTANCE_ATTACHMENT,
    MDX_INSTANCE_SCALE, MDX_INSTANCE_PALETTE, MDX_INSTANCE_DEPTH, MDX_INSTANCE_ALPHA,
    MDX_INSTANCE_EMPTY, MDX_INSTANCE_REASONS
} mdxInstanceBlock_t;
typedef enum {
    MDX_FLUSH_END, MDX_FLUSH_BLOCKED, MDX_FLUSH_CAPACITY, MDX_FLUSH_MODEL,
    MDX_FLUSH_FLAGS, MDX_FLUSH_TEAM, MDX_FLUSH_SKIN, MDX_FLUSH_TINT,
    MDX_FLUSH_MATERIAL, MDX_FLUSH_REASONS
} mdxBatchFlush_t;
typedef struct {
    uint32_t scopes, attempted, accepted, packets, singleton, instanced, largest;
    uint32_t uniform_packets, palette_uploads, effect_actors, stream_packets, vao_draws, attrib_sets;
    uint32_t block_packets, block_uploads, block_bytes, bone_limit;
    uint32_t reject[MDX_INSTANCE_REASONS], flush[MDX_FLUSH_REASONS];
} mdxBatchStats_t;
static struct { uint32_t start; mdxBatchStats_t phase[3]; } batch_stats;
static char const *const batch_block_names[] = {
    "ok", "bones", "emitters", "ribbons", "lights", "effect", "selected", "attachment",
    "scale", "palette", "depth", "alpha", "empty"
};
static char const *const batch_flush_names[] = {"end", "blocked", "capacity", "model", "flags", "team", "skin", "tint", "material"};

/* Opt-in scope totals distinguish work submitted immediately from packets sent to the GPU. */
static void MDLX_ReportModelBatches(void) {
    uint32_t now = SDL_GetTicks(), elapsed = now - batch_stats.start;
    if (elapsed < 1000) return;
    static char const *const phases[] = {"solid", "lights", "alpha"};
    FOR_LOOP(i, 3) {
        mdxBatchStats_t const *s = batch_stats.phase + i;
        if (!s->scopes) continue;
        char reasons[768]; size_t used = 0;
        FOR_LOOP(j, MDX_INSTANCE_REASONS - 1)
            used += snprintf(reasons + used, sizeof(reasons) - used, " reject_%s=%u", batch_block_names[j+1], s->reject[j+1]);
        FOR_LOOP(j, MDX_FLUSH_REASONS)
            used += snprintf(reasons + used, sizeof(reasons) - used, " flush_%s=%u", batch_flush_names[j], s->flush[j]);
        fprintf(stderr, "[MDX_BATCH] phase=%s scopes=%u attempted=%u accepted=%u packets=%u singleton=%u instanced=%u largest=%u uniform_packets=%u palette_uploads=%u effect_actors=%u stream_packets=%u vao_draws=%u attrib_sets=%u block_packets=%u block_uploads=%u block_bytes=%u bone_limit=%u elapsed_ms=%u%s\n", phases[i], s->scopes, s->attempted, s->accepted, s->packets, s->singleton, s->instanced, s->largest, s->uniform_packets, s->palette_uploads, s->effect_actors, s->stream_packets, s->vao_draws, s->attrib_sets, s->block_packets, s->block_uploads, s->block_bytes, s->bone_limit, elapsed, reasons);
    }
    memset(&batch_stats, 0, sizeof(batch_stats)); batch_stats.start = now;
}

static struct {
    bool enabled, auto_uniform, flushing;
    mdxPalette_t transport;
    uint32_t effect_mode, submit_mode, bone_limit;
    render_phase_t phase;
    uint32_t count, drawing, capacity, time;
    GLuint vbo, texture;
    GLint maxsize;
    uint32_t rows, span, frames[MDLX_INSTANCE_LIMIT][2];
    mdxModel_t const *model;
    mdxPoseMask_t mask;
    uint16_t bone_offsets[MDX_MAX_NODES];
    renderEntity_t entity;
    mat4_t matrices[MDLX_INSTANCE_LIMIT];
    mdxDrawPacket_t const *packets[MDLX_INSTANCE_LIMIT];
    mdxBatchStats_t *stats;
} model_batch;
static void MDLX_DrawBoneMask(renderEntity_t const *ent, mdxModel_t const *model, mdxPoseMask_t mask);

/* These lookups are owner-only; workers call the raw samplers and never read the active draw pointer. */
static mdxGeosetState_t const *MDLX_GeosetState(mdxModel_t const *model, mdxGeoset_t const *geo, uint32_t frame) {
    if (!draw_packet || draw_packet->model != model || draw_packet->frame != frame) return NULL;
    FOR_EACH_ARRAY(mdxGeosetState_t const, state, draw_packet->geosets)
        if (state->geoset == geo) return state;
    return NULL;
}
static mdxLayerState_t const *MDLX_LayerState(mdxLayerQuery_t const *query) {
    if (!draw_packet || draw_packet->model != query->model || draw_packet->frame != query->frame) return NULL;
    FOR_EACH_ARRAY(mdxGeosetState_t const, geo, draw_packet->geosets) {
        if (!geo->material || geo->color.w < EPSILON || (query->material && geo->material != query->material)) continue;
        FOR_LOOP(i, geo->material->num_layers)
            if (&geo->material->layers[i] == query->layer) return draw_packet->layers + geo->layerofs + i;
    }
    return NULL;
}

static void MDLX_StoreInstanceBone(uint32_t index, mat4_t const *matrix) {
    vec4_t *rows = instance_bones + index * MDLX_BONE_TEXELS;
    FOR_LOOP(i, MDLX_BONE_TEXELS)
        rows[i] = (vec4_t){matrix->v[i], matrix->v[i+4], matrix->v[i+8], matrix->v[i+12]};
}

/* Select the cheaper transport only when every complete geoset palette fits; keep packet boundaries intact. */
static bool MDLX_PacketFitsUniforms(uint32_t actors) {
    if (!model_batch.auto_uniform) return false;
    FOR_EACH_LIST(mdxGeoset_t, geo, model_batch.model->geosets) {
        uint32_t count = MIN((uint32_t)geo->num_matrixPalette, BZ_BONE_PALETTE_MAX);
        if (!count || count * actors > BZ_BONE_PALETTE_MAX) return false;
    }
    return true;
}

/* Flush before water, incompatible draws or release; a singleton retains the ordinary shader/VAO path. */
static void MDLX_FlushModelBatch(mdxBatchFlush_t reason) {
    if (!model_batch.count) return;
    bool enabled = model_batch.enabled;
    mdxPalette_t transport = model_batch.transport;
    uint32_t count = model_batch.count;
    if (model_batch.stats) {
        mdxBatchStats_t *s = model_batch.stats;
        s->packets++; s->singleton += count == 1; s->instanced += count > 1 ? count : 0;
        s->largest = MAX(s->largest, count); s->flush[reason]++;
    }
    mdxDrawPacket_t const *saved_packet = draw_packet;
    render_phase_t phase = tr.render_phase;
    modelProg_t *saved = mdlx.shader;
    renderEntity_t entity = model_batch.entity;
    mdxAnimationTimeScope_t scope; MDLX_PushEntityAnimationTime(&entity, &scope);
    entity.flags &= ~(RF_HAS_LUMBER | RF_HAS_GOLD); /* The queued frames have already been remapped. */
    model_batch.enabled = false; model_batch.count = 0;
    tr.render_phase = model_batch.phase;
    if (count > 1) {
        if (model_batch.transport == MDX_PALETTE_TEXTURE && MDLX_PacketFitsUniforms(count))
            model_batch.transport = MDX_PALETTE_UNIFORM;
        if (model_batch.stats) {
            model_batch.stats->uniform_packets += model_batch.transport == MDX_PALETTE_UNIFORM;
            model_batch.stats->block_packets += model_batch.transport == MDX_PALETTE_BLOCK;
            model_batch.stats->bone_limit = MAX(model_batch.stats->bone_limit, model_batch.bone_limit);
        }
        uint32_t span = model_batch.span;
        uint32_t rows = (count * span * MDLX_BONE_TEXELS + MDLX_BONE_TEX_WIDTH - 1) / MDLX_BONE_TEX_WIDTH;
        uint32_t needed = rows * MDLX_BONE_TEX_WIDTH;
        if (needed > instance_bone_capacity) {
            vec4_t *next = calloc(needed, sizeof(*next));
            if (!next) { fprintf(stderr, "MDX: unable to allocate %u instance bone texels\n", needed); Sys_Exit(EXIT_FAILURE); }
            free(instance_bones); instance_bones = next; instance_bone_capacity = needed;
        }
        FOR_LOOP(i, count) {
            uint32_t base = i * span;
            mdxDrawPacket_t const *packet = model_batch.packets[i];
            if (!packet) MDLX_BindBoneSubset(model_batch.model, &model_batch.matrices[i], model_batch.frames[i][1], model_batch.frames[i][0], model_batch.mask);
            mat4_t identity; Matrix4_identity(&identity); MDLX_StoreInstanceBone(base, &identity);
            FOR_LOOP(j, model_batch.model->num_nodes) {
                uint32_t id = model_batch.model->node_list[j]->node_id;
                if (!model_batch.bone_offsets[id] || (packet && model_batch.transport != MDX_PALETTE_TEXTURE)) continue;
                if (packet)
                    memcpy(instance_bones + (base + model_batch.bone_offsets[id]) * MDLX_BONE_TEXELS, packet->rows + j * MDLX_BONE_TEXELS, MDLX_BONE_TEXELS * sizeof(vec4_t));
                else MDLX_StoreInstanceBone(base + model_batch.bone_offsets[id], &node_matrices[id]);
            }
            /* Affine placement has w=1; the shared shader restores it after reading this exact integer base. */
            model_batch.matrices[i].v[15] = (model_batch.transport != MDX_PALETTE_TEXTURE ? i : base) + 1;
        }
        if (model_batch.transport == MDX_PALETTE_TEXTURE) {
            R_Call(glActiveTexture, GL_TEXTURE3);
            if (!model_batch.texture) R_Call(glGenTextures, 1, &model_batch.texture);
            R_Call(glBindTexture, GL_TEXTURE_2D, model_batch.texture);
            bool resize = rows > model_batch.rows;
            if (resize)
                model_batch.rows = MIN((uint32_t)model_batch.maxsize, rows * 2);
            if (resize || (ri.CvarString && R_CvarEnabled("r_mdx_bone_orphan", "0")))
                R_Call(glTexImage2D, GL_TEXTURE_2D, 0, GL_RGBA32F, MDLX_BONE_TEX_WIDTH, model_batch.rows, 0, GL_RGBA, GL_FLOAT, NULL);
            if (resize) {
                R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                R_Call(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            R_Call(glTexSubImage2D, GL_TEXTURE_2D, 0, 0, 0, MDLX_BONE_TEX_WIDTH, rows, GL_RGBA, GL_FLOAT, instance_bones);
            if (model_batch.stats) model_batch.stats->palette_uploads++;
            R_Call(glActiveTexture, GL_TEXTURE0);
        }
        if (model_batch.submit_mode) MDX_PrepareModelInstances(model_batch.model);
        if (!model_batch.submit_mode && !model_batch.vbo) R_Call(glGenBuffers, 1, &model_batch.vbo);
        if (model_batch.submit_mode && model_batch.stats) model_batch.stats->stream_packets++;
        uint32_t stream = model_batch.submit_mode ? model_batch.model->buffers[BZ_MDX_INSTANCE_BUFFER] : model_batch.vbo;
        R_Call(glBindBuffer, GL_ARRAY_BUFFER, stream);
        R_Call(glBufferData, GL_ARRAY_BUFFER, count * sizeof(mat4_t), model_batch.matrices, GL_STREAM_DRAW);
        mdlx.shader = model_batch.transport == MDX_PALETTE_BLOCK ? MDLX_BlockInstanceShader() :
            model_batch.transport == MDX_PALETTE_UNIFORM ? MDLX_UniformInstanceShader() : MDLX_InstanceShader();
        instance_prog.size = (vec2_t){MDLX_BONE_TEX_WIDTH, model_batch.rows};
        mdlx.shader->state.grassParams = (mat4_t){0};
        model_batch.drawing = count;
    }
    /* A packet draws geometry only; each source actor already advanced its effects on the owner. */
    model_batch.flushing = true;
    MDX_RenderModel(&entity, model_batch.model, &model_batch.matrices[0]);
    if (model_batch.drawing && model_batch.transport == MDX_PALETTE_BLOCK)
        R_Call(glBindBufferBase, GL_UNIFORM_BUFFER, BZ_MDX_BLOCK_BINDING, 0);
    model_batch.flushing = false;
    model_batch.drawing = 0; mdlx.shader = saved; model_batch.enabled = enabled; tr.render_phase = phase;
    model_batch.transport = transport; draw_packet = saved_packet;
    MDLX_PopEntityAnimationTime(&scope);
}

/* Only whole-scene game-renderer scopes defer geometry; direct UI/socket/model calls stay immediate. */
void MDLX_BeginModelBatch(void) {
    MDLX_EndModelBatch(); draw_packet = NULL;
    model_batch.phase = tr.render_phase;
    model_batch.enabled = tr.render_phase != RENDER_PHASE_LIGHTS &&
        !(tr.viewDef.rdflags & RDF_NOWORLDMODEL) && R_CvarEnabled("r_mdx_instancing", "1");
    model_batch.effect_mode = model_batch.enabled ? MAX(0, MIN(2, atoi(ri.CvarString("r_mdx_effect_instancing", BZ_MDX_EFFECT_INSTANCE_DEFAULT)))) : 0;
    /* Keep the no-gain automatic trial selectable; mode 2 also bounds packets to uniform palette capacity. */
    model_batch.transport = model_batch.enabled && (model_batch.effect_mode == 2 || R_CvarEnabled("r_mdx_uniform_instancing", "0"))
        ? MDX_PALETTE_UNIFORM : MDX_PALETTE_TEXTURE;
    model_batch.bone_limit = BZ_BONE_PALETTE_MAX;
    uint32_t block_mode = MAX(0, MIN(2, atoi(ri.CvarString("r_mdx_palette_ubo", BZ_MDX_PALETTE_UBO_DEFAULT))));
    if (model_batch.enabled && block_mode) {
        uint32_t limit = MDLX_BlockPaletteLimit();
        if (limit) {
            model_batch.transport = MDX_PALETTE_BLOCK;
            model_batch.bone_limit = block_mode == 1 ? MIN(limit, BZ_BONE_PALETTE_MAX) : limit;
        }
    }
    model_batch.submit_mode = MAX(0, MIN(2, atoi(ri.CvarString("r_mdx_submit_cache", "0"))));
    model_batch.auto_uniform = model_batch.enabled && R_CvarEnabled("r_mdx_uniform_auto", BZ_MDX_UNIFORM_AUTO_DEFAULT);
    if (model_batch.enabled && model_batch.transport == MDX_PALETTE_TEXTURE && !model_batch.maxsize) {
        R_Call(glGetIntegerv, GL_MAX_TEXTURE_SIZE, &model_batch.maxsize);
        if (model_batch.maxsize < MDLX_BONE_TEX_WIDTH) {
            fprintf(stderr, "MDX: texture limit %d is too small for instanced bones; using immediate models\n", model_batch.maxsize);
            model_batch.maxsize = -1;
        }
    }
    model_batch.enabled &= model_batch.transport != MDX_PALETTE_TEXTURE || model_batch.maxsize > 0;
    if (ri.CvarString && R_CvarEnabled("r_mdx_stats", "0")) {
        if (model_batch.enabled) {
            if (!batch_stats.start) batch_stats.start = SDL_GetTicks();
            model_batch.stats = batch_stats.phase + model_batch.phase;
        }
    } else memset(&batch_stats, 0, sizeof(batch_stats));
}
void MDLX_EndModelBatch(void) {
    MDLX_FlushModelBatch(MDX_FLUSH_END); model_batch.enabled = false; draw_packet = NULL;
    if (model_batch.stats) {
        model_batch.stats->scopes++; model_batch.stats = NULL;
        MDLX_ReportModelBatches();
    }
}
void MDLX_ShutdownModelBatches(void) {
    MDLX_EndModelBatch();
    if (model_batch.vbo) R_Call(glDeleteBuffers, 1, &model_batch.vbo);
    if (model_batch.texture) R_Call(glDeleteTextures, 1, &model_batch.texture);
    if (block_palette.vbo) R_Call(glDeleteBuffers, 1, &block_palette.vbo);
    R_DeleteShader(&block_palette.shader.base.prog);
    free(block_palette.body); free(block_palette.bones); memset(&block_palette, 0, sizeof(block_palette));
    model_batch.vbo = model_batch.texture = model_batch.rows = 0; model_batch.maxsize = 0;
    R_DeleteShader(&instance_prog.base.prog);
    R_DeleteShader(&uniform_prog.base.prog);
    memset(&instance_prog, 0, sizeof(instance_prog));
    memset(&uniform_prog, 0, sizeof(uniform_prog));
    free(instance_body); instance_body = NULL;
    free(uniform_body); uniform_body = NULL;
    free(instance_bones); instance_bones = NULL; instance_bone_capacity = 0;
    MDLX_ClearPoseCache(); free(draw_packets.data); memset(&draw_packets, 0, sizeof(draw_packets));
}
texture_t const *MDLX_GetTexture(mdxModel_t const *model,
                                 uint32_t teamID,
                                 uint32_t textureID,
                                 uint32_t replaceableID,
                                 texture_t const *overrideTexture, uint32_t overrideSlot) {
    mdxTexture_t const *modeltex = &model->textures[textureID];
    switch (replaceableID) {
        case TEXREPL_TEAMCOLOR: return tr.texture[TEX_TEAM_COLOR + teamID];
        case TEXREPL_TEAMGLOW: return tr.texture[TEX_TEAM_GLOW + teamID];
        default:
            if (replaceableID != TEXREPL_NONE && overrideTexture &&
                (!overrideSlot || replaceableID == overrideSlot)) {
                return overrideTexture;
            }
            return R_FindTextureByID(modeltex->texid);
    }
}

bool MDLX_SetLayerBlend(mdxMaterialLayer_t const *layer, uint32_t layerID) {
    R_SetAlphaKeyState(false);
#ifdef USE_SHADOWMAPS
    switch (tr.render_phase == RENDER_PHASE_LIGHTS ? (int)layer->blendMode : -1) {
        case BLEND_MODE_BLEND:
        case BLEND_MODE_ADD:
        case BLEND_MODE_ADDALPHA:
        case BLEND_MODE_MODULATE:
        case BLEND_MODE_MODULATE_2X:
            return false;
    }
#endif
    switch (layer->blendMode) {
        case BLEND_MODE_NONE:
            R_Call(glDisable, GL_BLEND);
            if (layerID == 0) {
                R_Call(glBlendFunc, GL_ONE, GL_ZERO);
            } else {
                R_Call(glBlendFunc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            R_Call(glDepthMask, GL_TRUE);
            break;
        case BLEND_MODE_ALPHAKEY:
            mdlx.shader->state.alphaKey = 1;
            R_SetAlphaKeyState(true);
            break;
        case BLEND_MODE_BLEND:
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_ADD:
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_ONE, GL_ONE);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_ADDALPHA:
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_SRC_ALPHA, GL_ONE);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_MODULATE:
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_DST_COLOR, GL_ZERO);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_MODULATE_2X:
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_DST_COLOR, GL_SRC_COLOR);
            R_Call(glDepthMask, GL_FALSE);
            break;
        default:
            R_Call(glDisable, GL_BLEND);
            R_Call(glBlendFunc, GL_ONE, GL_ZERO);
            R_Call(glDepthMask, GL_TRUE);
            break;
    }
    return true;
}

void MDLX_ApplyLayerFlags(mdxMaterialLayer_t const *layer) {
    if (layer->flags & MODEL_GEO_TWOSIDED) {
        R_Call(glDisable, GL_CULL_FACE);
    }
    if (layer->flags & MODEL_GEO_NO_DEPTH_TEST) {
        R_Call(glDisable, GL_DEPTH_TEST);
    }
    if (layer->flags & MODEL_GEO_NO_DEPTH_SET) {
        R_Call(glDepthMask, GL_FALSE);
    }
}

static bool MDLX_IsBlendedLayer(mdxMaterialLayer_t const *layer) {
    return layer->blendMode >= BLEND_MODE_BLEND;
}

static bool MDLX_MaterialHasPass(mdxMaterial_t const *material, bool blendedPass) {
    if (!material) {
        return false;
    }
    FOR_LOOP(layerID, material->num_layers) {
        if (MDLX_IsBlendedLayer(&material->layers[layerID]) == blendedPass) {
            return true;
        }
    }
    return false;
}

typedef struct mdxGeosetDrawOrder_s {
    mdxGeoset_t const *geoset;
    mdxMaterial_t const *material;
    int priority;
    uint32_t layer, order;
} mdxGeosetDrawOrder_t;

static mdxMaterial_t *MDLX_GetMaterialAtIndex(mdxGeoset_t const *geoset, mdxModel_t const *model) {
    mdxMaterial_t *material = model->materials;
    for (uint32_t materialID = geoset->materialID; materialID > 0; materialID--) {
        material = material->next;
    }
    return material;
}

static int MDLX_CompareGeosetDrawOrder(void const *a, void const *b) {
    mdxGeosetDrawOrder_t const *lhs = a;
    mdxGeosetDrawOrder_t const *rhs = b;

    if (lhs->priority != rhs->priority) {
        return lhs->priority < rhs->priority ? -1 : 1;
    }
    /* Alpha-blended bases precede additive overlays at the same priority. File order
     * alone let ShimmeringPortal's final white disk cover its textured layers. */
    BLEND_MODE left = lhs->material->layers[lhs->layer].blendMode;
    BLEND_MODE right = rhs->material->layers[rhs->layer].blendMode;
    if (left != right)
        return left < right ? -1 : 1;
    if (lhs->order != rhs->order) {
        return lhs->order < rhs->order ? -1 : 1;
    }
    return 0;
}

/* Visibility must not evaluate the RGB track; only a submitted draw consumes it. */
static float MDLX_SampleGeosetAlpha(mdxModel_t const *model, mdxGeoset_t const *geoset, uint32_t frame) {
    mdxGeosetAnim_t const *anim = geoset->geosetAnim;
    if (!anim) return 1.0f;
    float alpha = isfinite(anim->staticAlpha) ? anim->staticAlpha : 1.0f;
    if (anim->alphas) MDLX_GetModelKeytrackValue(model, anim->alphas, frame, &alpha);
    return MIN(MAX(alpha, 0.0f), 1.0f);
}

static bool MDLX_IsGeosetVisible(mdxModel_t const *model, mdxGeoset_t const *geoset, uint32_t frame) {
    return MDLX_EvaluateGeosetAlpha(model, geoset, frame) >= EPSILON;
}

static mdxTextureAnim_t *MDLX_GetTextureAnimAtIndex(mdxModel_t const *model, uint32_t textureAnimId) {
    mdxTextureAnim_t *textureAnim = model->textureAnims;
    if (textureAnimId == 0xFFFFFFFF) {
        return NULL;
    }
    for (uint32_t id = textureAnimId; textureAnim && id > 0; id--) {
        textureAnim = textureAnim->next;
    }
    return textureAnim;
}

/* Resolve the texture slot used by a layer at the current frame.  Layers with
 * a flipbook (KMTF) track animate their TEXS index over time (e.g. the menu
 * ocean cycling ocean_h.01..30); without evaluating it the layer is stuck on
 * its static base texture and renders wrong (white). */
static uint32_t MDLX_SampleLayerTextureId(mdxModel_t const *model,
                                         mdxMaterialLayer_t const *layer,
                                         uint32_t frame) {
    uint32_t textureId = layer->textureId;
    if (layer->flipbook) {
        int value = (int)textureId;
        MDLX_GetModelKeytrackValue(model, layer->flipbook, frame, &value);
        if (value >= 0 && value < model->num_textures) {
            textureId = (uint32_t)value;
        }
    }
    return textureId;
}

static mat3_t MDLX_SampleLayerUV(mdxModel_t const *model,
                                           mdxMaterialLayer_t const *layer,
                                           uint32_t frame)
{
    vec3_t translation = { 0, 0, 0 };
    quaternion_t rotation = { 0, 0, 0, 1 };
    vec3_t scale = { 1, 1, 1 };
    mdxTextureAnim_t const *textureAnim = MDLX_GetTextureAnimAtIndex(model, layer->transformId);

    if (textureAnim) {
        if (textureAnim->translation) {
            MDLX_GetModelKeytrackValue(model, textureAnim->translation, frame, &translation);
        }
        if (textureAnim->rotation) {
            MDLX_GetModelKeytrackValue(model, textureAnim->rotation, frame, &rotation);
        }
        if (textureAnim->scale) {
            MDLX_GetModelKeytrackValue(model, textureAnim->scale, frame, &scale);
        }
    }

    if (!isfinite(translation.x) || !isfinite(translation.y)) {
        translation = (vec3_t){ 0, 0, 0 };
    }
    if (!isfinite(rotation.z) || !isfinite(rotation.w)) {
        rotation = (quaternion_t){ 0, 0, 0, 1 };
    }
    if (!isfinite(scale.x) || !isfinite(scale.y)) {
        scale = (vec3_t){ 1, 1, 1 };
    }

    {
        /* Build the UV affine matrix.  Operations applied in order:
             1. translate by (T.x, T.y)
             2. rotate around UV centre (0.5,0.5) using quaternion zw components
             3. scale around UV centre
           Combined as a mat3: uv_out = (M * vec3(uv, 1)).xy
           Column-major for glUniformMatrix3fv. */
        float c = rotation.w * rotation.w - rotation.z * rotation.z;
        float s = 2.0f * rotation.z * rotation.w;
        float tx = scale.x * (c * (translation.x - 0.5f) - s * (translation.y - 0.5f)) + 0.5f;
        float ty = scale.y * (s * (translation.x - 0.5f) + c * (translation.y - 0.5f)) + 0.5f;
        GLfloat m[9] = { scale.x*c, scale.y*s, 0, -scale.x*s, scale.y*c, 0, tx, ty, 1 };
        mat3_t result; memcpy(&result, m, sizeof(result)); return result;
    }
}

static void MDLX_BindLayerTextureAnimation(mdxModel_t const *model, mdxMaterialLayer_t const *layer, uint32_t frame) {
    mdxLayerState_t const *state = MDLX_LayerState(&(mdxLayerQuery_t){.model = model, .layer = layer, .frame = frame});
    mdlx.shader->state.uvMatrix = state ? state->uv : MDLX_SampleLayerUV(model, layer, frame);
}

static void MDLX_BindGeosetMatrixPalette(mdxModel_t const *model, mdxGeoset_t const *geoset) {
    mat4_t matrixPalette[MDX_MATRIX_PALETTE];
    /* Skin indices are geoset-local (0..num_matrixPalette-1), so only the
     * palette entries the geoset actually references need to reach the shader.
     * Uploading the full BZ_BONE_PALETTE_MAX per geoset was wasted uniform
     * traffic for every draw. */
    uint32_t const count = MIN((uint32_t)geoset->num_matrixPalette, BZ_BONE_PALETTE_MAX);

    if (model_batch.drawing) {
        if (model_batch.transport != MDX_PALETTE_TEXTURE) {
            mat4_t *bones = model_batch.transport == MDX_PALETTE_BLOCK ? block_palette.bones : mdlx.shader->state.bones;
            FOR_LOOP(actor, model_batch.drawing) {
                mdxDrawPacket_t const *packet = model_batch.packets[actor];
                if (packet) {
                    FOR_EACH_ARRAY(mdxGeosetState_t const, geo, packet->geosets) {
                        if (geo->geoset != geoset) continue;
                        memcpy(bones + actor * count, packet->bones + geo->boneofs, count * sizeof(mat4_t));
                        break;
                    }
                    continue;
                }
                FOR_LOOP(i, count) {
                    int id = geoset->matrixPalette[i];
                    uint32_t slot = id >= 0 && id < MDX_MAX_NODES ? model_batch.bone_offsets[id] : 0;
                    vec4_t const *rows = instance_bones + (actor * model_batch.span + slot) * MDLX_BONE_TEXELS;
                    mat4_t *bone = &bones[actor * count + i];
                    Matrix4_identity(bone);
                    FOR_LOOP(row, MDLX_BONE_TEXELS) {
                        bone->v[row] = rows[row].x; bone->v[row+4] = rows[row].y;
                        bone->v[row+8] = rows[row].z; bone->v[row+12] = rows[row].w;
                    }
                }
            }
            mdlx.shader->state.boneCount = count * model_batch.drawing;
            if (model_batch.transport == MDX_PALETTE_BLOCK) {
                block_palette.shader.stride = count;
                if (!block_palette.vbo) R_Call(glGenBuffers, 1, &block_palette.vbo);
                R_Call(glBindBuffer, GL_UNIFORM_BUFFER, block_palette.vbo);
                /* A linked block requires its full declared range, even when only an active prefix is written. */
                R_Call(glBufferData, GL_UNIFORM_BUFFER, block_palette.limit * sizeof(mat4_t), NULL, GL_STREAM_DRAW);
                uint32_t bytes = count * model_batch.drawing * sizeof(mat4_t);
                if (bytes) R_Call(glBufferSubData, GL_UNIFORM_BUFFER, 0, bytes, bones);
                R_Call(glBindBufferBase, GL_UNIFORM_BUFFER, BZ_MDX_BLOCK_BINDING, block_palette.vbo);
                if (model_batch.stats) {
                    model_batch.stats->block_uploads++; model_batch.stats->block_bytes += bytes;
                }
            } else uniform_prog.stride = count;
            return;
        }
        FOR_LOOP(i, count) {
            int id = geoset->matrixPalette[i];
            instance_prog.offsets[i] = id >= 0 && id < MDX_MAX_NODES ? model_batch.bone_offsets[id] : 0;
        }
        mdlx.shader->state.boneCount = count;
        return;
    }

    mdxGeosetState_t const *prepared = MDLX_GeosetState(model, geoset, draw_packet ? draw_packet->frame : 0);
    if (prepared) {
        memcpy(mdlx.shader->state.bones, draw_packet->bones + prepared->boneofs, count * sizeof(mat4_t));
        mdlx.shader->state.boneCount = count;
        return;
    }
    FOR_LOOP(i, count) {
        int node_id = geoset->matrixPalette[i];
        if (node_id >= 0 && node_id < MDX_MAX_NODES && model->nodes[node_id]) {
            matrixPalette[i] = node_matrices[node_id];
        } else {
            Matrix4_identity(&matrixPalette[i]);
        }
    }

    memcpy(&mdlx.shader->state.bones, matrixPalette->v, (count) * sizeof(mat4_t));
    mdlx.shader->state.boneCount = count;
}

static vec4_t MDLX_SampleGeosetColor(mdxModel_t const *model,
                                        mdxGeoset_t const *geoset,
                                        uint32_t frame)
{
    vec4_t color = { 1.0f, 1.0f, 1.0f, 1.0f };

    if (!geoset->geosetAnim) {
        return color;
    }
    color.w = MDLX_SampleGeosetAlpha(model, geoset, frame);
    if (geoset->geosetAnim->flags & 0x2) {
        vec3_t geosetColor = { 1.0f, 1.0f, 1.0f };

        /* Static GEOA is RGB; animated KGAC is BGR. Normalize before the shared RGB shader upload. */
        MDLX_GetGeosetAnimationStaticColor(geoset->geosetAnim, &geosetColor);
        if (geoset->geosetAnim->colors)
            MDLX_GetAnimatedColorTrackValue(model, geoset->geosetAnim->colors, frame, &geosetColor);
        color.x = geosetColor.x;
        color.y = geosetColor.y;
        color.z = geosetColor.z;
    }
    color.x = MIN(MAX(color.x, 0.0f), 1.0f);
    color.y = MIN(MAX(color.y, 0.0f), 1.0f);
    color.z = MIN(MAX(color.z, 0.0f), 1.0f);
    color.w = MIN(MAX(color.w, 0.0f), 1.0f);
    return color;
}

static float MDLX_SampleLayerAlpha(mdxModel_t const *model,
                                     mdxMaterial_t const *material,
                                     mdxMaterialLayer_t const *layer,
                                     uint32_t frame)
{
    float alpha = layer->staticAlpha;

    if (!isfinite(alpha)) {
        alpha = 1.0f;
    }
    if (layer->alpha) {
        MDLX_GetModelKeytrackValue(model, layer->alpha, frame, &alpha);
    }
    if (material->alpha) {
        float materialAlpha = 1.0f;
        MDLX_GetModelKeytrackValue(model, material->alpha, frame, &materialAlpha);
        alpha *= materialAlpha;
    }
    if (!isfinite(alpha)) {
        alpha = 1.0f;
    }
    if (alpha < 0.0f) {
        alpha = 0.0f;
    } else if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    return alpha;
}

/* One bounded arena keeps every published pointer stable until the pose epoch is invalidated. */
void MDLX_ClearDrawPackets(void) { draw_packets.used = 0; draw_packet = NULL; }

mdxDrawPacket_t *MDLX_ReserveDrawPacket(mdxModel_t const *model, uint32_t frame) {
    uint32_t geos = 0, layers = 0, bones = 0;
    FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets) {
        mdxMaterial_t const *mat = MDLX_GetMaterialAtIndex(geo, model);
        geos++; layers += mat ? mat->num_layers : 0;
        bones += MIN((uint32_t)geo->num_matrixPalette, BZ_BONE_PALETTE_MAX);
    }
    if (!geos) return NULL;
    size_t bytes = sizeof(mdxDrawPacket_t) + geos * sizeof(mdxGeosetState_t) + layers * sizeof(mdxLayerState_t);
    bytes += bones * sizeof(mat4_t) + model->num_nodes * MDLX_BONE_TEXELS * sizeof(vec4_t);
    bytes = (bytes + 7) & ~(size_t)7;
    if (bytes > BZ_MDX_PACKET_BYTES - draw_packets.used || draw_packets.failed) return NULL;
    if (!draw_packets.data) {
        draw_packets.data = malloc(BZ_MDX_PACKET_BYTES);
        if (!draw_packets.data) {
            fprintf(stderr, "MDX: unable to allocate CPU packet arena; retaining on-demand draws\n");
            draw_packets.failed = true; return NULL;
        }
    }
    mdxDrawPacket_t *packet = (mdxDrawPacket_t *)(draw_packets.data + draw_packets.used);
    draw_packets.used += bytes;
    *packet = (mdxDrawPacket_t){.model = model, .frame = frame, .geosets = (void *)(packet + 1)};
    ARRAY_COUNT(packet->geosets) = geos;
    packet->layers = (void *)(packet->geosets + geos); ARRAY_COUNT(packet->layers) = layers;
    packet->bones = (void *)(packet->layers + layers); ARRAY_COUNT(packet->bones) = bones;
    packet->rows = (void *)(packet->bones + bones); ARRAY_COUNT(packet->rows) = model->num_nodes * MDLX_BONE_TEXELS;
    uint32_t index = 0, layerofs = 0, boneofs = 0;
    FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets) {
        mdxMaterial_t const *mat = MDLX_GetMaterialAtIndex(geo, model);
        packet->geosets[index++] = (mdxGeosetState_t){.geoset = geo, .material = mat, .layerofs = layerofs, .boneofs = boneofs};
        layerofs += mat ? mat->num_layers : 0; boneofs += MIN((uint32_t)geo->num_matrixPalette, BZ_BONE_PALETTE_MAX);
    }
    return packet;
}

/* Pose matrices and immutable authored tracks are the only inputs; no GL, shader scratch or effect/RNG work. */
void MDLX_PrepareDrawPacket(mdxDrawPacket_t *packet, mat4_t const *nodes) {
    mdxModel_t const *model = packet->model;
    mat4_t identity; Matrix4_identity(&identity);
    FOR_LOOP(i, model->num_nodes) {
        uint32_t id = model->node_list[i]->node_id;
        mat4_t const *matrix = id < MDX_MAX_NODES ? &nodes[id] : &identity;
        FOR_LOOP(row, MDLX_BONE_TEXELS)
            packet->rows[i * MDLX_BONE_TEXELS + row] = (vec4_t){matrix->v[row], matrix->v[row+4], matrix->v[row+8], matrix->v[row+12]};
    }
    FOR_EACH_ARRAY(mdxGeosetState_t, state, packet->geosets) {
        mdxGeoset_t const *geo = state->geoset;
        state->color = (vec4_t){1, 1, 1, MDLX_SampleGeosetAlpha(model, geo, packet->frame)};
        if (state->color.w < EPSILON) continue;
        state->color = MDLX_SampleGeosetColor(model, geo, packet->frame);
        FOR_LOOP(i, MIN((uint32_t)geo->num_matrixPalette, BZ_BONE_PALETTE_MAX)) {
            int id = geo->matrixPalette[i]; mat4_t *bone = packet->bones + state->boneofs + i;
            if (id >= 0 && id < MDX_MAX_NODES && model->nodes[id]) {
                if (!nodes[id].v[15]) return; /* A deliberately partial pose must retain on-demand palette assembly. */
                *bone = nodes[id];
            } else Matrix4_identity(bone);
        }
        if (!state->material) continue;
        FOR_LOOP(i, state->material->num_layers) {
            mdxMaterialLayer_t const *layer = &state->material->layers[i];
            packet->layers[state->layerofs + i] = (mdxLayerState_t){
                .alpha = MDLX_SampleLayerAlpha(model, state->material, layer, packet->frame),
                .texture = MDLX_SampleLayerTextureId(model, layer, packet->frame),
                .uv = MDLX_SampleLayerUV(model, layer, packet->frame)};
        }
    }
    packet->ready = true;
}

static float MDLX_EvaluateGeosetAlpha(mdxModel_t const *model, mdxGeoset_t const *geo, uint32_t frame) {
    mdxGeosetState_t const *state = MDLX_GeosetState(model, geo, frame);
    return state ? state->color.w : MDLX_SampleGeosetAlpha(model, geo, frame);
}
static vec4_t MDLX_EvaluateGeosetColor(mdxModel_t const *model, mdxGeoset_t const *geo, uint32_t frame) {
    mdxGeosetState_t const *state = MDLX_GeosetState(model, geo, frame);
    return state ? state->color : MDLX_SampleGeosetColor(model, geo, frame);
}
static float MDLX_EvaluateLayerAlpha(mdxModel_t const *model, mdxMaterial_t const *mat, mdxMaterialLayer_t const *layer, uint32_t frame) {
    mdxLayerState_t const *state = MDLX_LayerState(&(mdxLayerQuery_t){.model = model, .material = mat, .layer = layer, .frame = frame});
    return state ? state->alpha : MDLX_SampleLayerAlpha(model, mat, layer, frame);
}
static uint32_t MDLX_EvaluateLayerTextureId(mdxModel_t const *model, mdxMaterialLayer_t const *layer, uint32_t frame) {
    mdxLayerState_t const *state = MDLX_LayerState(&(mdxLayerQuery_t){.model = model, .layer = layer, .frame = frame});
    return state ? state->texture : MDLX_SampleLayerTextureId(model, layer, frame);
}

static void MDLX_RenderGeoset(mdxModel_t const *model,
                             mdxGeoset_t const *geoset,
                             mdxMaterial_t const *material,
                             uint32_t team,
                             texture_t const *overrideTexture,
                             uint32_t overrideSlot,
                             bool forceUnshaded,
                             uint32_t frame,
                             vec4_t const *tint,
                             bool blendedPass,
                             int drawLayer)
{
    bool force_two_sided = model && !model->cameras;
    modelProg_t *shader = mdlx.shader;
    vec4_t geosetColor;

    if (!MDLX_MaterialHasPass(material, blendedPass)) {
        return;
    }

    geosetColor = MDLX_EvaluateGeosetColor(model, geoset, frame);
    {
        float *component = &geosetColor.x;
        float const *factor = &tint->x;
        FOR_LOOP(i, 4) component[i] *= factor[i];
    }
    MDLX_BindGeosetMatrixPalette(model, geoset);
    shader->state.layerAlpha = 1.0f;
    shader->state.geosetColor = (vec4_t){ geosetColor.x, geosetColor.y, geosetColor.z, geosetColor.w };

    FOR_LOOP(layerID, material->num_layers) {
        mdxMaterialLayer_t const *layer = &material->layers[layerID];
        float alpha;

        if ((drawLayer >= 0 && layerID != (uint32_t)drawLayer) || MDLX_IsBlendedLayer(layer) != blendedPass) {
            continue;
        }
        R_Call(glEnable, GL_DEPTH_TEST);
        shader->state.alphaKey = 0;
        if (force_two_sided) {
            R_Call(glDisable, GL_CULL_FACE);
        } else {
            R_Call(glEnable, GL_CULL_FACE);
            R_Call(glCullFace, GL_BACK);
        }
        R_Call(glDepthMask, GL_TRUE);
        if (!MDLX_SetLayerBlend(layer, layerID))
            continue;
        /* Instance tint alpha is presentation opacity, not authored material
         * alpha. Opaque and alpha-key layers therefore need a real blend path
         * when the caller supplies a translucent instance; multiplying the
         * shader alpha alone would otherwise leave opaque layers fully opaque. */
        if (tint->w < 1.0f - EPSILON &&
            (layer->blendMode == BLEND_MODE_NONE ||
             layer->blendMode == BLEND_MODE_ALPHAKEY)) {
            shader->state.alphaKey = 0;
            R_SetAlphaKeyState(false);
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            R_Call(glDepthMask, GL_FALSE);
        }
        MDLX_ApplyLayerFlags(layer);
        bool unshaded = forceUnshaded || (layer->flags & MODEL_GEO_UNSHADED);
        shader->state.unshaded = unshaded;
        /* Fog only affects opaque/alpha-blended geometry.  Additive and
         * modulate layers (glows, the blue spire flare) must NOT mix toward
         * the fog colour or they turn into solid fog-coloured quads. */
        {
            bool layerFog = tr.viewDef.fogEnable &&
                !(layer->flags & MODEL_GEO_UNFOGGED) &&
                (layer->blendMode == BLEND_MODE_NONE ||
                 layer->blendMode == BLEND_MODE_ALPHAKEY ||
                 layer->blendMode == BLEND_MODE_BLEND);
            shader->state.fogEnable = layerFog ? 1 : 0;
        }
        alpha = MDLX_EvaluateLayerAlpha(model, material, layer, frame);
        if (alpha < EPSILON)
            continue;
        shader->state.layerAlpha = alpha;
        MDLX_BindLayerTextureAnimation(model, layer, frame);
        uint32_t textureId = MDLX_EvaluateLayerTextureId(model, layer, frame);
        mdxTexture_t const *modeltex = &model->textures[textureId];
        texture_t const *texture = MDLX_GetTexture(model, team, textureId, modeltex->replaceableID, overrideTexture, overrideSlot);
        R_BindTexture(texture, 0);
        bool cached = model_batch.drawing && model_batch.submit_mode == 2;
        uint32_t vao = MDX_GeosetArray(geoset, layer->coordId, cached);
        if (!vao) {
            fprintf(stderr, "MDX model '%s': layer references unavailable UV channel %d\n", model->info.name, layer->coordId);
            continue;
        }
        R_Call(glBindVertexArray, vao);
        /* The geoset VAO already binds the model-owned index buffer. */
        R_StatsDraw(GL_TRIANGLES, geoset->num_triangles, model_batch.drawing ? model_batch.drawing : 1);
        R_ApplyShader(shader);
        if (model_batch.drawing) {
            if (model_batch.stats) {
                model_batch.stats->vao_draws += cached; model_batch.stats->attrib_sets += !cached;
            }
            if (!cached) {
                uint32_t stream = model_batch.submit_mode ? model->buffers[BZ_MDX_INSTANCE_BUFFER] : model_batch.vbo;
                R_Call(glBindBuffer, GL_ARRAY_BUFFER, stream);
                FOR_LOOP(i, 4) {
                    R_Call(glEnableVertexAttribArray, attrib_instance + i);
                    R_Call(glVertexAttribPointer, attrib_instance + i, 4, GL_FLOAT, GL_FALSE, sizeof(mat4_t), (void *)(uintptr_t)(i * 4 * sizeof(float)));
                    R_Call(glVertexAttribDivisor, attrib_instance + i, 1);
                }
            }
            R_Call(glDrawElementsInstanced, GL_TRIANGLES, geoset->num_triangles, GL_UNSIGNED_SHORT, (void *)(uintptr_t)geoset->indexofs, model_batch.drawing);
            if (!cached) FOR_LOOP(i, 4) {
                R_Call(glVertexAttribDivisor, attrib_instance + i, 0);
                R_Call(glDisableVertexAttribArray, attrib_instance + i);
            }
        } else {
            R_Call(glDrawElements, GL_TRIANGLES, geoset->num_triangles, GL_UNSIGNED_SHORT, (void *)(uintptr_t)geoset->indexofs);
        }
    }

    R_Call(glEnable, GL_DEPTH_TEST);
    R_Call(glEnable, GL_CULL_FACE);
    R_SetAlphaKeyState(false);
    R_Call(glCullFace, GL_BACK);
    R_Call(glDepthMask, GL_TRUE);
    shader->state.unshaded = forceUnshaded;
    shader->state.layerAlpha = 1.0f;
    shader->state.geosetColor = (vec4_t){ 1.0f, 1.0f, 1.0f, 1.0f };
}

uint32_t MDLX_RemapAnimation(mdxModel_t const *model, uint32_t frame, cstring_t str) {
    mdxSequence_t const *seq = R_FindSequenceAtTime(model, frame);
    size_t seq_name_len;

    if (!seq) return frame;
    seq_name_len = strlen(seq->name);
    FOR_LOOP(i, model->num_sequences) {
        mdxSequence_t const *other = &model->sequences[i];
        if (!strncmp(other->name, seq->name, seq_name_len) &&
            other->name[seq_name_len] == ' ' &&
            !strcmp(other->name + seq_name_len + 1, str)) {
            return frame + other->interval[0] - seq->interval[0];
        }
    }
    return frame;
}

static bool MDLX_TraceModelMesh(renderEntity_t const *ent, line3_t const *line, vec3_t *intersection) {
    mat4_t invmodel, matmodel;
    vec3_t best_point = { 0 };
    float best_distance = FLT_MAX;
    bool hit = false;
    mdxModel_t const *model;

    if (!ent || !line || !ent->model)
        return false;
    model = ent->model->mdx;
    if (!model)
        return false;

    R_GetEntityMatrix(ent, &matmodel);
    Matrix4_inverse(&matmodel, &invmodel);
    line3_t linelocal = {
        Matrix4_multiply_vector3(&invmodel, &line->a),
        Matrix4_multiply_vector3(&invmodel, &line->b),
    };

    /* Warsmash's walkable-object height query calls
     * intersectRayWithCollision(..., true, true), which means "only use the
     * visible/selectable mesh" even when authored CollisionShapes exist.
     * Keep that behavior separate from normal model picking, where WC3 uses
     * CollisionShapes preferentially. */
    FOR_EACH_LIST(mdxGeoset_t, geoset, model->geosets) {
        box3_t box;
        vec3_t bounds_hit;

        if ((geoset->selectable & 4) ||
            !MDLX_IsGeosetVisible(model, geoset, ent->frame))
            continue;

        box = (box3_t) {
            .min = *(vec3_t const *)&geoset->default_bounds.box.min,
            .max = *(vec3_t const *)&geoset->default_bounds.box.max,
        };
        if (!Line3_intersect_box3(&linelocal, &box, &bounds_hit))
            continue;

        FOR_LOOP(i, geoset->num_triangles / 3) {
            vec3_t local_point;
            triangle3_t tri = {
                .a = geoset->vertices[geoset->triangles[i*3+0]],
                .b = geoset->vertices[geoset->triangles[i*3+1]],
                .c = geoset->vertices[geoset->triangles[i*3+2]],
            };
            if (Line3_intersect_triangle(&linelocal, &tri, &local_point)) {
                vec3_t const point = Matrix4_multiply_vector3(&matmodel, &local_point);
                float const distance = Vector3_distance(&line->a, &point);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_point = point;
                    hit = true;
                }
            }
        }
    }

    if (hit && intersection)
        *intersection = best_point;
    return hit;
}

bool MDLX_TraceWalkableSurface(renderEntity_t const *ent, line3_t const *line, vec3_t *intersection) {
    return MDLX_TraceModelMesh(ent, line, intersection);
}

bool MDLX_TraceModel(renderEntity_t const *ent, line3_t const *line, vec3_t *intersection) {
    mat4_t invmodel, matmodel;
    vec3_t best_point = { 0 };
    float best_distance = FLT_MAX;
    bool hit = false;
    mdxModel_t const *model;

    if (!ent || !line || !ent->model)
        return false;
    model = ent->model->mdx;
    if (!model)
        return false;

    R_GetEntityMatrix(ent, &matmodel);
    Matrix4_inverse(&matmodel, &invmodel);
    line3_t linelocal = {
        Matrix4_multiply_vector3(&invmodel, &line->a),
        Matrix4_multiply_vector3(&invmodel, &line->b),
    };

    if (model->collisionShapes) {
        FOR_EACH_LIST(mdxCollisionShape_t, collisionShape, model->collisionShapes) {
            vec3_t point;
            bool shape_hit = false;

            if (collisionShape->type == SHAPETYPE_BOX) {
                box3_t box = {
                    .min = collisionShape->vertex[0],
                    .max = collisionShape->vertex[1],
                };
                vec3_t local_point;
                if (Line3_intersect_box3(&linelocal, &box, &local_point)) {
                    point = Matrix4_multiply_vector3(&matmodel, &local_point);
                    shape_hit = true;
                }
            } else if (collisionShape->type == SHAPETYPE_SPHERE) {
                vec3_t center;
                memcpy(&center, &collisionShape->vertex[0], sizeof(vec3_t));
                sphere3_t sphere = {
                    .center = Matrix4_multiply_vector3(&matmodel, &center),
                    .radius = collisionShape->radius * ent->scale,
                };
                shape_hit = Line3_intersect_sphere3(line, &sphere, &point);
            }

            if (shape_hit) {
                float const distance = Vector3_distance(&line->a, &point);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_point = point;
                    hit = true;
                }
            }
        }
    } else {
        return MDLX_TraceModelMesh(ent, line, intersection);
    }

    if (hit && intersection)
        *intersection = best_point;
    return hit;
}

/* Reject empty phases before evaluating the pose; GEOA visibility needs tracks, not bone matrices. */
static bool MDLX_HasGeosetPass(mdxModel_t const *model, uint32_t frame, bool translucent) {
    bool solid = MDLX_RenderPhaseIncludesPass(tr.render_phase, false, translucent);
    bool alpha = MDLX_RenderPhaseIncludesPass(tr.render_phase, true, translucent);
    FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets) {
        mdxMaterial_t const *mat = MDLX_GetMaterialAtIndex(geo, model);
        if (((solid && MDLX_MaterialHasPass(mat, false)) || (alpha && MDLX_MaterialHasPass(mat, true))) &&
            MDLX_IsGeosetVisible(model, geo, frame))
            return true;
    }
    return false;
}

/* Hidden blood/effect geosets and unused sockets do not need a pose for a geometry-only packet. */
static void MDLX_DrawBoneMask(renderEntity_t const *ent, mdxModel_t const *model, mdxPoseMask_t mask) {
    memset(mask, 0, sizeof(mdxPoseMask_t));
    color32_t color = (ent->tint_valid || ent->tint.a) ? ent->tint : COLOR32_WHITE;
    bool trans = BYTE2FLOAT(color.a) < 1.0f - EPSILON;
    FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets) {
        if (!MDLX_IsGeosetVisible(model, geo, ent->frame)) continue;
        mdxMaterial_t const *mat = MDLX_GetMaterialAtIndex(geo, model);
        bool solid = MDLX_RenderPhaseIncludesPass(tr.render_phase, false, trans) && MDLX_MaterialHasPass(mat, false);
        bool alpha = MDLX_RenderPhaseIncludesPass(tr.render_phase, true, trans) && MDLX_MaterialHasPass(mat, true);
        if (!solid && !alpha) continue;
        FOR_LOOP(i, MIN(geo->num_matrixPalette, BZ_BONE_PALETTE_MAX)) {
            int id = geo->matrixPalette[i];
            if (id >= 0 && id < MDX_MAX_NODES && model->nodes[id]) mask[id / 32] |= 1u << (id % 32);
        }
    }
}

/* Instancing preserves transparent order only when each actor contributes a single layer draw. */
static mdxInstanceBlock_t MDLX_InstanceBlocker(renderEntity_t const *ent, mdxModel_t const *model, mat4_t const *matrix) {
    if (!model->num_nodes) return MDX_INSTANCE_BONES;
    if (model->emitters && !model_batch.effect_mode) return MDX_INSTANCE_EMITTERS;
    /* Ribbons draw immediately in alpha; solid geometry may still share a packet. */
    if (model->ribbons && (!model_batch.effect_mode || tr.render_phase == RENDER_PHASE_ALPHA)) return MDX_INSTANCE_RIBBONS;
    if (model->lights) return MDX_INSTANCE_LIGHTS;
    if (ent->effect_flags & EFX_MODEL) return MDX_INSTANCE_EFFECT;
    if ((ent->flags & (RF_SELECTED | RF_HOVERED)) || ent->indicator.a ||
        (ent->number && ent->number == tr.viewDef.hover_entity)) return MDX_INSTANCE_SELECTED;
    FOR_EACH_LIST(mdxAttachment_t, atch, model->attachments)
        if (atch->path[0]) return MDX_INSTANCE_ATTACHMENT;
    /* The shared instanced shader normalizes mat3(instance)*normal, requiring an orthogonal uniform scale. */
    vec3_t axes[3];
    FOR_LOOP(i, 3) axes[i] = (vec3_t){matrix->v[4*i], matrix->v[4*i+1], matrix->v[4*i+2]};
    float len = Vector3_dot(&axes[0], &axes[0]), tol = MAX(len, 1.0f) * 0.0001f;
    if (len < EPSILON || fabsf(Vector3_dot(&axes[1], &axes[1]) - len) > tol ||
        fabsf(Vector3_dot(&axes[2], &axes[2]) - len) > tol || fabsf(Vector3_dot(&axes[0], &axes[1])) > tol ||
        fabsf(Vector3_dot(&axes[0], &axes[2])) > tol || fabsf(Vector3_dot(&axes[1], &axes[2])) > tol)
        return MDX_INSTANCE_SCALE;
    color32_t color = (ent->tint_valid || ent->tint.a) ? ent->tint : COLOR32_WHITE;
    bool trans = BYTE2FLOAT(color.a) < 1.0f - EPSILON;
    uint32_t layers = 0;
    FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets) {
        if (!MDLX_IsGeosetVisible(model, geo, ent->frame)) continue;
        if (model_batch.transport != MDX_PALETTE_TEXTURE && !geo->num_matrixPalette) return MDX_INSTANCE_PALETTE;
        mdxMaterial_t const *mat = MDLX_GetMaterialAtIndex(geo, model);
        if (!mat) continue;
        FOR_LOOP(i, mat->num_layers) {
            mdxMaterialLayer_t const *layer = &mat->layers[i];
            if (!MDLX_RenderPhaseIncludesPass(tr.render_phase, MDLX_IsBlendedLayer(layer), trans)) continue;
            if (layer->flags & (MODEL_GEO_NO_DEPTH_TEST | MODEL_GEO_NO_DEPTH_SET)) return MDX_INSTANCE_DEPTH;
            layers++;
        }
    }
    if (!layers) return MDX_INSTANCE_EMPTY;
    return tr.render_phase == RENDER_PHASE_ALPHA && layers != 1 ? MDX_INSTANCE_ALPHA : MDX_INSTANCE_OK;
}

/* Pose frames may differ, but a packet must retain every frame-authored material, visibility and UV value. */
static bool MDLX_SameMaterials(mdxModel_t const *model, uint32_t a, uint32_t b) {
    if (a == b) return true;
    mdxDrawPacket_t const *old = model_batch.packets[0], *next = draw_packet;
    if (old && next && old->model == model && next->model == model && old->frame == a && next->frame == b) {
        FOR_LOOP(i, ARRAY_COUNT(old->geosets)) {
            mdxGeosetState_t const *x = old->geosets + i, *y = next->geosets + i;
            bool xv = x->color.w >= EPSILON, yv = y->color.w >= EPSILON;
            if (xv != yv) return false;
            if (!xv) continue;
            if (memcmp(&x->color, &y->color, sizeof(x->color))) return false;
            if (!x->material) continue;
            FOR_LOOP(j, x->material->num_layers) {
                mdxLayerState_t const *xl = old->layers + x->layerofs + j, *yl = next->layers + y->layerofs + j;
                if (xl->alpha != yl->alpha || xl->texture != yl->texture || memcmp(&xl->uv, &yl->uv, sizeof(xl->uv))) return false;
            }
        }
        return true;
    }
    FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets) {
        vec4_t ac = MDLX_EvaluateGeosetColor(model, geo, a), bc = MDLX_EvaluateGeosetColor(model, geo, b);
        bool av = ac.w >= EPSILON, bv = bc.w >= EPSILON;
        if (av != bv) return false;
        if (!av) continue;
        if (memcmp(&ac, &bc, sizeof(ac))) return false;
        mdxMaterial_t const *mat = MDLX_GetMaterialAtIndex(geo, model);
        if (!mat) continue;
        FOR_LOOP(i, mat->num_layers) {
            mdxMaterialLayer_t const *layer = &mat->layers[i];
            if (MDLX_EvaluateLayerAlpha(model, mat, layer, a) != MDLX_EvaluateLayerAlpha(model, mat, layer, b) ||
                MDLX_EvaluateLayerTextureId(model, layer, a) != MDLX_EvaluateLayerTextureId(model, layer, b))
                return false;
            if (MDLX_GetTextureAnimAtIndex(model, layer->transformId)) {
                mat3_t saved = mdlx.shader->state.uvMatrix;
                MDLX_BindLayerTextureAnimation(model, layer, a);
                mat3_t am = mdlx.shader->state.uvMatrix;
                MDLX_BindLayerTextureAnimation(model, layer, b);
                bool equal = !memcmp(&am, &mdlx.shader->state.uvMatrix, sizeof(am));
                mdlx.shader->state.uvMatrix = saved;
                if (!equal) return false;
            }
        }
    }
    return true;
}

/* Adjacent equal material/pose state shares one ordered matrix stream; actor placement remains per instance. */
static bool MDLX_QueueModel(renderEntity_t const *ent, mdxModel_t const *model, mat4_t const *matrix) {
    mdxInstanceBlock_t block = MDLX_InstanceBlocker(ent, model, matrix);
    if (model_batch.stats) { model_batch.stats->attempted++; model_batch.stats->reject[block]++; }
    if (block) { MDLX_FlushModelBatch(MDX_FLUSH_BLOCKED); return false; }
    if (model_batch.stats) {
        model_batch.stats->accepted++;
        model_batch.stats->effect_actors += model->emitters || model->ribbons;
    }
    renderEntity_t const *old = &model_batch.entity;
    if (model_batch.count) {
        mdxBatchFlush_t reason =
            model_batch.count == model_batch.capacity ? MDX_FLUSH_CAPACITY :
            model_batch.model != model ? MDX_FLUSH_MODEL : old->flags != ent->flags ? MDX_FLUSH_FLAGS :
            old->team != ent->team ? MDX_FLUSH_TEAM : old->skin != ent->skin || old->skin_slot != ent->skin_slot ? MDX_FLUSH_SKIN :
            old->tint_valid != ent->tint_valid || memcmp(&old->tint, &ent->tint, sizeof(ent->tint)) ? MDX_FLUSH_TINT :
            (model->num_globalSequences && model_batch.time != MDLX_GlobalAnimationTime()) ||
            !MDLX_SameMaterials(model, old->frame, ent->frame) ? MDX_FLUSH_MATERIAL : MDX_FLUSH_REASONS;
        if (reason != MDX_FLUSH_REASONS) MDLX_FlushModelBatch(reason);
    }
    if (!model_batch.count) {
        model_batch.entity = *ent; model_batch.model = model; model_batch.span = 1;
        model_batch.time = MDLX_GlobalAnimationTime();
        memset(model_batch.bone_offsets, 0, sizeof(model_batch.bone_offsets));
        MDLX_DrawBoneMask(ent, model, model_batch.mask);
        FOR_LOOP(i, model->num_nodes) {
            uint32_t id = model->node_list[i]->node_id;
            if (model_batch.mask[id / 32] & (1u << (id % 32)))
                model_batch.bone_offsets[id] = model_batch.span++;
        }
        model_batch.capacity = MIN(MDLX_INSTANCE_LIMIT,
            (uint64_t)MDLX_BONE_TEX_WIDTH * model_batch.maxsize / (model_batch.span * MDLX_BONE_TEXELS));
        if (model_batch.transport != MDX_PALETTE_TEXTURE) {
            uint32_t palette = 1;
            FOR_EACH_LIST(mdxGeoset_t, geo, model->geosets)
                palette = MAX(palette, MIN((uint32_t)geo->num_matrixPalette, BZ_BONE_PALETTE_MAX));
            model_batch.capacity = MIN(MDLX_INSTANCE_LIMIT, model_batch.bone_limit / palette);
        }
    }
    model_batch.frames[model_batch.count][0] = ent->oldframe;
    model_batch.frames[model_batch.count][1] = ent->frame;
    model_batch.packets[model_batch.count] = draw_packet;
    model_batch.matrices[model_batch.count++] = *matrix;
    return true;
}

static void MDLX_RenderGeosets(renderEntity_t const *entity,
                               mdxModel_t const *model, color32_t color)
{
    bool forceUnshaded = (entity->flags & RF_NO_LIGHTING) != 0;
    texture_t const *skin = entity->skin;
    uint32_t skin_slot = entity->skin_slot;
    /* Cliff/cave meshes usually have an empty TEXS path for replacement ID 11.
     * Resolve their map-specific image only when an explicit skin is absent. */
    if (!skin) {
        FOR_LOOP(i, model->num_textures) {
            if (model->textures[i].replaceableID == 11) {
                skin = R_W3CliffReplacementAt(entity->origin.x, entity->origin.y);
                skin_slot = 11;
                break;
            }
        }
    }
    vec4_t const tint = {
        BYTE2FLOAT(color.r), BYTE2FLOAT(color.g),
        BYTE2FLOAT(color.b), BYTE2FLOAT(color.a)
    };
    bool const translucent_instance = tint.w < 1.0f - EPSILON;
    uint32_t layerCount = 0;
    uint32_t drawCount = 0;
    mdxGeosetDrawOrder_t stackDrawOrder[MDLX_STACK_DRAW_ORDER];
    mdxGeosetDrawOrder_t *drawOrder;

    FOR_EACH_LIST(mdxGeoset_t, geoset, model->geosets) {
        mdxMaterial_t const *material;
        material = MDLX_GetMaterialAtIndex(geoset, model);
        if (material) layerCount += material->num_layers;
        /* Count alpha layers without sampling geometry owned by the solid pass. */
        if (!MDLX_RenderPhaseIncludesPass(tr.render_phase, false, translucent_instance)) continue;
        if (MDLX_MaterialHasPass(material, false) && MDLX_IsGeosetVisible(model, geoset, entity->frame)) {
            MDLX_RenderGeoset(model, geoset, material, entity->team&TEAM_MASK, skin, skin_slot, forceUnshaded, entity->frame, &tint, false, -1);
        }
    }

    if (layerCount == 0 ||
        !MDLX_RenderPhaseIncludesPass(tr.render_phase, true, translucent_instance)) {
        return;
    }

    drawOrder = layerCount <= MDLX_STACK_DRAW_ORDER ?
                stackDrawOrder :
                ri.MemAlloc(sizeof(*drawOrder) * layerCount);
    if (!drawOrder) {
        FOR_EACH_LIST(mdxGeoset_t, geoset, model->geosets) {
            mdxMaterial_t const *material = MDLX_GetMaterialAtIndex(geoset, model);
            if (MDLX_MaterialHasPass(material, true) && MDLX_IsGeosetVisible(model, geoset, entity->frame))
            {
                MDLX_RenderGeoset(model, geoset, material, entity->team&TEAM_MASK, skin, skin_slot, forceUnshaded, entity->frame, &tint, true, -1);
            }
        }
        return;
    }
    FOR_EACH_LIST(mdxGeoset_t, geoset, model->geosets) {
        mdxMaterial_t const *material;
        material = MDLX_GetMaterialAtIndex(geoset, model);
        if (!MDLX_MaterialHasPass(material, true) || !MDLX_IsGeosetVisible(model, geoset, entity->frame)) {
            continue;
        }
        FOR_LOOP(layer, material->num_layers) {
            if (!MDLX_IsBlendedLayer(&material->layers[layer])) continue;
            drawOrder[drawCount] = (mdxGeosetDrawOrder_t) {
                .geoset = geoset, .material = material, .priority = material->priority,
                .layer = layer, .order = drawCount,
            };
            drawCount++;
        }
    }

    if (drawCount > 1) {
        qsort(drawOrder, drawCount, sizeof(*drawOrder), MDLX_CompareGeosetDrawOrder);
    }

    FOR_LOOP(i, drawCount) {
        mdxGeoset_t const *geoset = drawOrder[i].geoset;
        MDLX_RenderGeoset(model, geoset, drawOrder[i].material, entity->team&TEAM_MASK, skin, skin_slot, forceUnshaded, entity->frame, &tint, true, drawOrder[i].layer);
    }

    if (drawOrder != stackDrawOrder) {
        ri.MemFree(drawOrder);
    }
}

static int MDLX_CollectModelLights(mdxModel_t const *model,
                                   mat4_t const *modelMatrix,
                                   uint32_t frame,
                                   rModelLight_t *lights,
                                   int maxLights)
{
    int count = 0;

    FOR_EACH_LIST(mdxLight_t, light, model->lights) {
        rModelLight_t evaluated;
        if (!MDLX_EvaluateLight(model, light, modelMatrix, frame, true, &evaluated))
            continue;
        if (count < maxLights)
            lights[count] = evaluated;
        count++;
    }

    return MIN(count, maxLights);
}

static buffer_t ribbon_buf;
static bool ribbon_buf_ready;
static vertex_t ribbon_verts[TRAIL_MAX_EDGES * 6];

mdxMaterial_t *MDLX_MaterialAt(mdxModel_t const *model, uint32_t id) {
    mdxMaterial_t *material = model->materials;
    for (; material && id > 0; id--)
        material = material->next;
    return material;
}

static void MDLX_EnsureRibbonBuffer(void) {
    static const struct { GLuint attr; GLint size; GLenum type; GLboolean norm; size_t ofs; } attrs[] = {
        { attrib_position, 3, GL_FLOAT, GL_FALSE, offsetof(vertex_t, position) },
        { attrib_texcoord, 2, GL_FLOAT, GL_FALSE, offsetof(vertex_t, texcoord) },
        { attrib_normal, 3, GL_FLOAT, GL_FALSE, offsetof(vertex_t, normal) },
        { attrib_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(vertex_t, color) },
        { attrib_skin1, 4, GL_UNSIGNED_BYTE, GL_FALSE, offsetof(vertex_t, skin) },
        { attrib_boneWeight1, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(vertex_t, boneWeight) },
    };
    if (ribbon_buf_ready) return;
    R_Call(glGenVertexArrays, 1, &ribbon_buf.vao);
    R_Call(glGenBuffers, 1, &ribbon_buf.vbo);
    R_Call(glBindVertexArray, ribbon_buf.vao);
    R_Call(glBindBuffer, GL_ARRAY_BUFFER, ribbon_buf.vbo);
    FOR_LOOP(i, sizeof(attrs) / sizeof(*attrs)) {
        R_Call(glEnableVertexAttribArray, attrs[i].attr);
        R_Call(glVertexAttribPointer, attrs[i].attr, attrs[i].size, attrs[i].type, attrs[i].norm,
               sizeof(vertex_t), (void *)attrs[i].ofs);
    }
    ribbon_buf_ready = true;
}

/* Upload one strip and run it through the material's layer stack. Shared by live
 * trails and detached (entity-less) orphans so both execute identical GL. */
void MDLX_DrawRibbonVerts(mdxModel_t const *model, vertex_t *verts, uint32_t nverts,
                          mdxMaterial_t const *material, uint32_t team)
{
    modelProg_t *shader = mdlx.shader;
    mat4_t identity;

    if (!shader || !model || !verts || !nverts || !material) return;
    MDLX_EnsureRibbonBuffer();
    Matrix4_identity(&identity);
    shader->state.model = identity;
    shader->state.bones[0] = identity;
    shader->state.boneCount = 1;
    shader->state.geosetColor = (vec4_t){ 1, 1, 1, 1 };
    shader->state.layerAlpha = 1.0f;
    R_Call(glBindVertexArray, ribbon_buf.vao);
    R_Call(glBindBuffer, GL_ARRAY_BUFFER, ribbon_buf.vbo);
    R_Call(glBufferData, GL_ARRAY_BUFFER, nverts * sizeof(vertex_t), verts, GL_STREAM_DRAW);
    FOR_LOOP(layerID, material->num_layers) {
        mdxMaterialLayer_t const *layer = &material->layers[layerID];
        uint32_t textureId = layer->textureId;
        mdxTexture_t const *modeltex;
        texture_t const *texture;
        bool layerFog;

        if (textureId >= (uint32_t)model->num_textures) continue;
        R_Call(glEnable, GL_DEPTH_TEST);
        R_Call(glDisable, GL_CULL_FACE);
        shader->state.alphaKey = 0;
        if (!MDLX_SetLayerBlend(layer, layerID)) continue;
        MDLX_ApplyLayerFlags(layer);
        shader->state.unshaded = (layer->flags & MODEL_GEO_UNSHADED) ? 1 : 0;
        layerFog = tr.viewDef.fogEnable &&
            !(layer->flags & MODEL_GEO_UNFOGGED) &&
            (layer->blendMode == BLEND_MODE_NONE ||
             layer->blendMode == BLEND_MODE_ALPHAKEY ||
             layer->blendMode == BLEND_MODE_BLEND);
        shader->state.fogEnable = layerFog ? 1 : 0;
        modeltex = &model->textures[textureId];
        texture = MDLX_GetTexture(model, team & TEAM_MASK, textureId, modeltex->replaceableID, NULL, 0);
        R_BindTexture(texture, 0);
        R_StatsDraw(GL_TRIANGLES, nverts, 1);
        R_ApplyShader(shader);
        R_Call(glDrawArrays, GL_TRIANGLES, 0, (GLsizei)nverts);
    }
}

void MDLX_RenderRibbonEmitters(renderEntity_t const *entity, mdxModel_t const *model, mat4_t const *model_matrix) {
    modelProg_t *shader;
    mat4_t saved_model;
    int saved_unshaded, saved_fog;

    if (!entity || !model || !model->ribbons || !model_matrix) return;
    if ((entity->flags & RF_NOT_SELECTABLE) && entity->oldframe == entity->frame) return;
    shader = mdlx.shader;
    if (!shader) return;
    saved_model = shader->state.model;
    saved_unshaded = shader->state.unshaded;
    saved_fog = shader->state.fogEnable;
    FOR_EACH_LIST(mdxRibbonEmitter_t, ribbon, model->ribbons) {
        uint32_t nverts = MDLX_EmitRibbonVertices((mdxModel_t *)model, entity, model_matrix, ribbon,
                                               ribbon_verts, TRAIL_MAX_EDGES * 6);
        MDLX_DrawRibbonVerts(model, ribbon_verts, nverts,
                             MDLX_MaterialAt(model, ribbon->materialId), entity->team);
    }
    shader->state.model = saved_model;
    shader->state.unshaded = saved_unshaded;
    shader->state.fogEnable = saved_fog;
    shader->state.layerAlpha = 1.0f;
    shader->state.geosetColor = (vec4_t){ 1, 1, 1, 1 };
    R_Call(glEnable, GL_CULL_FACE);
    R_Call(glDepthMask, GL_TRUE);
    R_SetAlphaKeyState(false);
}

static bool MDLX_ModelInView(renderEntity_t const *entity, mdxModel_t const *model, mat4_t const *transform) {
    if (!(tr.viewDef.rdflags & RDF_NOFRUSTUMCULL)) {
        vec3_t const center = Box3_Center(&model->bounds.box);
        sphere3_t const sphere = {
            .center = Matrix4_multiply_vector3(transform, &center),
            .radius = model->bounds.radius * entity->scale,
        };
        if (!Frustum_ContainsSphere(&tr.viewDef.frustum, &sphere))
            return false;
        if (!Frustum_ContainsBox(&tr.viewDef.frustum, &model->bounds.box, transform))
            return false;
    }

    return true;
}

static void MDLX_RemapEntityFrames(renderEntity_t *entity, mdxModel_t const *model) {
    cstring_t resource = entity->flags & RF_HAS_LUMBER ? "Lumber" : entity->flags & RF_HAS_GOLD ? "Gold" : NULL;
    if (!resource) return;
    entity->frame = MDLX_RemapAnimation(model, entity->frame, resource);
    entity->oldframe = MDLX_RemapAnimation(model, entity->oldframe, resource);
}

/* Predict only the node work that the unchanged solid/alpha paths will consume. */
void MDLX_QueueEntityPose(renderEntity_t const *source, mdxModel_t const *model, mat4_t const *matrix) {
    MDLX_UpdateEntityAnimationPause(source);
    if (!model->num_nodes || !MDLX_ModelInView(source, model, matrix)) return;
    renderEntity_t entity = *source;
    MDLX_RemapEntityFrames(&entity, model);
    mdxAnimationTimeScope_t scope; MDLX_PushEntityAnimationTime(&entity, &scope);
    /* Both emitter families force a full pose in their owning phase, even without geosets. */
    if (model->emitters || model->ribbons) {
        MDLX_QueuePose(model, matrix, entity.frame, entity.oldframe, NULL);
        MDLX_PopEntityAnimationTime(&scope);
        return;
    }
    color32_t color = (entity.tint_valid || entity.tint.a) ? entity.tint : COLOR32_WHITE;
    bool trans = BYTE2FLOAT(color.a) < 1.0f - EPSILON, full = false, any = false;
    mdxPoseMask_t needed = {0};
    render_phase_t saved = tr.render_phase, phases[] = {RENDER_PHASE_SOLID, RENDER_PHASE_ALPHA};
    FOR_LOOP(p, 2) {
        tr.render_phase = phases[p];
        bool effects = phases[p] == RENDER_PHASE_ALPHA ? model->ribbons != NULL : model->emitters != NULL;
        if (!effects && !MDLX_HasGeosetPass(model, entity.frame, trans)) continue;
        any = true;
        if (!model_batch.enabled || MDLX_InstanceBlocker(&entity, model, matrix) != MDX_INSTANCE_OK) full = true;
        else {
            mdxPoseMask_t mask;
            MDLX_DrawBoneMask(&entity, model, mask);
            FOR_LOOP(i, MDX_MAX_NODES / 32) needed[i] |= mask[i];
        }
    }
    tr.render_phase = saved;
    if (any) MDLX_QueuePose(model, matrix, entity.frame, entity.oldframe, full ? NULL : needed);
    MDLX_PopEntityAnimationTime(&scope);
}

void MDX_RenderModel(renderEntity_t const *entity,
                     mdxModel_t const *model,
                     mat4_t const *transform)
{
    /* Observe held intervals even when the entity is culled. */
    MDLX_UpdateEntityAnimationPause(entity);
    if (!MDLX_ModelInView(entity, model, transform)) return;
    renderEntity_t remappedEntity;
    if (entity->flags & (RF_HAS_LUMBER | RF_HAS_GOLD)) {
        remappedEntity = *entity;
        MDLX_RemapEntityFrames(&remappedEntity, model);
        entity = &remappedEntity;
    }

    mdxAnimationTimeScope_t animation_time_scope;
    MDLX_PushEntityAnimationTime(entity, &animation_time_scope);
    draw_packet = model_batch.flushing ? model_batch.packets[0] : MDLX_PreparedDraw(entity, model, transform);

    color32_t const color = (entity->tint_valid || entity->tint.a) ? entity->tint : COLOR32_WHITE;
    bool const effects = tr.render_phase == RENDER_PHASE_ALPHA ? model->ribbons != NULL : model->emitters != NULL;
    /* A material-only model often has no alpha work. Emitters/ribbons still need their own phase's pose. */
    if (!effects && !MDLX_HasGeosetPass(model, entity->frame, BYTE2FLOAT(color.a) < 1.0f - EPSILON)) {
        MDLX_PopEntityAnimationTime(&animation_time_scope);
        return;
    }
    if (model_batch.enabled && MDLX_QueueModel(entity, model, transform)) {
        /* Particle submission is deferred until after water, but emission/RNG stays in source actor order. */
        if (tr.render_phase != RENDER_PHASE_ALPHA && model->emitters) {
            MDLX_BindBoneMatrices(model, transform, entity->frame, entity->oldframe);
            MDLX_RenderParticleEmitters(entity, model, transform);
        }
        MDLX_PopEntityAnimationTime(&animation_time_scope);
        return;
    }
    
    modelProg_t *shader = mdlx.shader;
    mat3_t normalMatrix;
    GLfloat const *viewProjectionMatrix =
#ifdef USE_SHADOWMAPS
        tr.render_phase == RENDER_PHASE_LIGHTS ? tr.viewDef.lightMatrix.v :
#endif
        tr.viewDef.viewProjectionMatrix.v;
    Matrix3_normal(&normalMatrix, transform);

    shader->state.model = *transform;
    shader->state.normalMatrix = normalMatrix;
    shader->state.unshaded = (entity->flags & RF_NO_LIGHTING) != 0;

    /* Fill each program's typed state; the backend uniform cache skips unchanged uploads independently. */
    {
        memcpy(&shader->state.viewProjection, viewProjectionMatrix, (1) * sizeof(mat4_t));
        shader->state.textureMatrix = tr.viewDef.textureMatrix;
        shader->state.lightMatrix = tr.viewDef.lightMatrix;
        shader->state.fogEnable = tr.viewDef.fogEnable ? 1 : 0;
        shader->state.firstBoneLookupIndex = 0.0f;
        if (tr.viewDef.fogEnable) {
            shader->state.fogColor = (vec3_t){ tr.viewDef.fogColor.x, tr.viewDef.fogColor.y, tr.viewDef.fogColor.z };
            shader->state.fogParams = (vec2_t){ tr.viewDef.fogStart, tr.viewDef.fogEnd };
        }
    }
    modelLighting_t lighting = { 0 };
    bool const portraitLighting = (entity->flags & RF_PORTRAIT_LIGHTING) != 0;
    environLight_t const *environment = !portraitLighting && tr.viewDef.entityLight.valid
        ? &tr.viewDef.entityLight
        : (!portraitLighting && tr.viewDef.terrainLight.valid ? &tr.viewDef.terrainLight : NULL);
    int numLights = 0;

    if (environment && R_LightingFromEnviron(environment, &lighting))
        numLights = lighting.count;

    /* Environment light is already on viewDef from R_SetupEnvironmentLighting.
     * Rebind this entity before evaluating its local lights or rendering. */
    if (!model_batch.drawing) MDLX_BindBoneMatrices(model, transform, entity->frame, entity->oldframe);
    numLights += MDLX_CollectModelLights(model, transform, entity->frame,
                                        &lighting.lights[numLights],
                                        BZ_MODEL_LIGHT_MAX - numLights);
    float ambient = numLights ? (portraitLighting ? 0.22f : 0.0f)
                              : (portraitLighting ? 0.58f : 0.35f);
    float directional = (entity->flags & RF_PORTRAIT_LIGHTING) ? 0.62f : 0.75f;
    vec3_t lightDir = {
        -tr.viewDef.lightMatrix.v[2],
        -tr.viewDef.lightMatrix.v[6],
        -tr.viewDef.lightMatrix.v[10],
    };
    rModelLight_t sun = {
        .dir = lightDir,
        .color = { directional, directional, directional },
        .intensity = 1.0f,
        .type = R_MODEL_LIGHT_DIRECT,
    };
    lighting.ambient = (vec3_t){ ambient, ambient, ambient };
    lighting.count = numLights ? numLights : 1;
    if (!numLights) lighting.lights[0] = sun;
    R_SetModelLighting(shader, &lighting);

    if (entity->flags & RF_NO_FOGOFWAR) {
        R_Call(glActiveTexture, GL_TEXTURE2);
        R_Call(glBindTexture, GL_TEXTURE_2D, tr.texture[TEX_WHITE]->texid);
        R_Call(glActiveTexture, GL_TEXTURE0);
    }
    MDLX_RenderGeosets(entity, model, color);

    if (!model_batch.flushing && tr.render_phase == RENDER_PHASE_ALPHA) {
        MDLX_RenderRibbonEmitters(entity, model, transform);
    } else if (!model_batch.flushing) {
        /* Particle emitters enqueue particles; the particle renderer draws
         * them after water. Do not enqueue a second copy in the alpha pass. */
        MDLX_RenderParticleEmitters(entity, model, transform);
    }

    if ((entity->flags & RF_NO_FOGOFWAR) && tr.world) {
        R_Call(glActiveTexture, GL_TEXTURE2);
        R_Call(glBindTexture, GL_TEXTURE_2D, R_GetFogOfWarTexture());
        R_Call(glActiveTexture, GL_TEXTURE0);
    }
    MDLX_PopEntityAnimationTime(&animation_time_scope);
}
