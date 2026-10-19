#include "test.h"
#include "renderer/r_local.h"
#include "renderer/r_shader.h"
#include "common/online.h"
#include <stdarg.h>
extern SDL_Window *window;

static struct {
    Uint64 clock;
    uint32_t swaps, delays, slept, sets, ends, stages, shader_resets;
    int vsync, applied, maxfps;
    bool shader_cache, cache_applied;
    renderPass_t passes[8];
} frame_test;

static cstring_t frame_cvar(cstring_t name, cstring_t fallback) {
    if (!strcmp(name, "r_vsync")) return frame_test.vsync ? "1" : "0";
    if (!strcmp(name, "r_program_cache")) return frame_test.shader_cache ? "1" : "0";
    return fallback;
}
static int frame_integer(cstring_t name, int fallback) {
    return !strcmp(name, "com_maxfps") ? frame_test.maxfps : fallback;
}
static Uint32 frame_ticks(void) { return (Uint32)(frame_test.clock / 1000); }
static Uint64 frame_counter(void) { frame_test.clock += 100; return frame_test.clock; }
static void frame_delay(Uint32 ms) {
    frame_test.delays++; frame_test.slept += ms; frame_test.clock += ms * 1000;
}
static void frame_swap(SDL_Window *target) { T_EQ(target, window); frame_test.swaps++; }
static int frame_set_interval(int interval) {
    frame_test.sets++; frame_test.applied = interval; return 0;
}
static int frame_get_interval(void) { return frame_test.applied; }
static void frame_reset_shader(bool cache) {
    frame_test.shader_resets++; frame_test.cache_applied = cache; R_ResetShaderBinding(cache);
}
static void frame_cap(GLenum value) { (void)value; }
static void frame_depth(GLboolean value) { (void)value; }
static void frame_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    (void)r; (void)g; (void)b; (void)a;
}
static uint32_t frame_profile_reports;
static char frame_profile_output[512];
static Uint64 frame_frequency(void) { return 1000000; }
static int frame_profile_printf(FILE *stream, cstring_t format, ...) {
    (void)stream; va_list args; va_start(args, format);
    int size = vsnprintf(frame_profile_output, sizeof(frame_profile_output), format, args);
    va_end(args); frame_profile_reports++; return size;
}

static char frame_order[16];
static uint32_t frame_order_count;
static bool frame_polled, frame_prepared;
static void frame_mark(char mark) { frame_order[frame_order_count++] = mark; }
static void frame_online(uint32_t msec) { (void)msec; frame_polled = frame_prepared = false; frame_mark('O'); }
static void frame_update(uint32_t msec) { (void)msec; T_ASSERT(frame_polled); frame_mark('U'); }
static void frame_draw(uint32_t msec) { (void)msec; frame_mark('R'); }
static void frame_server_sync(uint32_t msec) { (void)msec; frame_polled = true; frame_mark('Y'); }
static void frame_server_prepare(uint32_t msec) {
    (void)msec; frame_polled = frame_prepared = true; frame_mark('P');
}
static void frame_server_start(bool threaded) {
    if (frame_prepared) frame_mark(threaded ? 'S' : 'Y');
    frame_prepared = false;
}
static void frame_server_end(void) { frame_mark('J'); }
static bool frame_needs_server(void) { return true; }
static bool frame_movie_active(void) { return false; }
static void frame_quit(void) { T_ASSERT(false); }

/* Exercise real presentation/pacing with controlled configuration and captured clock, OS and profiler calls. */
#define SDL_Delay frame_delay
#define SDL_GL_SwapWindow frame_swap
#define SDL_GL_SetSwapInterval frame_set_interval
#define SDL_GL_GetSwapInterval frame_get_interval
#define SDL_GetTicks frame_ticks
#define SDL_GetPerformanceCounter frame_counter
#define R_ResetShaderBinding frame_reset_shader
#define glEnable frame_cap
#define glDisable frame_cap
#define glDepthMask frame_depth
#define glDepthFunc frame_cap
#define glCullFace frame_cap
#define glColorMask frame_mask
#define glClear frame_cap
#include "renderer/r_main.c"
#undef glClear
#undef glColorMask
#undef glCullFace
#undef glDepthFunc
#undef glDepthMask
#undef glDisable
#undef glEnable
#undef R_ResetShaderBinding
#define Cvar_Integer frame_integer
#undef main
#define main FrameTestEngineMain
#define SDL_GetPerformanceFrequency frame_frequency
#define fprintf frame_profile_printf
#define Online_Frame frame_online
#define CL_UpdateFrame frame_update
#define SCR_UpdateScreen frame_draw
#define SV_PrepareFrame frame_server_prepare
#define SV_StartPreparedFrame frame_server_start
#define SV_Frame frame_server_sync
#define SV_EndFrame frame_server_end
#define SV_NeedsFrame frame_needs_server
#define CL_MovieActive frame_movie_active
#define Com_Quit frame_quit
#include "common/main.c"
#undef Com_Quit
#undef CL_MovieActive
#undef SV_NeedsFrame
#undef SV_EndFrame
#undef SV_Frame
#undef SV_PrepareFrame
#undef SV_StartPreparedFrame
#undef SCR_UpdateScreen
#undef CL_UpdateFrame
#undef Online_Frame
#undef fprintf
#undef SDL_GetPerformanceFrequency
#undef main
#undef Cvar_Integer
#undef SDL_GetPerformanceCounter
#undef SDL_GetTicks
#undef SDL_GL_GetSwapInterval
#undef SDL_GL_SetSwapInterval
#undef SDL_GL_SwapWindow
#undef SDL_Delay

struct client_static cls;
void Cbuf_AddText(cstring_t text) { (void)text; T_ASSERT(false); }
void Cbuf_Execute(void) { T_ASSERT(false); }

TEST(renderer_frame, engine_scheduler_polls_transport_before_client_then_overlaps_tick_with_draw) {
    memset(frame_order, 0, sizeof(frame_order)); frame_order_count = 0;
    cls.state = ca_active;
    openrealm_frame_state_t state = { .previous_time = frame_ticks() };
    OpenRealm_Frame(&state);
    T_STREQ(frame_order, "OPUSRJ"); T_EQ(state.frame_count, 1);
    memset(frame_order, 0, sizeof(frame_order)); frame_order_count = 0;
    cls.state = ca_connected;
    OpenRealm_Frame(&state);
    T_STREQ(frame_order, "OYURJ"); T_EQ(state.frame_count, 2);
}

void R_ProfileStage(renderPass_t pass) {
    if (frame_test.stages < sizeof(frame_test.passes) / sizeof(*frame_test.passes))
        frame_test.passes[frame_test.stages] = pass;
    frame_test.stages++;
}
void R_ProfileEndFrame(void) { frame_test.ends++; }
void R_ProfileBeginFrame(void) {}

/* The real frame entry owns external-state invalidation and samples the live diagnostic toggle once. */
TEST(renderer_frame, begin_frame_resets_shader_binding_and_honors_live_cache_mode) {
    memset(&frame_test, 0, sizeof(frame_test)); ri.CvarString = frame_cvar; drawable_dirty = false;
    FOR_LOOP(i, 2) {
        frame_test.shader_cache = !i;
        R_BeginFrame(); T_EQ(frame_test.shader_resets, i + 1);
        T_EQ(frame_test.cache_applied, !i);
        shaderBindStats_t stats = R_GetShaderBindStats(); T_EQ(stats.requests, 0); T_EQ(stats.binds, 0);
    }
}

/* Presentation must honor live VSync changes while leaving frame pacing to the main loop. */
TEST(renderer_frame, end_frame_presents_without_sleep_and_keeps_vsync_changes) {
    FOR_LOOP(i, 2) {
        memset(&frame_test, 0, sizeof(frame_test));
        frame_test.vsync = i; frame_test.clock = 100000;
        ri.CvarString = frame_cvar; r_swapinterval = -999;
        R_EndFrame(); R_EndFrame();
        T_EQ(frame_test.swaps, 2); T_EQ(frame_test.delays, 0); T_EQ(frame_test.ends, 2);
        T_EQ(frame_test.sets, 1); T_EQ(frame_test.applied, i);
        T_EQ(frame_test.stages, 2); T_EQ(frame_test.passes[0], RP_SWAP);
        frame_test.vsync = !i;
        R_EndFrame();
        T_EQ(frame_test.swaps, 3); T_EQ(frame_test.sets, 2); T_EQ(frame_test.applied, !i);
        T_EQ(frame_test.delays, 0);
    }
}

TEST(renderer_frame, main_loop_limiter_waits_only_for_remaining_capped_budget) {
    int const rates[] = {30, 60, 120};
    FOR_LOOP(i, 3) {
        memset(&frame_test, 0, sizeof(frame_test));
        frame_test.maxfps = rates[i]; frame_test.clock = 5000;
        Com_LimitFrameRate(0, 1000000, false);
        T_ASSERT(frame_test.delays > 0); T_ASSERT(frame_test.slept > 0);
        T_ASSERT(frame_test.clock >= 1000000 / rates[i]);
        T_ASSERT(frame_test.clock < 1000000 / rates[i] + 300);
    }
    FOR_LOOP(i, 4) {
        memset(&frame_test, 0, sizeof(frame_test));
        frame_test.maxfps = i == 0 ? 0 : i == 1 ? -1 : 30;
        frame_test.clock = i == 3 ? 40000 : 5000;
        Com_LimitFrameRate(0, 1000000, i == 2);
        T_EQ(frame_test.delays, 0);
    }
}

TEST(renderer_frame, cpu_report_counts_complete_stage_windows_and_resets_when_disabled) {
    frame_profile_reports = 0; frame_profile_output[0] = 0;
    Com_ProfileFrame(false, 0, 0, 0, 0);
    Com_ProfileFrame(true, 10000, 20000, 50000, 100000);
    T_EQ(frame_profile_reports, 0);
    Com_ProfileFrame(true, 1010000, 1020000, 1060000, 1110000);
    T_EQ(frame_profile_reports, 1);
    T_ASSERT(strstr(frame_profile_output, "loops=2"));
    T_ASSERT(strstr(frame_profile_output, "elapsed_ms=1100.000"));
    T_ASSERT(strstr(frame_profile_output, "online_ms=20.000"));
    T_ASSERT(strstr(frame_profile_output, "server_ms=70.000"));
    T_ASSERT(strstr(frame_profile_output, "server_max_ms=40.000"));
    T_ASSERT(strstr(frame_profile_output, "client_ms=100.000"));
    Com_ProfileFrame(false, 0, 0, 0, 0);
    Com_ProfileFrame(true, 2000000, 2010000, 2020000, 2030000);
    T_EQ(frame_profile_reports, 1);
    Com_ProfileFrame(false, 0, 0, 0, 0);
}
