#include "server.h"
#include "common/server_api.h"
#include <SDL2/SDL.h>

#ifndef __EMSCRIPTEN__
typedef enum { SV_THREAD_IDLE, SV_THREAD_RUN, SV_THREAD_DONE, SV_THREAD_STOP } svThreadState_t;

static struct {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    svThreadState_t state;
    void (*call)(void *);
    void *data;
    Uint64 wall, host;
    uint32_t calls;
} sv_thread;
static _Thread_local bool sv_in_worker;
static struct { Uint64 start, wall, host, barrier; uint32_t jobs, calls; } sv_work_profile;

/* Report background time separately from the owner's blocking time; overlapping columns must not be summed. */
static void SV_ProfileThread(Uint64 barrier) {
    if (!Cvar_Integer("com_profile", 0)) { memset(&sv_work_profile, 0, sizeof(sv_work_profile)); return; }
    Uint64 const now = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    if (!sv_work_profile.start) sv_work_profile.start = now;
    sv_work_profile.jobs++; sv_work_profile.calls += sv_thread.calls;
    sv_work_profile.wall += sv_thread.wall; sv_work_profile.host += sv_thread.host;
    sv_work_profile.barrier += barrier;
    if (now - sv_work_profile.start < frequency) return;
    double const ms = 1000.0 / frequency;
    fprintf(stderr, "[SV_THREAD] jobs=%u elapsed_ms=%.3f wall_ms=%.3f host_wait_ms=%.3f barrier_ms=%.3f host_calls=%u\n",
        sv_work_profile.jobs, (now - sv_work_profile.start) * ms, sv_work_profile.wall * ms,
        sv_work_profile.host * ms, sv_work_profile.barrier * ms, sv_work_profile.calls);
    memset(&sv_work_profile, 0, sizeof(sv_work_profile));
}

/* Only game simulation runs here; transport and client resources stay on the owner. */
static int SV_GameThread(void *unused) {
    (void)unused; sv_in_worker = true;
    SDL_LockMutex(sv_thread.mutex);
    for (;;) {
        while (sv_thread.state == SV_THREAD_IDLE || sv_thread.state == SV_THREAD_DONE)
            SDL_CondWait(sv_thread.cond, sv_thread.mutex);
        if (sv_thread.state == SV_THREAD_STOP) break;
        Uint64 const start = SDL_GetPerformanceCounter();
        SDL_UnlockMutex(sv_thread.mutex);
        SV_RunGameFrame();
        SDL_LockMutex(sv_thread.mutex);
        sv_thread.wall = SDL_GetPerformanceCounter() - start;
        sv_thread.state = SV_THREAD_DONE;
        SDL_CondBroadcast(sv_thread.cond);
    }
    SDL_UnlockMutex(sv_thread.mutex);
    return 0;
}

/* A single outstanding tick bounds memory and leaves lifecycle changes at the frame barrier. */
void SV_StartGameFrame(void) {
    if (!sv_thread.thread) {
        sv_thread.mutex = SDL_CreateMutex(); sv_thread.cond = SDL_CreateCond();
        if (!sv_thread.mutex || !sv_thread.cond) {
            fprintf(stderr, "Server: cannot create simulation synchronization: %s\n", SDL_GetError()); abort();
        }
        sv_thread.thread = SDL_CreateThread(SV_GameThread, "server-sim", NULL);
        if (!sv_thread.thread) {
            fprintf(stderr, "Server: cannot create simulation thread: %s\n", SDL_GetError()); abort();
        }
    }
    SDL_LockMutex(sv_thread.mutex);
    assert(sv_thread.state == SV_THREAD_IDLE);
    sv_thread.wall = sv_thread.host = 0; sv_thread.calls = 0;
    sv_thread.state = SV_THREAD_RUN;
    SDL_CondBroadcast(sv_thread.cond);
    SDL_UnlockMutex(sv_thread.mutex);
}

/* Borrowed arguments remain live on the blocked worker stack until the owner returns the result. */
void SV_CallOnMain(void (*call)(void *), void *data) {
    if (!sv_in_worker) { call(data); return; }
    Uint64 const start = SDL_GetPerformanceCounter();
    SDL_LockMutex(sv_thread.mutex);
    assert(!sv_thread.call);
    sv_thread.call = call; sv_thread.data = data;
    SDL_CondBroadcast(sv_thread.cond);
    while (sv_thread.call) SDL_CondWait(sv_thread.cond, sv_thread.mutex);
    sv_thread.host += SDL_GetPerformanceCounter() - start; sv_thread.calls++;
    SDL_UnlockMutex(sv_thread.mutex);
}

/* Pump client/host requests after drawing, so waiting on a tick cannot deadlock its imports. */
void SV_JoinGameFrame(void) {
    if (!sv_thread.thread) return;
    Uint64 const start = SDL_GetPerformanceCounter();
    SDL_LockMutex(sv_thread.mutex);
    if (sv_thread.state == SV_THREAD_IDLE) { SDL_UnlockMutex(sv_thread.mutex); return; }
    while (sv_thread.state == SV_THREAD_RUN) {
        if (!sv_thread.call) { SDL_CondWait(sv_thread.cond, sv_thread.mutex); continue; }
        void (*call)(void *) = sv_thread.call;
        void *data = sv_thread.data;
        SDL_UnlockMutex(sv_thread.mutex);
        call(data);
        SDL_LockMutex(sv_thread.mutex);
        sv_thread.call = NULL; sv_thread.data = NULL;
        SDL_CondBroadcast(sv_thread.cond);
    }
    sv_thread.state = SV_THREAD_IDLE;
    SDL_UnlockMutex(sv_thread.mutex);
    SV_ProfileThread(SDL_GetPerformanceCounter() - start);
}

/* Join before game teardown or SDL shutdown; a later map may start a fresh worker. */
void SV_StopThread(void) {
    if (!sv_thread.thread) return;
    SV_JoinGameFrame();
    SDL_LockMutex(sv_thread.mutex);
    sv_thread.state = SV_THREAD_STOP;
    SDL_CondBroadcast(sv_thread.cond);
    SDL_UnlockMutex(sv_thread.mutex);
    SDL_WaitThread(sv_thread.thread, NULL);
    SDL_DestroyCond(sv_thread.cond); SDL_DestroyMutex(sv_thread.mutex);
    memset(&sv_thread, 0, sizeof(sv_thread));
}
#else
/* The browser build has no pthread runtime; its event loop remains synchronous. */
void SV_StartGameFrame(void) { SV_RunGameFrame(); }
void SV_JoinGameFrame(void) {}
void SV_StopThread(void) {}
void SV_CallOnMain(void (*call)(void *), void *data) { call(data); }
#endif
