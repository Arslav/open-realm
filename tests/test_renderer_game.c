#include "renderer/r_local.h"
void R_TestPoseFlush(void);
void R_TestMinimapBatch(drawImageBatchParams_t const *p);
void R_TestMinimapImage(texture_t const *tex, rect_t const *screen, rect_t const *uv, color32_t color);
#define glFlush R_TestPoseFlush
/* Compile the WC3 game renderer into the headless renderer test. Function
 * sections let the linker retain R_RegisterMap and its dependencies without
 * requiring a GL context for unrelated draw paths. */
#define R_RegisterMap R_TestProductionRegisterMap
#define R_BlightTexture R_TestProductionBlightTexture
#define R_LoadModel R_TestProductionLoadModel
#define R_TerrainArt R_TestProductionTerrainArt
#define R_CliffType R_TestProductionCliffType
#define R_ReleaseModel R_TestProductionReleaseModel
#define MDLX_DrawSpriteInstance R_TestCursorSprite
#define MDLX_FindSequenceByName R_TestCursorSequence
#define MDLX_TraceWalkableSurface R_TestWalkableSurfaceTrace
#define MDLX_SetEntityAnimationFrame R_TestSetEntityAnimationFrame
#define _W3M_DrawAlphaSurfaces R_TestWaterAlphaSurfacePass
#define _W3M_DrawWorld R_TestWorldPass
#define R_LightningDraw R_TestLightningDraw
#define R_WeatherEmit R_TestWeatherEmit
#define R_DrawImage R_TestMinimapImage
#define R_DrawImageBatchEx R_TestMinimapBatch
#include "../games/warcraft-3/renderer/r_game.c"
#undef glFlush
#undef MDLX_TraceWalkableSurface
#undef MDLX_SetEntityAnimationFrame
#undef _W3M_DrawAlphaSurfaces
#undef _W3M_DrawWorld
#undef R_LightningDraw
#undef R_WeatherEmit
#undef R_DrawImage
#undef R_DrawImageBatchEx

/* Keep the selection-ring production path in this headless test translation
 * unit. The renderer imports below record whether it requests terrain-conformed
 * or flat geometry, without needing a GL context. */
#define R_GetEntityMatrix R_TestEntityMatrixFromEnts
#include "../renderer/r_ents.c"
#undef R_GetEntityMatrix

#include "test.h"

extern void R_TestUseProductionModelLoader(bool enabled);
extern cstring_t R_TestLastTextureLoad(void);
extern void R_TestSetTextureLoadResult(texture_t *texture);
extern uint32_t R_TestCaptureSplatPass(void (*draw)(void));
extern bool R_TestCapturingSplats(void);
extern void R_TestProductionRenderRectSplatUV(rectSplatParams_t const *params);
extern void R_TestProductionRenderSplat(vec2_t const *origin, float radius, texture_t const *texture, splat_shader_t *shader, color32_t color);
static uint32_t test_spn_render_count;
static renderEntity_t test_spn_render_entity;
static mat4_t test_spn_render_transform;
static handle_t test_renderer_archive;
static char test_sound_path[512];
static vec3_t test_sound_origin;
static uint32_t test_sound_count;
static uint32_t test_splat_count;
static vec2_t test_splat_origin;
static float test_splat_radius;
static uint32_t test_flat_splat_count;
static float test_flat_splat_z;
static color32_t test_splat_color;
static vec2_t test_splat_uv_mins, test_splat_uv_maxs;
static texture_t test_splat_texture;
static float test_walkable_hit_z;
static bool test_walkable_surface_height, test_walkable_no_hit;
static uint32_t test_walkable_traces;
static uint32_t test_alpha_draw_order, test_water_alpha_order, test_unit_alpha_order;
static bool test_real_geoset_order;
static struct {
    uint32_t calls, count;
    vertex_t vertices[4096];
    texture_t const *textures[512];
    uint32_t sizes[512];
} minimap_capture;

/* Capture the actual minimap quad stream; projection and quad construction stay production code. */
void R_TestMinimapBatch(drawImageBatchParams_t const *p) {
    T_ASSERT(minimap_capture.calls < 512 && minimap_capture.count + p->vertexCount <= 4096);
    if (minimap_capture.calls >= 512 || minimap_capture.count + p->vertexCount > 4096) return;
    T_EQ(p->shader, SHADER_UI); T_EQ(p->alphamode, BLEND_MODE_BLEND); T_ASSERT(!p->hasClip && !p->repeat);
    minimap_capture.textures[minimap_capture.calls] = p->texture;
    minimap_capture.sizes[minimap_capture.calls++] = p->vertexCount;
    memcpy(minimap_capture.vertices + minimap_capture.count, p->vertices, p->vertexCount * sizeof(vertex_t));
    minimap_capture.count += p->vertexCount;
}
void R_TestMinimapImage(texture_t const *tex, rect_t const *screen, rect_t const *uv, color32_t color) {
    vertex_t quad[6] = {0}; R_AddQuad(quad, screen, uv, color, 0);
    R_TestMinimapBatch(&(drawImageBatchParams_t){ .texture = tex, .shader = SHADER_UI,
        .alphamode = BLEND_MODE_BLEND, .vertices = quad, .vertexCount = 6 });
}

TEST(renderer_game, minimap_contacts_batch_preserves_every_quad_color_texture_and_order) {
    struct render_globals saved = tr;
    war3map_t world = { .width = 65, .height = 65 };
    renderEntity_t ents[304] = {0};
    texture_t white = { .texid = 1 }, hero = { .texid = 2 };
    texture_t const *old_hero = minimap_special[WC3_MINIMAP_CONTACT_HERO];
    tr.world = &world; tr.hasMinimap = true; tr.minimapRect = (rect_t){0.1f, 0.2f, 0.15f, 0.15f};
    tr.texture[TEX_WHITE] = &white; minimap_special[WC3_MINIMAP_CONTACT_HERO] = &hero;
    tr.viewDef.player = 0; tr.viewDef.game_variant = WC3_MINIMAP_ALLY_COLOR_MINIMAP;
    tr.viewDef.entities = ents; tr.viewDef.num_entities = 304;
    FOR_LOOP(i, 304) {
        ents[i] = (renderEntity_t){ .number = i + 1, .origin = {i * 20.0f, 100.0f, 0},
            .effect_flags = wc3_minimap_contact_set(0, WC3_MINIMAP_CONTACT_UNIT) };
    }
    ents[300].flags = RF_HIDDEN;
    ents[1].owner = 1; ents[1].flags = RF_HOSTILE;
    ents[2].owner = PLAYER_NEUTRAL_AGGRESSIVE; ents[3].owner = 2;
    ents[301].effect_flags = wc3_minimap_contact_set(0, WC3_MINIMAP_CONTACT_NONE);
    ents[302].effect_flags = wc3_minimap_contact_set(0, WC3_MINIMAP_CONTACT_HERO);
    minimap_capture = (typeof(minimap_capture)){0};
    R_DrawMinimapEntityMarkers();
    T_EQ(minimap_capture.calls, 5); T_EQ(minimap_capture.count, 302 * 6);
    T_EQ(minimap_capture.textures[0], &white); T_EQ(minimap_capture.textures[3], &hero);
    T_EQ(minimap_capture.textures[4], &white);
    FOR_LOOP(i, 302) {
        uint32_t index = i < 300 ? i : i + 2;
        vec2_t point; T_ASSERT(R_WorldToMinimap((vec2_t const *)&ents[index].origin, &point));
        rect_t rect = wc3_minimap_marker_rect(&point, wc3_minimap_contact_get(ents[index].effect_flags));
        uint32_t enemy = WC3_PlayerColorRGB(0), ally = WC3_PlayerColorRGB(2);
        color32_t colors[] = {COLOR32_WHITE, {(enemy >> 16) & 255, (enemy >> 8) & 255, enemy & 255, 255},
            {0,0,0,255}, {(ally >> 16) & 255, (ally >> 8) & 255, ally & 255, 255}};
        color32_t color = index < 4 ? colors[index] : COLOR32_WHITE;
        vertex_t quad[6] = {0}; R_AddQuad(quad, &rect, &(rect_t){0, 0, 1, 1}, color, 0);
        FOR_LOOP(j, 6) {
            vertex_t const *actual = minimap_capture.vertices + i * 6 + j;
            T_EQ(memcmp(&quad[j].position, &actual->position, sizeof(vec3_t)), 0);
            T_EQ(memcmp(&quad[j].texcoord, &actual->texcoord, sizeof(vec2_t)), 0);
            T_EQ(memcmp(&quad[j].color, &actual->color, sizeof(color32_t)), 0);
        }
    }
    minimap_special[WC3_MINIMAP_CONTACT_HERO] = old_hero; tr = saved;
}
void R_TestObserveGeosetDraw(void) {
    if (test_real_geoset_order) test_unit_alpha_order = ++test_alpha_draw_order;
}
void R_TestGeosetModel(renderEntity_t const *entity, mdxModel_t const *model, mat4_t const *matrix);

TEST(renderer_game, deferred_solid_models_flush_before_water_with_the_original_phase) {
    viewDef_t saved = tr.viewDef;
    render_phase_t phase = tr.render_phase;
    modelProg_t normal = {0}, *shader = mdlx.shader;
    mdxNode_t node = { .parent_id = (uint32_t)-1 };
    uint32_t vao = 7;
    int palette = 0;
    mdxGeoset_t geo = { .vaos = &vao, .vaos_count = 1, .num_triangles = 3,
        .matrixPalette = &palette, .num_matrixPalette = 1 };
    mdxMaterialLayer_t layer = { .blendMode = BLEND_MODE_NONE, .staticAlpha = 1, .transformId = (uint32_t)-1 };
    mdxMaterial_t mat = { .layers = &layer, .num_layers = 1 };
    mdxTexture_t tex = {0};
    mdxModel_t model = { .geosets = &geo, .materials = &mat, .textures = &tex, .num_textures = 1,
        .nodes = {&node}, .node_list = {&node}, .num_nodes = 1 };
    renderEntity_t ent = { .scale = 1 };
    mat4_t matrix;
    Matrix4_identity(&matrix); tr.viewDef = (viewDef_t){ .rdflags = RDF_NOFRUSTUMCULL };
    mdlx.shader = &normal; tr.render_phase = RENDER_PHASE_SOLID;
    test_alpha_draw_order = test_unit_alpha_order = test_water_alpha_order = 0;
    test_real_geoset_order = true;
    MDLX_BeginModelBatch();
    R_TestGeosetModel(&ent, &model, &matrix); matrix.v[12] = 10;
    R_TestGeosetModel(&ent, &model, &matrix);
    T_EQ(test_unit_alpha_order, 0);
    tr.render_phase = RENDER_PHASE_ALPHA;
    R_DrawAlphaSurfaces();
    T_EQ(test_unit_alpha_order, 1); T_EQ(test_water_alpha_order, 2);
    T_EQ(tr.render_phase, RENDER_PHASE_ALPHA);
    MDLX_ShutdownModelBatches(); test_real_geoset_order = false;
    mdlx.shader = shader; tr.viewDef = saved; tr.render_phase = phase;
}

TEST(renderer_game, null_splat_sound_is_treated_as_an_empty_optional_field) {
    wc3SplatData_t splat = { .name = "EmptySplat", .blend_mode = "0", .sound = "NULL" };
    wc3UberSplatData_t uber = { .name = "EmptyUber", .blend_mode = "0", .sound = "NULL" };

    R_W3WarnUnsupportedSplatFields(&splat);
    R_W3WarnUnsupportedUberSplatFields(&uber);
    T_ASSERT(!splat.unsupported_warned);
    T_ASSERT(!uber.unsupported_warned);
}

TEST(renderer_game, splat_atlas_rejects_overflowing_dimensions) {
    wc3SplatData_t splat = { .name = "InvalidAtlas", .rows = INT_MAX, .columns = 2 };

    T_ASSERT(!R_W3SplatAtlasValid(&splat));
    T_ASSERT(splat.atlas_warned);
}

TEST(renderer_game, splat_atlas_frame_handles_full_signed_range) {
    T_EQ(R_W3SplatAtlasFrame(INT_MIN, INT_MAX, 1.0f), INT_MAX);
    T_EQ(R_W3SplatAtlasFrame(INT_MAX, INT_MIN, 1.0f), INT_MIN);
}

TEST(renderer_game, event_warning_cache_suppresses_duplicate_asset_diagnostics) {
    R_W3ClearEventWarnings();
    T_ASSERT(R_W3EventWarningShouldLog("missing-spawn-row", "SPNxMissing"));
    T_ASSERT(!R_W3EventWarningShouldLog("missing-spawn-row", "SPNxMissing"));
    T_ASSERT(R_W3EventWarningShouldLog("missing-splat-row", "SPNxMissing"));
    R_W3ClearEventWarnings();
    T_ASSERT(R_W3EventWarningShouldLog("missing-spawn-row", "SPNxMissing"));
    R_W3ClearEventWarnings();
}

TEST(renderer_game, zero_lifetime_event_splats_are_one_frame_and_marked_unverified) {
    wc3SplatData_t splat_row = { .name = "ZeroSplat", .rows = 1, .columns = 1,
                                 .scale = 8.0f, .texture = &test_splat_texture };
    wc3UberSplatData_t uber_row = { .name = "ZeroUber", .scale = 8.0f,
                                    .texture = &test_splat_texture };
    wc3EventSplat_t splat = { .kind = WC3_EVENT_SPLAT_SPLAT, .splat_row = &splat_row, .active = true };
    wc3EventSplat_t uber = { .kind = WC3_EVENT_SPLAT_UBER, .uber_row = &uber_row, .active = true };
    uint32_t saved_time = tr.viewDef.time;

    R_W3ClearEventWarnings(); test_splat_count = 0; tr.viewDef.time = 100;
    T_ASSERT(R_W3RenderEventSplat(&splat));
    T_ASSERT(!splat.active); T_ASSERT(splat_row.zero_duration_warned);
    T_EQ(test_splat_count, 1);
    T_ASSERT(!R_W3RenderEventSplat(&splat));
    T_ASSERT(R_W3RenderEventSplat(&uber));
    T_ASSERT(!uber.active); T_ASSERT(uber_row.zero_duration_warned);
    T_EQ(test_splat_count, 2);
    T_ASSERT(!R_W3RenderEventSplat(&uber));
    R_W3ClearEventWarnings(); tr.viewDef.time = saved_time;
}

TEST(renderer_game, reused_entity_generation_seeds_mdx_event_clock) {
    uint32_t key = 100;
    vec3_t pivot = { 0 };
    mdxSequence_t sequence = { .interval = { 0, 1000 } };
    mdxEvent_t event = { .num_keys = 1, .globalSeqId = (uint32_t)-1, .keys = &key };
    mdxModel_t mdx = { .events = &event, .sequences = &sequence, .num_sequences = 1,
                       .pivots = &pivot, .num_pivots = 1 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    renderEntity_t entity = { .model = &model, .number = 12, .frame = 150, .generation = 2 };
    wc3SplatData_t row = { .name = "TestReuse", .rows = 1, .columns = 1, .scale = 8.0f,
                           .lifespan = 1.0f, .texture = &test_splat_texture };
    wc3SplatData_t *saved_rows = splat_data_rows;
    uint32_t saved_count = splat_data_count, saved_time = tr.viewDef.time;
    render_phase_t saved_phase = tr.render_phase;
    wc3EventState_t state = { .model = &model, .frame = 900, .render_time = 900,
                              .valid = true, .generation = 1 };
    mat4_t transform;

    snprintf(event.node.name, sizeof(event.node.name), "SPLxTestReuse");
    event.node.node_id = 0; event.node.parent_id = (uint32_t)-1;
    mdx.nodes[0] = &event.node; mdx.node_list[0] = &event.node; mdx.num_nodes = 1;
    splat_data_rows = &row; splat_data_count = 1;
    test_splat_count = 0; tr.viewDef.time = 1050; tr.render_phase = RENDER_PHASE_SOLID;
    Matrix4_identity(&transform);

    R_W3DispatchModelEvents(&(wc3EventDispatchParams_t){ .entity = &entity, .model = &mdx,
        .state = &state, .transform = &transform, .depth = 0 });
    T_EQ(test_splat_count, 0); /* The reused slot seeds a fresh event clock; it must not replay old keys. */
    T_EQ(state.generation, 2);

    splat_data_rows = saved_rows; splat_data_count = saved_count;
    tr.viewDef.time = saved_time; tr.render_phase = saved_phase;
    R_W3ClearEventSplats();
}

TEST(renderer_game, entity_camera_event_state_invalidates_when_model_has_no_events) {
    uint32_t key = 100;
    mdxEvent_t event = { .num_keys = 1, .globalSeqId = (uint32_t)-1, .keys = &key };
    mdxModel_t mdx_with_events = { .events = &event };
    mdxModel_t mdx_without_events = { 0 };
    model_t model_with_events = { .modeltype = ID_MDLX, .mdx = &mdx_with_events };
    model_t model_without_events = { .modeltype = ID_MDLX, .mdx = &mdx_without_events };
    renderEntity_t entity = { .model = &model_with_events, .number = 12, .frame = 0,
                              .generation = 1, .instance_id = 99127 };
    uint32_t saved_rdflags = tr.viewDef.rdflags;
    render_phase_t saved_phase = tr.render_phase;

    R_W3ClearCameraEventStates();
    tr.viewDef.rdflags = RDF_USE_ENTITY_CAMERA;
    tr.render_phase = RENDER_PHASE_SOLID;
    R_UpdateEntityPresentation(&entity);
    T_EQ(camera_event_state_count, 1);
    wc3EventState_t *state = &camera_event_states[0].state;
    T_ASSERT(state->valid);
    T_EQ(state->model, &model_with_events);

    entity.model = &model_without_events;
    R_UpdateEntityPresentation(&entity);
    /* Inspect retained state directly: calling R_W3CameraEventState here would
     * itself perform the invalidation and let a broken update path pass. */
    state = &camera_event_states[0].state;
    T_ASSERT(!state->valid);
    T_NULL(state->model);

    R_W3ClearCameraEventStates();
    tr.viewDef.rdflags = saved_rdflags;
    tr.render_phase = saved_phase;
}

static handle_t test_renderer_alloc(long size) { return calloc(1, (size_t)size); }
static void test_renderer_free(handle_t ptr) { free(ptr); }

/* Removing a middle entry compacts the table; its old tail must not seed a new camera. */
TEST(renderer_game, reused_camera_event_slot_starts_with_fresh_clock) {
    refImport_t saved = ri;
    model_t model = {0};
    renderEntity_t entity = { .instance_id = 101, .model = &model };
    ri.MemAlloc = test_renderer_alloc; ri.MemFree = test_renderer_free;
    R_W3ClearCameraEventStates();
    T_NOT_NULL(R_W3CameraEventState(&entity));
    entity.instance_id = 202;
    wc3EventState_t *state = R_W3CameraEventState(&entity);
    state->valid = true; state->frame = 60; state->render_time = 100;
    R_ReleaseGameEntityCameraEvents(101);
    entity.instance_id = 303;
    state = R_W3CameraEventState(&entity);
    T_ASSERT(!state->valid); T_EQ(state->frame, 0); T_EQ(state->render_time, 0);
    T_ASSERT(camera_event_states[0].state.valid); T_EQ(camera_event_states[0].state.frame, 60);
    R_W3ClearCameraEventStates(); ri = saved;
}

static int test_renderer_read(cstring_t path, void **buffer) {
    handle_t file = NULL;
    uint32_t size, read = 0;
    *buffer = NULL;
    if (!test_renderer_archive || !SFileOpenFileEx(test_renderer_archive, path, 0, &file)) return -1;
    size = SFileGetFileSize(file, NULL);
    *buffer = malloc((size_t)size + 1);
    if (!*buffer || !SFileReadFile(file, *buffer, size, &read, NULL) || read != size) {
        free(*buffer); *buffer = NULL; SFileCloseFile(file); return -1;
    }
    ((uint8_t *)*buffer)[size] = 0;
    SFileCloseFile(file);
    return (int)size;
}

static uint32_t test_renderer_load_slk(cstring_t path, slkField_t const *schema,
                                    void **rows, uint32_t row_stride) {
    void *buffer = NULL;
    int size = test_renderer_read(path, &buffer);
    uint32_t count = size < 0 ? 0 : Stb_SlkLoadBuffer(buffer, schema, rows, row_stride);
    free(buffer);
    return count;
}

static void test_renderer_play_sound(cstring_t path, vec3_t const *origin, float volume) {
    (void)volume;
    snprintf(test_sound_path, sizeof(test_sound_path), "%s", path);
    test_sound_origin = *origin;
    test_sound_count++;
}

void R_GetEntityMatrix(renderEntity_t const *entity, mat4_t *matrix) {
    Matrix4_identity(matrix);
    Matrix4_translate(matrix, &entity->origin);
}

void MDX_RenderModel(renderEntity_t const *entity, mdxModel_t const *model, mat4_t const *transform) {
    (void)model;
    test_spn_render_count++;
    test_spn_render_entity = *entity;
    test_spn_render_transform = *transform;
    if (tr.render_phase == RENDER_PHASE_ALPHA) {
        test_unit_alpha_order = ++test_alpha_draw_order;
    }
}

/* Sockets with empty paths still serve effects, but only authored child paths need the embedded-model pass. */
TEST(renderer_game, model_draw_skips_attachment_pose_without_children_and_keeps_child_birth_transform) {
    mdxAttachment_t first = { .node = { .name = "Sprite First Ref", .node_id = 0, .parent_id = (uint32_t)-1 } };
    mdxAttachment_t second = { .node = { .name = "Origin Ref", .node_id = 1, .parent_id = (uint32_t)-1 } };
    vec3_t pivots[] = {{1, 2, 3}, {4, 5, 6}};
    mdxSequence_t parent_seq = { .name = "Birth", .interval = {100, 300} };
    mdxSequence_t child_seq = { .name = "Birth", .interval = {1000, 1400} };
    mdxModel_t parent = { .attachments = &first, .pivots = pivots, .num_pivots = 2,
        .node_list = {&first.node, &second.node}, .num_nodes = 2, .sequences = &parent_seq, .num_sequences = 1 };
    mdxModel_t child = { .sequences = &child_seq, .num_sequences = 1 };
    model_t parent_model = { .modeltype = ID_MDLX, .mdx = &parent };
    model_t child_model = { .modeltype = ID_MDLX, .mdx = &child };
    renderEntity_t entity = { .model = &parent_model, .frame = 200, .oldframe = 100, .scale = 1,
        .origin = {10, 20, 30} };
    wc3AttachmentModel_t saved_models[32];
    uint32_t saved_count = wc3_attachment_model_count;
    mat4_t saved_nodes[] = {node_matrices[0], node_matrices[1]};
    render_phase_t saved_phase = tr.render_phase;
    vec3_t point;
    memcpy(saved_models, wc3_attachment_models, sizeof(saved_models));
    parent.nodes[0] = &first.node; parent.nodes[1] = &second.node; first.next = &second;
    tr.render_phase = RENDER_PHASE_SOLID;

    Matrix4_identity(&node_matrices[0]); node_matrices[0].v[12] = 777;
    test_spn_render_count = 0;
    R_RenderModel(&entity);
    T_EQ(test_spn_render_count, 1); T_FEQ(node_matrices[0].v[12], 777, 0.001f);
    /* The first socket stays empty: a later real child must still use the authored node basis and Birth time. */
    strlcpy(second.path, "Models\\Child.mdx", sizeof(second.path));
    wc3_attachment_models[0] = (wc3AttachmentModel_t){ .model = &child_model, .path = "Models\\Child.mdx" };
    wc3_attachment_model_count = 1; test_spn_render_count = 0;
    R_RenderModel(&entity);
    T_EQ(test_spn_render_count, 2); T_EQ(test_spn_render_entity.model, &child_model);
    T_EQ(test_spn_render_entity.frame, 1200); T_EQ(test_spn_render_entity.oldframe, 1000);
    T_FEQ(test_spn_render_transform.v[12], 10, 0.001f);
    T_FEQ(test_spn_render_transform.v[13], 20, 0.001f); T_FEQ(test_spn_render_transform.v[14], 30, 0.001f);
    T_ASSERT(test_spn_render_entity.flags & RF_NO_SHADOW);
    T_ASSERT(test_spn_render_entity.flags & RF_NO_FOGOFWAR);
    T_ASSERT(test_spn_render_entity.flags & RF_NO_UBERSPLAT);

    second.path[0] = '\0'; node_matrices[0].v[12] = 777; test_spn_render_count = 0;
    R_RenderModel(&entity);
    T_EQ(test_spn_render_count, 1); T_FEQ(node_matrices[0].v[12], 777, 0.001f);
    T_ASSERT(R_EntityAttachmentPosition(&entity, "Sprite ", &point));
    T_FEQ(point.x, 11, 0.001f); T_FEQ(point.y, 22, 0.001f); T_FEQ(point.z, 33, 0.001f);
    memcpy(wc3_attachment_models, saved_models, sizeof(saved_models)); wc3_attachment_model_count = saved_count;
    node_matrices[0] = saved_nodes[0]; node_matrices[1] = saved_nodes[1]; tr.render_phase = saved_phase;
}

void R_TestWaterAlphaSurfacePass(void) {
    test_water_alpha_order = ++test_alpha_draw_order;
}
uint32_t R_TestWorldPoseEvaluations;
uint32_t R_TestPoseFlushes;
void R_TestPoseFlush(void) { R_TestPoseFlushes++; }
void R_TestWorldPass(void) {
    extern _Atomic uint32_t MDLX_TestPoseEvaluations;
    R_TestWorldPoseEvaluations = MDLX_TestPoseEvaluations;
}
void R_TestLightningDraw(void) {}
void R_TestWeatherEmit(void) {}

void R_RenderRectSplatUV(rectSplatParams_t const *params) {
    test_splat_count++;
    test_splat_origin = MAKE(vec2_t, (params->mins->x + params->maxs->x) * 0.5f,
                             (params->mins->y + params->maxs->y) * 0.5f);
    test_splat_radius = (params->maxs->x - params->mins->x) * 0.5f;
    test_splat_color = params->color;
    test_splat_uv_mins = *params->uv_mins; test_splat_uv_maxs = *params->uv_maxs;
    if (R_TestCapturingSplats()) R_TestProductionRenderRectSplatUV(params);
}

void R_RenderSplat(vec2_t const *position, float radius, texture_t const *texture,
                   splat_shader_t *shader, color32_t color) {
    (void)texture; (void)shader;
    test_splat_count++;
    test_splat_origin = *position;
    test_splat_radius = radius;
    test_splat_color = color;
    if (R_TestCapturingSplats()) R_TestProductionRenderSplat(position, radius, texture, shader, color);
}

TEST(renderer_game, retained_event_splats_batch_adjacent_atlas_textures) {
    texture_t textures[2] = { { .texid = 1 }, { .texid = 2 } };
    wc3SplatData_t rows[2] = {
        { .name = "BatchA", .rows = 2, .columns = 2, .scale = 16, .lifespan = 2, .start_a = 1, .texture = textures },
        { .name = "BatchB", .rows = 2, .columns = 2, .scale = 16, .lifespan = 2, .start_a = 1, .texture = textures + 1 },
    };
    wc3UberSplatData_t uber = { .name = "BatchUber", .scale = 16, .pause_time = 2, .texture = textures + 1 };
    uint32_t time = tr.viewDef.time;
    render_phase_t phase = tr.render_phase;
    R_W3ClearEventSplats(); tr.viewDef.time = 100; tr.render_phase = RENDER_PHASE_SOLID;
    FOR_LOOP(i, 4) event_splats[i] = (wc3EventSplat_t){ .kind = WC3_EVENT_SPLAT_SPLAT, .splat_row = rows + i / 2,
        .origin = {64, 64}, .start_time = 0, .active = true };
    event_splats[3] = (wc3EventSplat_t){ .kind = WC3_EVENT_SPLAT_UBER, .uber_row = &uber,
        .origin = {64, 64}, .start_time = 0, .active = true };
    T_EQ(R_TestCaptureSplatPass(R_W3DrawEventSplats), 2);
    FOR_LOOP(i, 4) T_ASSERT(event_splats[i].active);
    tr.render_phase = RENDER_PHASE_ALPHA;
    T_EQ(R_TestCaptureSplatPass(R_W3DrawEventSplats), 0);
    tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 2000;
    T_EQ(R_TestCaptureSplatPass(R_W3DrawEventSplats), 0);
    FOR_LOOP(i, 4) T_ASSERT(!event_splats[i].active);
    R_W3ClearEventSplats(); tr.viewDef.time = time; tr.render_phase = phase;
}

void R_RenderFlatRectSplat(vec2_t const *mins, vec2_t const *maxs, float z,
                           texture_t const *texture, splat_shader_t *shader, color32_t color) {
    (void)mins; (void)maxs; (void)texture; (void)shader; (void)color;
    test_flat_splat_count++;
    test_flat_splat_z = z;
}

bool R_TestWalkableSurfaceTrace(renderEntity_t const *surface, line3_t const *line, vec3_t *hit) {
    (void)line;
    test_walkable_traces++;
    if (!surface || !(surface->flags & RF_GROUND_SURFACE) || !hit) return false;
    if (test_walkable_no_hit) return false;
    hit->z = test_walkable_surface_height ? surface->origin.z : test_walkable_hit_z;
    return true;
}

bool R_TestSetEntityAnimationFrame(model_t const *model, cstring_t anim, renderEntity_t *entity) {
    (void)model; (void)anim; (void)entity;
    return false;
}

TEST(renderer_game, water_supported_selection_ring_is_flat_at_water_surface) {
    model_t model = { 0 };
    renderEntity_t entity = {
        .origin = { 64.0f, 96.0f, 128.0f }, /* water surface 8 units below unit origin */
        .model = &model,
        .flags = RF_SELECTED | RF_SELECTION_CIRCLE_ON_WATER,
        .radius = 12.0f,
        .ground_offset = 8.0f,
    };
    renderEntity_t *saved_entities = tr.viewDef.entities;
    uint32_t saved_num_entities = tr.viewDef.num_entities;

    test_splat_count = 0;
    test_flat_splat_count = 0;
    tr.viewDef.entities = &entity;
    tr.viewDef.num_entities = 1;

    /* The entity pass defers this overlay; the post-water pass emits it as a
     * flat quad at its support water height, plus the one-unit depth bias. */
    R_RenderSelectedCircle(&entity, (vec2_t const *)&entity.origin, false);
    T_EQ(test_splat_count, 0);
    T_EQ(test_flat_splat_count, 0);
    R_DrawSupportedEntityOverlays();

    T_EQ(test_splat_count, 0);
    T_EQ(test_flat_splat_count, 1);
    T_FEQ(test_flat_splat_z, 121.0f, 0.001f);

    tr.viewDef.entities = saved_entities;
    tr.viewDef.num_entities = saved_num_entities;
}

TEST(renderer_game, conform_support_classifies_each_view_once_and_clears_stale_support) {
    model_t model = {0}; renderEntity_t entities[256] = {0};
    viewDef_t view = {.entities = entities, .num_entities = 256};
    FOR_LOOP(i, 256) entities[i] = (renderEntity_t){.model = &model, .flags = RF_GROUND_SURFACE_SUPPORT};
    FOR_LOOP(i, 3) { entities[i].flags |= RF_GROUND_CONFORM; entities[i].ground_offset = 5 + i; }
    entities[3].flags |= RF_GROUND_CONFORM | RF_HIDDEN;
    entities[4].flags |= RF_GROUND_CONFORM; entities[4].model = NULL;
    FOR_LOOP(i, 3) entities[10 + i].flags |= RF_GROUND_SURFACE;
    entities[13].flags |= RF_GROUND_SURFACE | RF_HIDDEN;
    entities[14].flags |= RF_GROUND_SURFACE; entities[14].model = NULL;
    test_walkable_hit_z = 77; R_TestConformClassifications = 0;
    R_ConformGroundSurfaces(&view);
    T_ASSERT(R_TestConformClassifications <= 2 * 256 + 3 * 3);
    FOR_LOOP(i, 3) { T_FEQ(entities[i].origin.z, 82 + i, 0.001f); T_ASSERT(entities[i].flags & RF_GROUND_SURFACE_SUPPORT); }
    FOR_LOOP(i, 253) T_ASSERT(!(entities[i + 3].flags & RF_GROUND_SURFACE_SUPPORT));
    /* A later view without surfaces must clear support rather than reuse a prior candidate. */
    FOR_LOOP(i, 5) entities[10 + i].flags &= ~RF_GROUND_SURFACE;
    R_TestConformClassifications = 0; R_ConformGroundSurfaces(&view);
    T_ASSERT(R_TestConformClassifications <= 2 * 256);
    FOR_LOOP(i, 256) T_ASSERT(!(entities[i].flags & RF_GROUND_SURFACE_SUPPORT));
    entities[0].flags |= RF_GROUND_SURFACE_SUPPORT; view.rdflags = RDF_NOWORLDMODEL;
    R_TestConformClassifications = 0; R_ConformGroundSurfaces(&view);
    T_EQ(R_TestConformClassifications, 0); T_ASSERT(entities[0].flags & RF_GROUND_SURFACE_SUPPORT);
}

TEST(renderer_game, conform_support_preserves_surface_order_overflow_highest_hit_and_no_hit) {
    model_t model = {0}; renderEntity_t entities[66] = {0};
    viewDef_t view = {.entities = entities, .num_entities = 66};
    FOR_LOOP(i, 65) entities[i] = (renderEntity_t){.model = &model, .flags = RF_GROUND_SURFACE, .origin.z = i};
    entities[0].origin.z = 1000; entities[64].origin.z = 999;
    entities[65] = (renderEntity_t){.model = &model, .flags = RF_GROUND_CONFORM, .ground_offset = 7, .origin.z = 3};
    test_walkable_surface_height = true; test_walkable_traces = 0;
    R_ConformGroundSurfaces(&view);
    T_EQ(test_walkable_traces, 65); T_FEQ(entities[65].origin.z, 1007, 0.001f);
    entities[0].flags |= RF_HIDDEN; entities[65].origin.z = 3; test_walkable_traces = 0;
    R_ConformGroundSurfaces(&view);
    T_EQ(test_walkable_traces, 64); T_FEQ(entities[65].origin.z, 1006, 0.001f);
    entities[64].model = NULL; entities[65].origin.z = 3; test_walkable_traces = 0;
    R_ConformGroundSurfaces(&view);
    T_EQ(test_walkable_traces, 63); T_FEQ(entities[65].origin.z, 70, 0.001f);
    entities[65].origin.z = 3; test_walkable_no_hit = true; test_walkable_traces = 0;
    R_ConformGroundSurfaces(&view);
    T_EQ(test_walkable_traces, 63); T_FEQ(entities[65].origin.z, 3, 0.001f);
    T_ASSERT(!(entities[65].flags & RF_GROUND_SURFACE_SUPPORT));
    test_walkable_surface_height = test_walkable_no_hit = false;
}

TEST(renderer_game, ordinary_selected_ring_still_uses_terrain_conforming_path) {
    renderEntity_t entity = {
        .origin = { 64.0f, 96.0f, 128.0f },
        .flags = RF_SELECTED,
        .radius = 12.0f,
    };
    vec2_t const origin = { entity.origin.x, entity.origin.y };

    test_splat_count = 0;
    test_flat_splat_count = 0;
    R_RenderSelectedCircle(&entity, &origin, false);

    T_EQ(test_splat_count, 1);
    T_EQ(test_flat_splat_count, 0);
    T_FEQ(test_splat_origin.x, entity.origin.x, 0.001f);
    T_FEQ(test_splat_origin.y, entity.origin.y, 0.001f);
}

TEST(renderer_game, bridge_supported_selection_ring_uses_walkable_surface_height) {
    model_t bridge_model = { 0 }, unit_model = { 0 };
    renderEntity_t entities[] = {
        { .origin = { 64.0f, 96.0f, 40.0f }, .model = &bridge_model,
          .flags = RF_GROUND_SURFACE },
        { .origin = { 64.0f, 96.0f, 0.0f }, .model = &unit_model,
          .flags = RF_SELECTED | RF_GROUND_CONFORM, .radius = 12.0f, .ground_offset = 8.0f },
    };
    renderEntity_t *saved_entities = tr.viewDef.entities;
    uint32_t saved_num_entities = tr.viewDef.num_entities;
    viewDef_t view = { .entities = entities, .num_entities = 2 };

    test_walkable_hit_z = 77.0f;
    R_ConformGroundSurfaces(&view);
    T_ASSERT(entities[1].flags & RF_GROUND_SURFACE_SUPPORT);
    T_FEQ(entities[1].origin.z, 85.0f, 0.001f);

    test_splat_count = 0;
    test_flat_splat_count = 0;
    tr.viewDef.entities = entities;
    tr.viewDef.num_entities = 2;
    R_DrawSupportedEntityOverlays();
    T_EQ(test_splat_count, 0);
    T_EQ(test_flat_splat_count, 1);
    T_FEQ(test_flat_splat_z, 78.0f, 0.001f);

    tr.viewDef.entities = saved_entities;
    tr.viewDef.num_entities = saved_num_entities;
}

TEST(renderer_game, elevator_render_support_cannot_lower_authoritative_unit_height) {
    model_t bridge_model = { 0 }, unit_model = { 0 };
    renderEntity_t entities[] = {
        { .origin = { 64.0f, 96.0f, 40.0f }, .model = &bridge_model, .flags = RF_GROUND_SURFACE },
        { .origin = { 64.0f, 96.0f, 160.0f }, .model = &unit_model,
          .flags = RF_GROUND_CONFORM, .ground_offset = 8.0f,
          .ground_snapshot_z = 264.0f, .ground_snapshot_valid = true },
    };
    viewDef_t view = { .entities = entities, .num_entities = 2 };

    test_walkable_hit_z = 77.0f; /* animated mesh is still below the raised deck */
    R_ConformGroundSurfaces(&view);
    T_ASSERT(entities[1].flags & RF_GROUND_SURFACE_SUPPORT);
    T_FEQ(entities[1].origin.z, 264.0f, 0.001f);

    /* A later lower snapshot may descend again; no sticky height is cached. */
    entities[1].ground_snapshot_z = 64.0f;
    entities[1].origin.z = 64.0f;
    R_ConformGroundSurfaces(&view);
    T_FEQ(entities[1].origin.z, 85.0f, 0.001f);
}

TEST(renderer_game, unit_alpha_models_draw_after_the_water_alpha_pass) {
    mdxModel_t mdx = { 0 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    renderEntity_t entity = { .origin = { 10.0f, 20.0f, 30.0f }, .model = &model };
    renderEntity_t *saved_entities = tr.viewDef.entities;
    uint32_t saved_num_entities = tr.viewDef.num_entities;
    render_phase_t saved_phase = tr.render_phase;

    test_alpha_draw_order = test_water_alpha_order = test_unit_alpha_order = 0;
    tr.viewDef.entities = &entity;
    tr.viewDef.num_entities = 1;
    tr.render_phase = RENDER_PHASE_ALPHA;
    R_DrawAlphaSurfaces();

    T_ASSERT(test_water_alpha_order > 0);
    T_ASSERT(test_unit_alpha_order > test_water_alpha_order);

    tr.viewDef.entities = saved_entities;
    tr.viewDef.num_entities = saved_num_entities;
    tr.render_phase = saved_phase;
}

TEST(renderer_model, camera_alpha_pass_draws_owned_event_children_without_world_water) {
    mdxSequence_t sequence = { .interval = { 0, 1000 } };
    mdxModel_t mdx = { .sequences = &sequence, .num_sequences = 1 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    renderEntity_t source = { .model = &model, .instance_id = 7654 };
    viewDef_t saved_view = tr.viewDef;
    render_phase_t saved_phase = tr.render_phase;

    R_W3ClearEventSpawns();
    event_spawns[0] = (wc3EventSpawn_t){ .model = &model, .source_model = &model,
        .source_instance_id = source.instance_id, .entity_camera = true, .active = true,
        .scale = 1, .event_frame = 17, .event_render_time = 18, .event_valid = true };
    Matrix4_identity(&event_spawns[0].transform);
    tr.viewDef = (viewDef_t){ .time = 100, .entities = &source, .num_entities = 1,
        .rdflags = RDF_NOWORLDMODEL | RDF_USE_ENTITY_CAMERA };
    tr.render_phase = RENDER_PHASE_ALPHA;
    test_spn_render_count = test_water_alpha_order = 0;
    R_DrawAlphaSurfaces();
    T_EQ(test_spn_render_count, 2); /* Source layers and the retained child layers. */
    T_EQ(test_spn_render_entity.number, MAX_GAME_ENTITIES + 1);
    T_EQ(test_water_alpha_order, 0);
    T_EQ(event_spawns[0].event_frame, 17);
    T_EQ(event_spawns[0].event_render_time, 18);
    source.instance_id++;
    R_DrawAlphaSurfaces();
    T_EQ(test_spn_render_count, 3); /* The other camera draws only its source. */
    R_W3ClearEventSpawns();
    tr.viewDef = saved_view;
    tr.render_phase = saved_phase;
}

TEST(renderer_model, production_spn_dispatch_retains_spawn_after_parent_update) {
    static uint32_t key = 100;
    static vec3_t pivot = { 1.0f, 2.0f, 3.0f };
    static mdxSequence_t parent_sequence = { .interval = { 0, 1000 } };
    static mdxEvent_t event;
    static mdxModel_t parent_mdx;
    static model_t parent_model;
    static renderEntity_t parent;
    renderEntity_t camera_parent;
    wc3EventState_t saved_state = event_state[7];
    uint32_t saved_time = tr.viewDef.time;
    uint32_t saved_rdflags = tr.viewDef.rdflags;
    render_phase_t saved_phase = tr.render_phase;
    refImport_t saved_imports = ri;
    model_t *child_model = NULL;

    memset(&event, 0, sizeof(event));
    event.num_keys = 1; event.globalSeqId = (uint32_t)-1; event.keys = &key;
    parent_mdx = (mdxModel_t){ .events = &event, .sequences = &parent_sequence,
                               .num_sequences = 1, .pivots = &pivot, .num_pivots = 1 };
    parent_model = (model_t){ .modeltype = ID_MDLX, .mdx = &parent_mdx };
    parent = (renderEntity_t){ .origin = { 10.0f, 20.0f, 30.0f }, .model = &parent_model,
                               .number = 7, .team = 2, .scale = 1.0f };
    snprintf(event.node.name, sizeof(event.node.name), "SPNxTestSpawn");
    event.node.node_id = 0; event.node.parent_id = (uint32_t)-1;
    parent_mdx.nodes[0] = &event.node; parent_mdx.node_list[0] = &event.node; parent_mdx.num_nodes = 1;
    R_SetMapAssetScope(NULL); R_ShutdownModels();
    T_ASSERT(SFileOpenArchive("build/tests/tests.mpq", 0, 0, &test_renderer_archive));
    if (!test_renderer_archive) goto cleanup_spn_test;
    ri.FS_ReadFile = test_renderer_read; ri.FS_FreeFile = test_renderer_free;
    ri.LoadSlk = test_renderer_load_slk; ri.MemAlloc = test_renderer_alloc; ri.MemFree = test_renderer_free;
    R_W3LoadSpawnData();
    T_EQ(spawn_data_count, 1);
    if (!spawn_data_rows || spawn_data_count != 1) goto cleanup_spn_test;
    T_STREQ(spawn_data_rows[0].name, "TestSpawn");
    T_STREQ(spawn_data_rows[0].model_path, "TestUI\\Models\\quad_sprite.mdx");
    R_W3LoadSplatData();
    T_EQ(splat_data_count, 1);
    if (!splat_data_rows || splat_data_count != 1) goto cleanup_spn_test;
    T_STREQ(splat_data_rows[0].name, "TestSplat");
    T_STREQ(splat_data_rows[0].blend_mode, "0");
    T_EQ(splat_data_rows[0].rows, 2); T_EQ(splat_data_rows[0].columns, 2);
    T_EQ(splat_data_rows[0].uv_lifespan_start, 0); T_EQ(splat_data_rows[0].uv_decay_end, 3);
    R_W3LoadUberSplatData();
    T_EQ(uber_splat_count, 1);
    if (!uber_splat_rows || uber_splat_count != 1) goto cleanup_spn_test;
    T_STREQ(uber_splat_rows[0].name, "TestUber");
    T_STREQ(uber_splat_rows[0].blend_mode, "0");
    T_FEQ(uber_splat_rows[0].scale, 64.0f, 0.001f);
    T_FEQ(uber_splat_rows[0].birth_time, 1.0f, 0.001f);
    R_TestUseProductionModelLoader(true);
    child_model = R_W3SpawnModel(spawn_data_rows);
    T_NOT_NULL(child_model);
    if (!child_model || !child_model->mdx) goto cleanup_spn_test;
    T_EQ(child_model->modeltype, ID_MDLX);
    T_NOT_NULL(child_model->mdx->sequences);
    T_ASSERT(child_model->mdx->num_sequences > 0);
    T_EQ(parent_mdx.num_pivots, 1);
    T_EQ(R_W3SpawnModel(spawn_data_rows), child_model);
    event_state[7] = (wc3EventState_t){ 0 }; R_W3ClearEventSpawns();
    test_spn_render_count = 0; tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 0;

    R_UpdateEntityPresentation(&parent); /* First observation seeds the crossing state. */
    T_EQ(test_spn_render_count, 0);
    parent.frame = 150; tr.viewDef.time = 150;
    R_UpdateEntityPresentation(&parent);
    T_EQ(test_spn_render_count, 1);
    T_EQ(test_spn_render_entity.model, child_model);
    T_EQ(test_spn_render_entity.team, 2);
    T_ASSERT(test_spn_render_entity.number > MAX_GAME_ENTITIES);
    T_FEQ(test_spn_render_transform.v[12], 11.0f, 0.001f);
    T_FEQ(test_spn_render_transform.v[13], 22.0f, 0.001f);
    T_FEQ(test_spn_render_transform.v[14], 33.0f, 0.001f);

    /* Drawing the pool does not depend on another parent entity update. */
    tr.viewDef.time = 250; R_W3DrawEventSpawns(false, NULL, 0);
    T_EQ(test_spn_render_count, 2);
    T_EQ(test_spn_render_entity.frame, 100);
    tr.render_phase = RENDER_PHASE_LIGHTS; R_W3DrawEventSpawns(false, NULL, 0);
    T_EQ(test_spn_render_count, 2);
    tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 1150; R_W3DrawEventSpawns(false, NULL, 0);
    T_EQ(test_spn_render_count, 2);
    T_ASSERT(!event_spawns[0].active);

    /* Separate entity cameras can use the same synthetic entity number. Their
     * event clocks must remain independent so both authored SPN keys fire. */
    R_W3ClearEventSpawns();
    test_spn_render_count = 0;
    event_state[7] = (wc3EventState_t){ 0 };
    parent = (renderEntity_t){ .origin = { 10.0f, 20.0f, 30.0f }, .model = &parent_model,
                               .number = 7, .team = 2, .scale = 1.0f, .instance_id = 1001 };
    camera_parent = parent;
    camera_parent.instance_id = 1002;
    tr.viewDef.rdflags = RDF_USE_ENTITY_CAMERA;
    tr.viewDef.time = 200;
    R_UpdateEntityPresentation(&parent);
    R_UpdateEntityPresentation(&camera_parent);
    parent.frame = camera_parent.frame = 150;
    tr.viewDef.time = 350;
    R_UpdateEntityPresentation(&parent);
    R_UpdateEntityPresentation(&camera_parent);
    T_EQ(test_spn_render_count, 2);
    T_ASSERT(event_spawns[0].entity_camera && event_spawns[1].entity_camera);
    T_EQ(event_spawns[0].source_instance_id, 1001);
    T_EQ(event_spawns[1].source_instance_id, 1002);
    R_DrawEntityCameraEventSpawns(&parent_model, parent.instance_id);
    T_EQ(test_spn_render_count, 3);
    R_DrawEntityCameraEventSpawns(camera_parent.model, camera_parent.instance_id);
    T_EQ(test_spn_render_count, 4);
    R_W3DrawEventSpawns(false, NULL, 0);
    T_EQ(test_spn_render_count, 4);
    T_EQ(camera_event_state_count, 2);
    for (uint32_t i = 0; i < 40; i++) {
        camera_parent.instance_id = 2000 + i;
        T_NOT_NULL(R_W3CameraEventState(&camera_parent));
    }
    camera_parent.instance_id = 1002;
    T_EQ(camera_event_state_count, 42);
    T_EQ(R_W3CameraEventState(&parent)->frame, 150);
    R_ReleaseGameEntityCameraEvents(parent.instance_id);
    T_EQ(camera_event_state_count, 41);
    R_DrawEntityCameraEventSpawns(&parent_model, parent.instance_id);
    T_EQ(test_spn_render_count, 4);
    R_ReleaseGameEntityCameraEvents(camera_parent.instance_id);
    T_EQ(camera_event_state_count, 40);
    R_W3ClearCameraEventStates();
    T_EQ(camera_event_state_count, 0);

cleanup_spn_test:
    R_W3ClearEventSpawns();
    R_W3ClearCameraEventStates();
    R_W3ClearEventSplats();
    R_W3FreeSpawnData(true);
    R_W3FreeSplatData();
    R_W3FreeUberSplatData();
    R_ShutdownModels();
    R_TestUseProductionModelLoader(false);
    if (test_renderer_archive) { SFileCloseArchive(test_renderer_archive); test_renderer_archive = NULL; }
    ri = saved_imports;
    event_state[7] = saved_state; tr.viewDef.time = saved_time; tr.viewDef.rdflags = saved_rdflags;
    tr.render_phase = saved_phase;
}

TEST(renderer_model, production_spl_dispatch_uses_splat_atlas_and_event_transform) {
    static uint32_t key = 100;
    static vec3_t pivot = { 1.0f, 2.0f, 3.0f };
    static mdxSequence_t sequence = { .interval = { 0, 3000 } };
    mdxEvent_t event = { .num_keys = 1, .globalSeqId = (uint32_t)-1, .keys = &key };
    mdxModel_t mdx = { .events = &event, .sequences = &sequence, .num_sequences = 1,
                       .pivots = &pivot, .num_pivots = 1 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    renderEntity_t entity = { .origin = { 10.0f, 20.0f, 30.0f }, .model = &model, .number = 10 };
    wc3SplatData_t row = {
        .name = "TestSplat", .rows = 2, .columns = 2, .scale = 32.0f, .lifespan = 1.0f, .decay_time = 1.0f,
        .uv_lifespan_start = 0, .uv_lifespan_end = 1, .uv_decay_start = 2, .uv_decay_end = 3,
        .start_r = 1.0f, .start_a = 1.0f, .middle_g = 1.0f, .middle_a = 1.0f,
        .end_b = 1.0f, .end_a = 0.0f, .texture = &test_splat_texture,
    };
    wc3SplatData_t *saved_rows = splat_data_rows;
    uint32_t saved_count = splat_data_count, saved_time = tr.viewDef.time;
    render_phase_t saved_phase = tr.render_phase;
    wc3EventState_t saved_state = event_state[10];

    snprintf(event.node.name, sizeof(event.node.name), "SPLxTestSplat");
    event.node.node_id = 0; event.node.parent_id = (uint32_t)-1;
    mdx.nodes[0] = &event.node; mdx.node_list[0] = &event.node; mdx.num_nodes = 1;
    splat_data_rows = &row; splat_data_count = 1;
    event_state[10] = (wc3EventState_t){ 0 }; R_W3ClearEventSplats();
    test_splat_count = 0; tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 0;

    R_UpdateEntityPresentation(&entity);
    entity.frame = 150; tr.viewDef.time = 150; R_UpdateEntityPresentation(&entity);
    T_EQ(test_splat_count, 1);
    T_FEQ(test_splat_origin.x, 11.0f, 0.001f); T_FEQ(test_splat_origin.y, 22.0f, 0.001f);
    T_FEQ(test_splat_radius, 32.0f, 0.001f);
    T_FEQ(test_splat_uv_mins.x, 0.0f, 0.001f); T_FEQ(test_splat_uv_mins.y, 0.0f, 0.001f);
    T_FEQ(test_splat_uv_maxs.x, 0.5f, 0.001f); T_FEQ(test_splat_uv_maxs.y, 0.5f, 0.001f);

    tr.viewDef.time = 1150; R_W3DrawEventSplats();
    T_EQ(test_splat_count, 2);
    T_FEQ(test_splat_uv_mins.x, 0.0f, 0.001f); T_FEQ(test_splat_uv_mins.y, 0.5f, 0.001f);
    T_EQ(test_splat_color.r, 0); T_EQ(test_splat_color.g, 255); T_EQ(test_splat_color.b, 0);
    tr.viewDef.time = 2150; R_W3DrawEventSplats();
    T_EQ(test_splat_count, 2); T_ASSERT(!event_splats[0].active);

    R_W3ClearEventSplats();
    splat_data_rows = saved_rows; splat_data_count = saved_count;
    event_state[10] = saved_state; tr.viewDef.time = saved_time; tr.render_phase = saved_phase;
}

TEST(renderer_model, production_fpt_dispatch_uses_splat_data) {
    mdxEvent_t event = { 0 };
    wc3EventFamily_t const *family;
    char id[32] = { 0 };

    snprintf(event.node.name, sizeof(event.node.name), "FPTxTestSplat   ");
    family = R_W3EventFamily(&event);
    T_NOT_NULL(family);
    T_EQ(family->kind, WC3_EVENT_FOOTPRINT);
    T_ASSERT(MDLX_EventObjectId(&event, family->prefix, id, sizeof(id)));
    T_STREQ(id, "TestSplat");
}

TEST(renderer_model, production_ubr_dispatch_uses_data_lifetime_and_event_transform) {
    static uint32_t key = 100;
    static vec3_t pivot = { 1.0f, 2.0f, 3.0f };
    static mdxSequence_t sequence = { .interval = { 0, 4000 } };
    mdxEvent_t event = { .num_keys = 1, .globalSeqId = (uint32_t)-1, .keys = &key };
    mdxModel_t mdx = { .events = &event, .sequences = &sequence, .num_sequences = 1,
                       .pivots = &pivot, .num_pivots = 1 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    renderEntity_t entity = { .origin = { 10.0f, 20.0f, 30.0f }, .model = &model, .number = 9 };
    wc3UberSplatData_t row = {
        .name = "TestUber", .scale = 64.0f, .birth_time = 1.0f, .pause_time = 1.0f, .decay_time = 1.0f,
        .start_r = 1.0f, .start_a = 1.0f,
        .middle_g = 1.0f, .middle_a = 1.0f,
        .end_b = 1.0f, .end_a = 0.0f,
        .texture = &test_splat_texture,
    };
    wc3UberSplatData_t *saved_rows = uber_splat_rows;
    uint32_t saved_count = uber_splat_count, saved_time = tr.viewDef.time;
    render_phase_t saved_phase = tr.render_phase;
    wc3EventState_t saved_state = event_state[9];

    snprintf(event.node.name, sizeof(event.node.name), "UBRxTestUber");
    event.node.node_id = 0; event.node.parent_id = (uint32_t)-1;
    mdx.nodes[0] = &event.node; mdx.node_list[0] = &event.node; mdx.num_nodes = 1;
    uber_splat_rows = &row; uber_splat_count = 1;
    event_state[9] = (wc3EventState_t){ 0 }; R_W3ClearEventSplats();
    test_splat_count = 0; tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 0;

    R_UpdateEntityPresentation(&entity);
    entity.frame = 150; tr.viewDef.time = 150; R_UpdateEntityPresentation(&entity);
    T_EQ(test_splat_count, 1);
    T_FEQ(test_splat_origin.x, 11.0f, 0.001f);
    T_FEQ(test_splat_origin.y, 22.0f, 0.001f);
    T_FEQ(test_splat_radius, 64.0f, 0.001f);
    T_EQ(test_splat_color.r, 255); T_EQ(test_splat_color.g, 0); T_EQ(test_splat_color.b, 0);

    tr.viewDef.time = 1650; R_W3DrawEventSplats();
    T_EQ(test_splat_count, 2);
    T_EQ(test_splat_color.r, 0); T_EQ(test_splat_color.g, 255); T_EQ(test_splat_color.b, 0);
    tr.viewDef.time = 3150; R_W3DrawEventSplats();
    T_EQ(test_splat_count, 2);
    T_ASSERT(!event_splats[0].active);

    R_W3ClearEventSplats();
    uber_splat_rows = saved_rows; uber_splat_count = saved_count;
    event_state[9] = saved_state; tr.viewDef.time = saved_time; tr.render_phase = saved_phase;
}

TEST(renderer_model, production_spn_child_dispatches_its_own_ubr_event) {
    static uint32_t key = 100;
    static vec3_t pivot = { 1.0f, 2.0f, 3.0f };
    static mdxSequence_t sequence = { .interval = { 0, 1000 } };
    mdxEvent_t event = { .num_keys = 1, .globalSeqId = (uint32_t)-1, .keys = &key };
    mdxModel_t mdx = { .events = &event, .sequences = &sequence, .num_sequences = 1,
                       .pivots = &pivot, .num_pivots = 1 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    wc3UberSplatData_t row = {
        .name = "TestUber", .scale = 16.0f, .birth_time = 1.0f, .pause_time = 1.0f, .decay_time = 1.0f,
        .start_r = 1.0f, .start_a = 1.0f, .middle_r = 1.0f, .middle_a = 1.0f,
        .end_r = 1.0f, .end_a = 1.0f, .texture = &test_splat_texture,
    };
    wc3EventSpawn_t spawn = { .model = &model, .scale = 1.0f, .active = true, .serial = 1 };
    wc3UberSplatData_t *saved_rows = uber_splat_rows;
    uint32_t saved_count = uber_splat_count, saved_time = tr.viewDef.time;
    render_phase_t saved_phase = tr.render_phase;

    snprintf(event.node.name, sizeof(event.node.name), "UBRxTestUber");
    event.node.node_id = 0; event.node.parent_id = (uint32_t)-1;
    mdx.nodes[0] = &event.node; mdx.node_list[0] = &event.node; mdx.num_nodes = 1;
    Matrix4_identity(&spawn.transform);
    spawn.transform.v[12] = 10.0f; spawn.transform.v[13] = 20.0f; spawn.transform.v[14] = 30.0f;
    uber_splat_rows = &row; uber_splat_count = 1;
    R_W3ClearEventSplats(); test_splat_count = 0; tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 0;

    T_ASSERT(R_W3RenderEventSpawn(&spawn, 0)); /* seed child event state */
    tr.viewDef.time = 150;
    T_ASSERT(R_W3RenderEventSpawn(&spawn, 0));
    T_EQ(test_splat_count, 1);
    T_FEQ(test_splat_origin.x, 11.0f, 0.001f);
    T_FEQ(test_splat_origin.y, 22.0f, 0.001f);

    R_W3ClearEventSplats();
    uber_splat_rows = saved_rows; uber_splat_count = saved_count;
    tr.viewDef.time = saved_time; tr.render_phase = saved_phase;
}

TEST(renderer_model, production_snd_dispatch_uses_event_world_transform) {
    static uint32_t key = 100;
    static vec3_t pivot = { 1.0f, 2.0f, 3.0f };
    static mdxSequence_t sequence = { .interval = { 0, 1000 } };
    static wc3AnimSound_t sound = { .name = "TestSound", .files = "hit.wav", .directory = "Sounds", .volume = 127.0f };
    mdxEvent_t event = { .num_keys = 1, .globalSeqId = (uint32_t)-1, .keys = &key };
    mdxModel_t mdx = { .events = &event, .sequences = &sequence, .num_sequences = 1,
                       .pivots = &pivot, .num_pivots = 1 };
    model_t model = { .modeltype = ID_MDLX, .mdx = &mdx };
    renderEntity_t entity = { .origin = { 10.0f, 20.0f, 30.0f }, .model = &model, .number = 8 };
    wc3AnimSound_t *saved_sounds = anim_sound_rows;
    uint32_t saved_sound_count = anim_sound_count, saved_time = tr.viewDef.time;
    render_phase_t saved_phase = tr.render_phase;
    wc3EventState_t saved_state = event_state[8];
    void (*saved_play_sound)(cstring_t, vec3_t const *, float) = ri.PlaySoundAt;

    snprintf(event.node.name, sizeof(event.node.name), "SNDxTestSound");
    event.node.node_id = 0; event.node.parent_id = (uint32_t)-1;
    mdx.nodes[0] = &event.node; mdx.node_list[0] = &event.node; mdx.num_nodes = 1;
    anim_sound_rows = &sound; anim_sound_count = 1; event_state[8] = (wc3EventState_t){ 0 };
    test_sound_count = 0; test_sound_path[0] = '\0'; ri.PlaySoundAt = test_renderer_play_sound;
    tr.render_phase = RENDER_PHASE_SOLID; tr.viewDef.time = 0;
    R_UpdateEntityPresentation(&entity);
    entity.frame = 150; tr.viewDef.time = 150; R_UpdateEntityPresentation(&entity);
    T_EQ(test_sound_count, 1);
    T_STREQ(test_sound_path, "Sounds\\hit.wav");
    T_FEQ(test_sound_origin.x, 11.0f, 0.001f);
    T_FEQ(test_sound_origin.y, 22.0f, 0.001f);
    T_FEQ(test_sound_origin.z, 33.0f, 0.001f);
    anim_sound_rows = saved_sounds; anim_sound_count = saved_sound_count;
    ri.PlaySoundAt = saved_play_sound; event_state[8] = saved_state;
    tr.viewDef.time = saved_time; tr.render_phase = saved_phase;
}

static drawSprite_t cursor_drawn;
static color32_t cursor_tint;
void R_TestCursorSprite(drawSprite_t const *sprite, color32_t tint) {
    cursor_drawn = *sprite; cursor_tint = tint;
}
mdxSequence_t const *R_TestCursorSequence(mdxModel_t const *model, cstring_t name) {
    for (int i = 0; i < model->num_sequences; i++)
        if (!strcmp(model->sequences[i].name, name)) return &model->sequences[i];
    return NULL;
}

TEST(renderer_cursor, retail_restart_and_paused_clock) {
    mdxSequence_t sequences[] = {
        { .name = "Normal", .interval = {333, 533} },
        { .name = "Select", .interval = {1000, 1500} },
        /* Deliberately non-stock duration: the model owns the interval. */
        { .name = "Scroll Right", .interval = {2000, 2273} },
    };
    mdxModel_t mdx = { .sequences = sequences, .num_sequences = 3 };
    model_t model = { .mdx = &mdx, .modeltype = ID_MDLX };
    drawCursor_t cursor = { .origin = {0.3f, 0.2f}, .tint = {255, 220, 80, 255}, .time = 100 };
    cursor_model = &model; cursor_anim = NULL;
    tr.viewDef.time = 100;
    T_ASSERT(R_DrawCursor(&cursor));
    T_STREQ(cursor_drawn.anim, "Normal");
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 0);
    /* Simulation remains frozen throughout; presentation keeps advancing. */
    cursor.time = 290; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 190);
    cursor.time = 315; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 15);
    cursor.time = 331; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 16);
    cursor.hover = true; cursor.time = 351; R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "Select");
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 20);
    T_EQ(cursor_tint.g, 255);
    cursor.scroll.x = 1; cursor.time = 381; R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "Scroll Right");
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 30);
    T_EQ(cursor_tint.g, 255);
    /* A long frame crosses several loops; no 500ms clamp. */
    cursor.time = 1706; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 263);
    cursor.time = 1723; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 17);
    cursor_model = NULL; cursor_anim = NULL;
}

TEST(renderer_cursor, d3d_hotspot_pixel_centers) {
    mdxSequence_t sequence = { .name = "Normal", .interval = {333, 533} };
    mdxModel_t mdx = { .sequences = &sequence, .num_sequences = 1 };
    model_t model = { .mdx = &mdx, .modeltype = ID_MDLX };
    drawCursor_t cursor = { .origin = {0.403125f, 0.30078125f}, .tint = COLOR32_WHITE };
    cursor_model = &model; cursor_anim = NULL;
    tr.uiScene = (rect_t){0, 0, 0.8f, 0.6f};
    tr.drawableSize = (size2_t){1024, 768};
    R_DrawCursor(&cursor);
    T_FEQ(cursor_drawn.x, 0.403515625f, 0.0000001f);
    T_FEQ(cursor_drawn.y, 0.301171875f, 0.0000001f);
    /* Expanded canvas and high-DPI drawable: correction is half a physical pixel. */
    tr.uiScene.w = 1.2f;
    tr.drawableSize = (size2_t){2400, 1350};
    R_DrawCursor(&cursor);
    T_FEQ(cursor_drawn.x, cursor.origin.x + 0.00025f, 0.0000001f);
    T_FEQ(cursor_drawn.y, cursor.origin.y + 0.6f / 2700, 0.0000001f);
    cursor_model = NULL; cursor_anim = NULL;
    tr.uiScene = (rect_t){0}; tr.drawableSize = (size2_t){0};
}

TEST(renderer_cursor, nonlooping_custom_sequence_holds_endpoint) {
    mdxSequence_t sequence = { .name = "Normal", .interval = {600, 717}, .flags = 1 };
    mdxModel_t mdx = { .sequences = &sequence, .num_sequences = 1 };
    model_t model = { .mdx = &mdx, .modeltype = ID_MDLX };
    drawCursor_t cursor = { .tint = COLOR32_WHITE, .time = 100 };
    cursor_model = &model; cursor_anim = NULL;
    tr.viewDef.time = 0;
    R_DrawCursor(&cursor);
    cursor.time = 250; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 117);
    cursor.time = 270; R_DrawCursor(&cursor);
    T_EQ(tr.viewDef.time - cursor_drawn.start_time, 117);
    cursor_model = NULL; cursor_anim = NULL;
}

TEST(renderer_cursor, authored_model_target_and_held_item) {
    mdxSequence_t seqs[] = {
        { .name = "Normal", .interval = {100, 300} },
        { .name = "Target", .interval = {500, 550} },
        { .name = "TargetSelect", .interval = {700, 900} },
        { .name = "HoldItem", .interval = {1000, 1200} },
        { .name = "Scroll Right", .interval = {1500, 1700} },
    };
    mdxModel_t mdx = { .sequences = seqs, .num_sequences = 5 };
    model_t model = { .mdx = &mdx, .modeltype = ID_MDLX }, other = model;
    texture_t icon = {0};
    drawCursor_t cursor = { .model = &model, .game = true, .tint = COLOR32_WHITE, .interaction = WC3_POINTER_TARGETING, .time = 100 };
    cursor_model = &model; cursor_anim = NULL;
    T_ASSERT(R_DrawCursor(&cursor));
    T_STREQ(cursor_drawn.anim, "Target");
    cursor.hover = true; cursor.hostile = true; cursor.tint = (color32_t){255, 0, 0, 255}; cursor.time = 120;
    R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "TargetSelect"); T_EQ(cursor_tint.r, 255); T_EQ(cursor_tint.g, 0);
    cursor.interaction = WC3_POINTER_HOLDING; cursor.skin = &icon; cursor.time = 140;
    R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "HoldItem"); T_ASSERT(cursor_drawn.skin == &icon); T_EQ(cursor_tint.g, 255);
    T_EQ(cursor_drawn.skin_slot, 21);
    cursor.scroll.x = 1; R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "Scroll Right"); T_NULL(cursor_drawn.skin);
    cursor.scroll.x = 0; R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "HoldItem"); T_ASSERT(cursor_drawn.skin == &icon);
    cursor.interaction = 0; cursor.hover = false; cursor.skin = NULL; cursor.model = &other;
    R_DrawCursor(&cursor);
    T_ASSERT(cursor_drawn.model == &other); T_NULL(cursor_drawn.skin); T_STREQ(cursor_drawn.anim, "Normal");
    cursor_model = NULL; cursor_anim = NULL;
}

TEST(renderer_cursor, signal_locks_targetselect_and_restores_target) {
    mdxSequence_t seqs[] = {
        { .name = "Normal", .interval = {0, 200} },
        { .name = "Target", .interval = {200, 400} },
        { .name = "TargetSelect", .interval = {400, 600} },
        { .name = "Scroll Right", .interval = {600, 800} },
    };
    mdxModel_t mdx = { .sequences = seqs, .num_sequences = 4 };
    model_t model = { .mdx = &mdx, .modeltype = ID_MDLX };
    drawCursor_t cursor = { .model = &model, .game = true, .interaction = WC3_POINTER_SIGNALING,
        .tint = {17, 83, 149, 255}, .time = 100 };
    texture_t color = { .first_pixel = {17,83,149,255}, .has_first_pixel = true };
    cursor.skin = &color;
    cursor_active_model = NULL;
    T_ASSERT(R_DrawCursor(&cursor));
    T_STREQ(cursor_drawn.anim, "TargetSelect");
    T_EQ(cursor_tint.r, 17); T_EQ(cursor_tint.g, 83); T_EQ(cursor_tint.b, 149);
    cursor.scroll.x = 1; cursor.hover = true;
    R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "TargetSelect");
    cursor.interaction = WC3_POINTER_TARGETING; cursor.scroll.x = 0; cursor.hover = false;
    R_DrawCursor(&cursor);
    T_STREQ(cursor_drawn.anim, "Target");
    cursor_active_model = NULL; cursor_anim = NULL;
}

TEST(renderer_cursor, resolves_all_retail_modes) {
    struct { int interaction, hover, owned, hostile, x, y, mode; } const cases[] = {
        {0,0,0,0, 0,0, 0}, {0,1,0,0, 0,0, 1}, {0,1,0,1, 0,0, 2}, {0,1,1,0, 0,0, 3},
        {1,0,0,0, 0,0, 4}, {1,1,0,0, 0,0, 5}, {1,1,0,1, 0,0, 6}, {1,1,1,0, 0,0, 7},
        {3,1,1,1, 1,1, 8}, {2,1,1,1, 0,0, 9},
        {1,1,1,1, -1,0,10}, {1,1,1,1, 1,0,11}, {1,1,1,1, 0,1,12}, {1,1,1,1, 0,-1,13},
        {2,1,1,1, -1,1,14}, {2,1,1,1, 1,1,15}, {2,1,1,1, -1,-1,16}, {2,1,1,1, 1,-1,17},
    };
    cstring_t const names[] = {"Normal", "Select", "Select", "Select", "Target",
        "TargetSelect", "TargetSelect", "TargetSelect", "TargetSelect", "HoldItem",
        "Scroll Left", "Scroll Right", "Scroll Up", "Scroll Down", "Scroll Up Left",
        "Scroll Up Right", "Scroll Down Left", "Scroll Down Right"};
    mdxSequence_t seqs[18] = {0};
    FOR_LOOP(i, 18) { snprintf(seqs[i].name, sizeof(seqs[i].name), "%s", names[i]); seqs[i].interval[1] = 100; }
    mdxModel_t mdx = { .sequences = seqs, .num_sequences = 18 };
    model_t model = { .mdx = &mdx, .modeltype = ID_MDLX };
    texture_t color = { .first_pixel = {19,87,151,255}, .has_first_pixel = true };
    cursor_active_model = NULL;
    FOR_LOOP(i, 18) {
        drawCursor_t cursor = { .model = &model, .skin = &color, .interaction = cases[i].interaction,
            .hover = cases[i].hover, .owned = cases[i].owned, .hostile = cases[i].hostile,
            .scroll = {cases[i].x, cases[i].y}, .time = 100 + i * 7 };
        T_EQ(R_ResolveCursorMode(&cursor), cases[i].mode);
        T_ASSERT(R_DrawCursor(&cursor)); T_STREQ(cursor_drawn.anim, names[i]);
        color32_t expected = i == 8 ? color.first_pixel :
            (i == 1 || i == 5) ? (color32_t){255,255,0,255} :
            (i == 2 || i == 6) ? (color32_t){255,0,0,255} :
            (i == 3 || i == 7) ? (color32_t){0,255,0,255} : COLOR32_WHITE;
        T_ASSERT(!memcmp(&cursor_tint, &expected, sizeof(expected)));
        T_ASSERT(cursor_drawn.skin == (i == 9 ? &color : NULL));
    }
    cursor_active_model = NULL; cursor_anim = NULL;
}

TEST(renderer_terrain, water_style_reads_tileset_row_from_water_slk) {
    refImport_t saved_imports = ri;
    texture_t texture = {0};
    wc3WaterStyle_t const *style = R_WaterStyle();

    T_ASSERT(SFileOpenArchive("build/tests/tests.mpq", 0, 0, &test_renderer_archive));
    if (!test_renderer_archive) return;
    ri.FS_ReadFile = test_renderer_read; ri.FS_FreeFile = test_renderer_free;
    ri.LoadSlk = test_renderer_load_slk; ri.MemAlloc = test_renderer_alloc; ri.MemFree = test_renderer_free;
    R_TestSetTextureLoadResult(&texture);

    /* Stock TFT Outland row: the Abyss is an opaque black TeamColor surface 1.5 tiles down. */
    R_LoadWaterStyle('O');
    T_STREQ(R_TestLastTextureLoad(), "ReplaceableTextures\\TeamColor\\TeamColor00.blp");
    T_EQ(style->num_frames, 1); T_ASSERT(style->frames[0] == &texture);
    T_FEQ(style->height, -1.5f, 0.0001f); T_FEQ(style->frame_rate, 12.0f, 0.0001f);
    T_EQ(style->shallow_min.r, 0); T_EQ(style->shallow_min.a, 255);
    T_EQ(style->deep_max.b, 0); T_EQ(style->deep_max.a, 255);

    R_LoadWaterStyle('L'); /* Non-stock cells prove each band, channel and frame is read from its column. */
    T_STREQ(R_TestLastTextureLoad(), "TestUI\\Textures\\TestWater02.blp");
    T_EQ(style->num_frames, 3); T_FEQ(style->frame_rate, 15.0f, 0.0001f);
    T_FEQ(style->height, -0.7f, 0.0001f);
    T_EQ(style->shallow_min.r, 1); T_EQ(style->shallow_min.g, 2); T_EQ(style->shallow_min.b, 3);
    T_EQ(style->shallow_min.a, 4);
    T_EQ(style->shallow_max.r, 101); T_EQ(style->shallow_max.a, 104);
    T_EQ(style->deep_min.g, 202); T_EQ(style->deep_min.a, 204);
    T_EQ(style->deep_max.b, 253); T_EQ(style->deep_max.a, 254);

    R_LoadWaterStyle('X'); /* No frames: reported, and water is not drawn; the surface height still applies. */
    T_EQ(style->num_frames, 0); T_NULL(R_WaterFrame(style, 1000));
    T_FEQ(style->height, -0.7f, 0.0001f);
    R_LoadWaterStyle('?'); /* No row for the tileset. */
    T_EQ(style->num_frames, 0); T_EQ(style->deep_max.a, 0);

    R_TestSetTextureLoadResult(NULL);
    SFileCloseArchive(test_renderer_archive); test_renderer_archive = NULL;
    ri = saved_imports;
}

TEST(renderer_terrain, water_frames_advance_at_texrate_and_wrap) {
    texture_t frames[3] = {0};
    wc3WaterStyle_t style = { .frames = { &frames[0], &frames[1], &frames[2] }, .num_frames = 3, .frame_rate = 15.0f };

    T_ASSERT(R_WaterFrame(&style, 0) == &frames[0]);
    T_ASSERT(R_WaterFrame(&style, 66) == &frames[0]);   /* 0.99 frames */
    T_ASSERT(R_WaterFrame(&style, 67) == &frames[1]);   /* 1.005 frames */
    T_ASSERT(R_WaterFrame(&style, 134) == &frames[2]);
    T_ASSERT(R_WaterFrame(&style, 200) == &frames[0]);  /* 3 frames wrap to the first */
    style.num_frames = 1; /* Outland's single Abyss frame never changes. */
    T_ASSERT(R_WaterFrame(&style, 123456) == &frames[0]);
    style.frame_rate = 0; style.num_frames = 3;
    T_ASSERT(R_WaterFrame(&style, 123456) == &frames[0]);
}

TEST(renderer_terrain, tileset_archive_layers_between_map_imports_and_base_data) {
    refImport_t saved_imports = ri;
    assetCandidates_t candidates;
    PATHSTR candidate;
    PATHSTR resolved;
    void *buffer = NULL;
    cstring_t const cliff = "ReplaceableTextures\\Cliff\\Cliff1.blp";

    T_ASSERT(SFileOpenArchive("build/tests/tests.mpq", 0, 0, &test_renderer_archive));
    if (!test_renderer_archive) return;
    ri.FS_ReadFile = test_renderer_read; ri.FS_FreeFile = test_renderer_free;
    ri.MemAlloc = test_renderer_alloc; ri.MemFree = test_renderer_free;

    /* Retail resolves Outland's abyss cliffs from the nested O.mpq, not Lordaeron's base Cliff1. */
    R_W3OpenTilesetArchive('O');
    T_ASSERT(R_GameAssetCandidate(cliff, candidate, sizeof(candidate)));
    T_STREQ(candidate, "O.mpq\\ReplaceableTextures\\Cliff\\Cliff1.blp");
    T_ASSERT(test_renderer_read(candidate, &buffer) > 0); /* The FS resolves the layered path. */
    test_renderer_free(buffer);
    /* Destructable object data authors .tga, but MPQ cliff assets are BLP. */
    T_ASSERT(R_GameAssetCandidate("ReplaceableTextures\\Cliff\\Cliff1.tga", candidate, sizeof(candidate)));
    T_STREQ(candidate, "O.mpq\\ReplaceableTextures\\Cliff\\Cliff1.tga");
    T_ASSERT(R_ReadTextureFile(candidate, resolved, &buffer) > 0);
    test_renderer_free(buffer);
    T_ASSERT(!R_GameAssetCandidate("ReplaceableTextures\\Cliff\\Cliff0.blp", candidate, sizeof(candidate)));

    R_SetMapAssetScope("Maps\\Test.w3x"); /* Map imports outrank the tileset layer, which outranks base data. */
    R_AssetCandidates(cliff, &candidates);
    T_EQ(candidates.count, 3); T_ASSERT(candidates.scoped);
    T_STREQ(candidates.path[0], "Maps\\Test.w3x\\ReplaceableTextures\\Cliff\\Cliff1.blp");
    T_STREQ(candidates.path[1], "O.mpq\\ReplaceableTextures\\Cliff\\Cliff1.blp");
    T_STREQ(candidates.path[2], cliff);
    R_SetMapAssetScope(NULL);
    R_AssetCandidates("ReplaceableTextures\\Cliff\\Cliff0.blp", &candidates);
    T_EQ(candidates.count, 1); T_ASSERT(!candidates.scoped);

    R_W3OpenTilesetArchive('Q'); /* No such tileset archive: reported, and nothing is layered. */
    T_ASSERT(!R_GameAssetCandidate(cliff, candidate, sizeof(candidate)));

    SFileCloseArchive(test_renderer_archive); test_renderer_archive = NULL;
    ri = saved_imports;
}

TEST(renderer_terrain, cliff_types_store_absent_upper_tile_as_short_code) {
    /* CliffTypes.slk writes "_" for "no upper tile"; R_CliffTileIsSet relies on the parsed ID having a zero byte. */
    cstring_t slk =
        "ID;PWXL;N;E\n"
        "C;Y1;X1;K\"cliffID\"\nC;Y1;X2;K\"groundTile\"\nC;Y1;X3;K\"upperTile\"\n"
        "C;Y2;X1;K\"CLdi\"\nC;Y2;X2;K\"Ldrt\"\nC;Y2;X3;K\"_\"\n"
        "C;Y3;X1;K\"COrd\"\nC;Y3;X2;K\"Oaby\"\nC;Y3;X3;K\"Osmb\"\nE\n";
    w3CliffType_t *rows = NULL;
    uint32_t count = Stb_SlkLoadBuffer(slk, cliff_schema, (void **)&rows, sizeof(w3CliffType_t));
    T_EQ(count, 2);
    if (count == 2) {
        T_EQ(rows[0].upperTile >> 24, 0);
        T_EQ(rows[1].upperTile, MAKEFOURCC('O','s','m','b'));
        T_EQ(rows[1].groundTile, MAKEFOURCC('O','a','b','y'));
    }
    FS_SLKFreeRows(cliff_schema, rows, count, sizeof(w3CliffType_t));
}
