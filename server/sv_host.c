#include "server.h"

typedef enum {
    SV_HOST_UNICAST,
    SV_HOST_SOUND,
    SV_HOST_POSITIONED_SOUND,
    SV_HOST_SOUND_POLICY,
    SV_HOST_MINIMAP_PING,
    SV_HOST_MenuAction,
    SV_HOST_ResolveMap,
    SV_HOST_QueueMovie,
    SV_HOST_QueueModelCinematic,
    SV_HOST_LoadingFrame,
    SV_HOST_SetPaused,
    SV_HOST_ReadFile,
    SV_HOST_ReadFileAll,
    SV_HOST_SetPriorityArchive,
    SV_HOST_UserPath,
    SV_HOST_SavePath,
    SV_HOST_ListMaps,
    SV_HOST_ScriptSavePath,
    SV_HOST_ScriptCachePath,
    SV_HOST_ScriptCacheReadPath,
    SV_HOST_BindScriptSaveScope,
    SV_HOST_ScriptSaveExists,
    SV_HOST_ScriptRemoveSaveDirectory,
    SV_HOST_DeleteSave,
    SV_HOST_SaveLocalClientState,
    SV_HOST_ScriptCopySave,
    SV_HOST_ScriptRenameSaveDirectory,
    SV_HOST_ScriptScopeIdentity,
    SV_HOST_ListSaves,
} svHostOp_t;

typedef struct {
    svHostOp_t op;
    cstring_t a, b;
    string_t out;
    uint32_t count, *size;
    bool ok;
    handle_t ptr;
    void (*read)(handle_t, uint32_t, void *);
    void (*map)(cstring_t, void *);
    void *ud;
    edict_t *ent;
    vec3_t const *origin;
    soundPolicy_t const *policy;
    int channel, sound;
    float volume, attenuation, offset;
    vec2_t const *position;
    color32_t color;
    uint32_t flags;
} svHostCall_t;

static struct game_import sv_host;

/* Client state and mutable filesystem services are only entered by the owner at the barrier. */
static void SV_HostDispatch(void *data) {
    svHostCall_t *c = data;
    switch (c->op) {
    case SV_HOST_UNICAST: sv_host.unicast(c->ent); break;
    case SV_HOST_SOUND: sv_host.Sound(c->ent, c->channel, c->sound, c->volume, c->attenuation, c->offset); break;
    case SV_HOST_POSITIONED_SOUND: sv_host.PositionedSound(c->origin, c->ent, c->channel, c->sound, c->volume, c->attenuation, c->offset); break;
    case SV_HOST_SOUND_POLICY: sv_host.SoundPolicy(c->origin, c->ent, c->channel, c->sound, c->volume, c->attenuation, c->offset, c->policy); break;
    case SV_HOST_MINIMAP_PING: sv_host.MinimapPing(c->ent, c->position, c->offset, c->color, c->flags); break;
    case SV_HOST_MenuAction: sv_host.MenuAction(c->a, c->b); break;
    case SV_HOST_ResolveMap: c->ok = sv_host.ResolveMap(c->a, c->out, c->count); break;
    case SV_HOST_QueueMovie: sv_host.QueueMovie(c->a); break;
    case SV_HOST_QueueModelCinematic: sv_host.QueueModelCinematic(c->a); break;
    case SV_HOST_LoadingFrame: sv_host.LoadingFrame(); break;
    case SV_HOST_SetPaused: sv_host.SetPaused(c->ok); break;
    case SV_HOST_ReadFile: c->ptr = sv_host.ReadFile(c->a, c->size); break;
    case SV_HOST_ReadFileAll: sv_host.ReadFileAll(c->a, c->read, c->ud); break;
    case SV_HOST_SetPriorityArchive: sv_host.SetPriorityArchive(c->ptr); break;
    case SV_HOST_UserPath: sv_host.UserPath(c->a, c->out, c->count); break;
    case SV_HOST_SavePath: sv_host.SavePath(c->a, c->out, c->count); break;
    case SV_HOST_ListMaps: c->count = sv_host.ListMaps(c->map, c->ud); break;
    case SV_HOST_ScriptSavePath: c->ok = sv_host.ScriptSavePath(c->a, c->out, c->count); break;
    case SV_HOST_ScriptCachePath: c->ok = sv_host.ScriptCachePath(c->a, c->out, c->count); break;
    case SV_HOST_ScriptCacheReadPath: c->ok = sv_host.ScriptCacheReadPath(c->a, c->out, c->count); break;
    case SV_HOST_BindScriptSaveScope: c->ok = sv_host.BindScriptSaveScope(c->a); break;
    case SV_HOST_ScriptSaveExists: c->ok = sv_host.ScriptSaveExists(c->a); break;
    case SV_HOST_ScriptRemoveSaveDirectory: c->ok = sv_host.ScriptRemoveSaveDirectory(c->a); break;
    case SV_HOST_DeleteSave: c->ok = sv_host.DeleteSave(c->a); break;
    case SV_HOST_SaveLocalClientState: c->ok = sv_host.SaveLocalClientState(c->a); break;
    case SV_HOST_ScriptCopySave: c->ok = sv_host.ScriptCopySave(c->a, c->b); break;
    case SV_HOST_ScriptRenameSaveDirectory: c->ok = sv_host.ScriptRenameSaveDirectory(c->a, c->b); break;
    case SV_HOST_ScriptScopeIdentity: c->a = sv_host.ScriptScopeIdentity(); break;
    case SV_HOST_ListSaves: c->count = sv_host.ListSaves(c->out, c->count); break;
    }
}

/* Netchan/EOS ownership stays on the main thread, including immediate reliable sound sends. */
static void SV_HostUnicast(edict_t *ent) {
    svHostCall_t call = { .op = SV_HOST_UNICAST, .ent = ent };
    SV_CallOnMain(SV_HostDispatch, &call);
}

static void SV_HostMinimapPing(edict_t *ent, vec2_t const *position, float duration, color32_t color, uint32_t flags) {
    svHostCall_t call = { .op = SV_HOST_MINIMAP_PING, .ent = ent, .position = position,
        .offset = duration, .color = color, .flags = flags };
    SV_CallOnMain(SV_HostDispatch, &call);
}

static void SV_HostSound(edict_t *ent, int channel, int sound, float volume, float attenuation, float offset) {
    svHostCall_t call = { .op = SV_HOST_SOUND, .ent = ent, .channel = channel, .sound = sound,
        .volume = volume, .attenuation = attenuation, .offset = offset };
    SV_CallOnMain(SV_HostDispatch, &call);
}

static void SV_HostPositionedSound(vec3_t const *origin, edict_t *ent, int channel, int sound, float volume, float attenuation, float offset) {
    svHostCall_t call = { .op = SV_HOST_POSITIONED_SOUND, .origin = origin, .ent = ent, .channel = channel,
        .sound = sound, .volume = volume, .attenuation = attenuation, .offset = offset };
    SV_CallOnMain(SV_HostDispatch, &call);
}

static void SV_HostSoundPolicy(vec3_t const *origin, edict_t *ent, int channel, int sound, float volume, float attenuation, float offset, soundPolicy_t const *policy) {
    svHostCall_t call = { .op = SV_HOST_SOUND_POLICY, .origin = origin, .ent = ent, .channel = channel,
        .sound = sound, .volume = volume, .attenuation = attenuation, .offset = offset, .policy = policy };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostMenuAction(cstring_t action, cstring_t arg) {
    svHostCall_t call = { .op = SV_HOST_MenuAction, .a = action, .b = arg };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostResolveMap(cstring_t arg, string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_ResolveMap, .a = arg, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostQueueMovie(cstring_t path) {
    svHostCall_t call = { .op = SV_HOST_QueueMovie, .a = path };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostQueueModelCinematic(cstring_t path) {
    svHostCall_t call = { .op = SV_HOST_QueueModelCinematic, .a = path };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostLoadingFrame(void) {
    svHostCall_t call = { .op = SV_HOST_LoadingFrame };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostSetPaused(bool paused) {
    svHostCall_t call = { .op = SV_HOST_SetPaused, .ok = paused };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static handle_t SV_HostReadFile(cstring_t filename, uint32_t *size) {
    svHostCall_t call = { .op = SV_HOST_ReadFile, .a = filename, .size = size };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ptr;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostReadFileAll(cstring_t filename, void (*callback)(handle_t, uint32_t, void *), void *ud) {
    svHostCall_t call = { .op = SV_HOST_ReadFileAll, .a = filename, .read = callback, .ud = ud };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostSetPriorityArchive(handle_t archive) {
    svHostCall_t call = { .op = SV_HOST_SetPriorityArchive, .ptr = archive };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostUserPath(cstring_t rel, string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_UserPath, .a = rel, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static void SV_HostSavePath(cstring_t rel, string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_SavePath, .a = rel, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static uint32_t SV_HostListMaps(void (*callback)(cstring_t, void *), void *ud) {
    svHostCall_t call = { .op = SV_HOST_ListMaps, .map = callback, .ud = ud };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.count;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptSavePath(cstring_t name, string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_ScriptSavePath, .a = name, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptCachePath(cstring_t name, string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_ScriptCachePath, .a = name, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptCacheReadPath(cstring_t name, string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_ScriptCacheReadPath, .a = name, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostBindScriptSaveScope(cstring_t name) {
    svHostCall_t call = { .op = SV_HOST_BindScriptSaveScope, .a = name };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptSaveExists(cstring_t name) {
    svHostCall_t call = { .op = SV_HOST_ScriptSaveExists, .a = name };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptRemoveSaveDirectory(cstring_t name) {
    svHostCall_t call = { .op = SV_HOST_ScriptRemoveSaveDirectory, .a = name };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostDeleteSave(cstring_t name) {
    svHostCall_t call = { .op = SV_HOST_DeleteSave, .a = name };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostSaveLocalClientState(cstring_t name) {
    svHostCall_t call = { .op = SV_HOST_SaveLocalClientState, .a = name };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptCopySave(cstring_t source, cstring_t target) {
    svHostCall_t call = { .op = SV_HOST_ScriptCopySave, .a = source, .b = target };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static bool SV_HostScriptRenameSaveDirectory(cstring_t source, cstring_t target) {
    svHostCall_t call = { .op = SV_HOST_ScriptRenameSaveDirectory, .a = source, .b = target };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.ok;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static cstring_t SV_HostScriptScopeIdentity(void) {
    svHostCall_t call = { .op = SV_HOST_ScriptScopeIdentity };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.a;
}

/* Keep the existing typed import contract across the worker/owner handoff. */
static uint32_t SV_HostListSaves(string_t out, uint32_t out_size) {
    svHostCall_t call = { .op = SV_HOST_ListSaves, .out = out, .count = out_size };
    SV_CallOnMain(SV_HostDispatch, &call);
    return call.count;
}

/* Capture once when the game API is initialized; no game-specific rules cross this boundary. */
void SV_BindHostImports(struct game_import *imports) {
    sv_host = *imports;
    imports->unicast = SV_HostUnicast;
    imports->MinimapPing = SV_HostMinimapPing;
    imports->Sound = SV_HostSound;
    imports->PositionedSound = SV_HostPositionedSound;
    imports->SoundPolicy = SV_HostSoundPolicy;
    imports->MenuAction = SV_HostMenuAction;
    imports->ResolveMap = SV_HostResolveMap;
    imports->QueueMovie = SV_HostQueueMovie;
    imports->QueueModelCinematic = SV_HostQueueModelCinematic;
    imports->LoadingFrame = SV_HostLoadingFrame;
    imports->SetPaused = SV_HostSetPaused;
    imports->ReadFile = SV_HostReadFile;
    imports->ReadFileAll = SV_HostReadFileAll;
    imports->SetPriorityArchive = SV_HostSetPriorityArchive;
    imports->UserPath = SV_HostUserPath;
    imports->SavePath = SV_HostSavePath;
    imports->ListMaps = SV_HostListMaps;
    imports->ScriptSavePath = SV_HostScriptSavePath;
    imports->ScriptCachePath = SV_HostScriptCachePath;
    imports->ScriptCacheReadPath = SV_HostScriptCacheReadPath;
    imports->BindScriptSaveScope = SV_HostBindScriptSaveScope;
    imports->ScriptSaveExists = SV_HostScriptSaveExists;
    imports->ScriptRemoveSaveDirectory = SV_HostScriptRemoveSaveDirectory;
    imports->DeleteSave = SV_HostDeleteSave;
    imports->SaveLocalClientState = imports->SaveLocalClientState ? SV_HostSaveLocalClientState : NULL;
    imports->ScriptCopySave = SV_HostScriptCopySave;
    imports->ScriptRenameSaveDirectory = SV_HostScriptRenameSaveDirectory;
    imports->ScriptScopeIdentity = SV_HostScriptScopeIdentity;
    imports->ListSaves = SV_HostListSaves;
}
