#include "test.h"
#include "client/client.h"
#include "client/cl_input_local.h"
#include <limits.h>

struct client_state cl;
refExport_t re;
mouseEvent_t mouse;
static int entity_debug, entity_debug_reads, entity_debug_lines;

static int entity_integer(cstring_t name, int fallback) {
    if (strcmp(name, "cl_debug_entities")) return fallback;
    entity_debug_reads++;
    return entity_debug;
}
static int entity_log(FILE *stream, cstring_t format, ...) {
    (void)stream;
    if (!strncmp(format, "CL_BUILD paused-render", 22)) entity_debug_lines++;
    return 0;
}

/* Count the configuration boundary; exercise the real entity conversion and diagnostic gate. */
#define Cvar_Integer entity_integer
#define fprintf entity_log
#include "client/cl_view.c"
#undef fprintf
#undef Cvar_Integer

void CL_ApplyIndicator(renderEntity_t *ent) { (void)ent; }

TEST(client_view, ordinary_entities_do_not_query_paused_emission_diagnostics) {
    model_t model = { 0 };
    cl.models[1] = &model;
    cl.viewDef.lerpfrac = 0.5f;
    entity_debug_reads = entity_debug_lines = 0;
    FOR_LOOP(enabled, 2) {
        entity_debug = enabled;
        V_ClearScene();
        FOR_LOOP(i, 200) {
            centity_t cent = {
                .prev = { .origin = { 10, 20, 30 }, .scale = 1 },
                .current = { .number = i + 1, .model = 1, .frame = 500,
                    .origin = { 20, 30, 40 }, .scale = 1,
                    .flags = i & 1 ? EF_UNIT : EF_BUILDING },
            };
            V_AddClientEntity(&cent);
            T_EQ(view_state.entities[i].model, &model);
            T_EQ(view_state.entities[i].number, i + 1);
            T_FEQ(view_state.entities[i].origin.x, 15, 0.0001f);
            T_EQ(view_state.entities[i].frame, 500);
            T_EQ(view_state.entities[i].flags & RF_MDX_EMISSION_PAUSED, 0);
        }
        T_EQ(view_state.num_entities, 200);
        T_EQ(entity_debug_reads, 0);
        T_EQ(entity_debug_lines, 0);
    }
    cl.models[1] = NULL;
}

TEST(client_view, paused_emission_diagnostics_preserve_toggle_throttle_and_time_rewind) {
    centity_t cent = { .current = { .number = 7, .flags = EF_MDX_EMISSION_PAUSED } };
    debug_paused_building_time[7] = 0;
    entity_debug_reads = entity_debug_lines = entity_debug = 0;
    cl.time = 100; V_ClearScene(); V_AddClientEntity(&cent);
    T_EQ(entity_debug_reads, 1); T_EQ(entity_debug_lines, 0);
    T_ASSERT(view_state.entities[0].flags & RF_MDX_EMISSION_PAUSED);
    entity_debug = 1; V_ClearScene(); V_AddClientEntity(&cent);
    T_EQ(entity_debug_lines, 1); T_EQ(debug_paused_building_time[7], 100);
    cl.time = 1099; V_ClearScene(); V_AddClientEntity(&cent);
    T_EQ(entity_debug_lines, 1); T_EQ(entity_debug_reads, 2);
    cl.time = 1100; V_ClearScene(); V_AddClientEntity(&cent);
    T_EQ(entity_debug_lines, 2);
    cl.time = 20; V_ClearScene(); V_AddClientEntity(&cent);
    T_EQ(entity_debug_lines, 3); T_EQ(debug_paused_building_time[7], 20);
    cent.current.number = MAX_CLIENT_ENTITIES;
    entity_debug_reads = 0; V_ClearScene(); V_AddClientEntity(&cent);
    T_EQ(entity_debug_reads, 0); T_EQ(entity_debug_lines, 3);
}
