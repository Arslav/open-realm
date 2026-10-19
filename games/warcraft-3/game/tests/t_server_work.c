#ifdef BZ_TESTS
#include "test.h"
#include "../g_local.h"
#include "../skills/s_skills.h"

edict_t *alloc_test_unit(uint32_t class_id, float x, float y);
void reset_entities(void);
void setup_test_world(void);
bool run_test_jass(cstring_t src);
slkTestData_t *parse_slk_string(char const *text);
void free_slk_rows(slkTestData_t *rows);
void S_TestResetHeroAuraAliasResolves(void);
uint32_t S_TestHeroAuraAliasResolves(void);
extern uint32_t G_TestSelectionScans, G_TestTintVisibilityChecks;

static cstring_t (*scan_cvar_orig)(cstring_t, cstring_t);
static cstring_t scan_mode;
static cstring_t const scan_options[] = { "wc3_selection_mask", "wc3_tint_fast", "wc3_aura_candidates" };
static uint32_t const footman_id = MAKEFOURCC('h', 'f', 'o', 'o');
static uint32_t const ghost_id = MAKEFOURCC('A', 'g', 'h', 'o');
static uint32_t const aura_id = MAKEFOURCC('X', 'U', 'a', 'u');
static uint32_t const paladin_id = MAKEFOURCC('H', 'p', 'a', 'l');
static UnitAbilities_t const scan_empty_abilities;
static char const scan_aura_slk[] =
    "ID;PWXL;N;EBB;Y2;X9\n"
    "C;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\nC;Y1;X3;K\"targs\"\n"
    "C;Y1;X4;K\"Area1\"\nC;Y1;X5;K\"DataA1\"\nC;Y1;X6;K\"DataB1\"\n"
    "C;Y1;X7;K\"DataC1\"\nC;Y1;X8;K\"BuffID1\"\nC;Y1;X9;K\"levels\"\n"
    "C;Y2;X1;K\"XUau\"\nC;Y2;X2;K\"AUau\"\nC;Y2;X3;K\"ground,friend,organic\"\n"
    "C;Y2;X4;K\"500\"\nC;Y2;X5;K\"0.2\"\nC;Y2;X6;K\"0.017\"\n"
    "C;Y2;X7;K\"1\"\nC;Y2;X8;K\"Biml\"\nC;Y2;X9;K\"1\"\nE\n";

/* Override only experiment switches; exercise the same registered imports as a running server. */
static cstring_t scan_cvar(cstring_t name, cstring_t fallback) {
    FOR_LOOP(i, sizeof(scan_options) / sizeof(*scan_options))
        if (!strcmp(name, scan_options[i])) return scan_mode;
    return scan_cvar_orig(name, fallback);
}

/* The engine test has no transport peers; UI serialization still runs through the game scheduler. */
static void scan_write(pfWriteType_t type, void const *data) { (void)type; (void)data; }
static void scan_unicast(edict_t *client) { (void)client; }

/* Tests compare recipients by player number, including non-local and disconnected slots. */
static edict_t *scan_unit(uint32_t player, float x) {
    edict_t *unit = alloc_test_unit(footman_id, x, 0);
    unit->s.player = player;
    unit->svflags |= SVF_MONSTER;
    unit->data.UnitAbilities = &scan_empty_abilities;
    return unit;
}

TEST(wc3_server_work, selection_mask_preserves_remote_and_disconnected_selection_cleanup) {
    void (*old_write)(pfWriteType_t, void const *) = gi.Write;
    void (*old_unicast)(edict_t *) = gi.unicast;
    scan_cvar_orig = gi.CvarString;
    gi.CvarString = scan_cvar;
    gi.Write = scan_write; gi.unicast = scan_unicast;
    FOR_LOOP(mode, 2) {
        edict_t *first, *second, *third, *dead;
        reset_entities(); setup_test_world();
        scan_mode = mode ? "1" : "0";
        first = scan_unit(2, 0); second = scan_unit(5, 128); dead = scan_unit(9, 256);
        third = scan_unit(9, 384);
        FOR_LOOP(i, 64) scan_unit(PLAYER_NEUTRAL_PASSIVE, (float)i * 4);
        game.clients[2].connected = game.clients[5].connected = true;
        game.clients[9].connected = false;
        first->selected = 1u << 2; second->selected = 1u << 5;
        third->selected = 1u << 9;
        dead->selected = (1u << 2) | (1u << 5) | (1u << 9);
        dead->health.value = 0; dead->svflags |= SVF_DEADMONSTER;
        T_ASSERT(run_test_jass("function main takes nothing returns nothing\nendfunction\n"));
        level.started = level.scriptsConfigured = level.scriptsStarted = true;
        level.time = 1000;
        G_TestSelectionScans = 0; globals.RunFrame();
        T_EQ(first->selected, 1u << 2); T_EQ(second->selected, 1u << 5); T_EQ(dead->selected, 0);
        T_EQ(third->selected, 1u << 9);
        T_EQ(G_TestSelectionScans, mode ? 3 : game.max_clients);
        first->selected = second->selected = third->selected = 0;
        game.clients[2].selection_dirty = true;
        level.time += FRAMETIME; G_TestSelectionScans = 0; globals.RunFrame();
        T_EQ(G_TestSelectionScans, mode ? 0 : game.max_clients);
        T_ASSERT(!game.clients[2].selection_dirty);
    }
    gi.CvarString = scan_cvar_orig;
    gi.Write = old_write; gi.unicast = old_unicast;
}

TEST(wc3_server_work, tint_fast_keeps_recipient_specific_wire_bytes) {
    uint8_t before[2048], after[2048];
    edict_t *ghost, *tinted, *hero;
    abilityitem_t item = S_AbilityItem(ghost_id);
    abilityCall_t call = MAKE(abilityCall_t, .item = &item);
    reset_entities(); setup_test_world(); G_BlightShutdown();
    FOR_LOOP(i, 64) scan_unit(PLAYER_NEUTRAL_PASSIVE, (float)i * 4);
    ghost = scan_unit(2, 32); tinted = scan_unit(2, 64);
    tinted->vertex_color_set = true; tinted->vertex_color = MAKE(color32_t, 17, 34, 51, 0);
    T_ASSERT(S_AbilityMessage(ghost, A_UNIT_INIT, &call));
    T_ASSERT(S_UnitHasInvisibilityState(ghost));
    hero = alloc_test_unit(paladin_id, 128, 0);
    hero->s.player = 2; hero->health.value = 0;
    hero->svflags |= SVF_MONSTER | SVF_DEADMONSTER; hero->think = monster_think;
    game.constants.dissipateTime = 1; level.time = 1000; unit_begin_decay(hero);
    FOR_LOOP(i, 5) { level.time += FRAMETIME; G_RunEntities(); }
    T_FEQ(G_UnitDissipatePresentationAlpha(hero), 0.5f, 0.001f);
    level.alliances[5][2] |= 1u << ALLIANCE_SHARED_VISION;
    G_FowInit();
    game.clients[2].ps.rdflags |= RDF_NOFOG;
    game.clients[5].ps.rdflags |= RDF_NOFOG;
    game.clients[6].ps.rdflags |= RDF_NOFOG;
    scan_cvar_orig = gi.CvarString; gi.CvarString = scan_cvar;
    FOR_LOOP(n, 3) {
        uint32_t const viewer = n == 0 ? 2 : n == 1 ? 5 : 6;
        uint32_t a, b, slow;
        edict_t *client = &g_edicts[viewer];
        scan_mode = "0"; G_TestTintVisibilityChecks = 0;
        a = G_WriteClientDatagram(client, before, sizeof(before)); slow = G_TestTintVisibilityChecks;
        scan_mode = "1"; G_TestTintVisibilityChecks = 0;
        b = G_WriteClientDatagram(client, after, sizeof(after));
        T_ASSERT(a > 0); T_EQ(a, b); T_EQ(memcmp(before, after, a), 0);
        T_ASSERT(G_TestTintVisibilityChecks < slow);
    }
    /* Fog changes and lost shared vision must be evaluated for each recipient again. */
    level.alliances[5][2] = 0; game.clients[6].ps.rdflags &= ~RDF_NOFOG;
    FOR_LOOP(viewer, 7) {
        uint32_t a, b;
        scan_mode = "0"; a = G_WriteClientDatagram(&g_edicts[viewer], before, sizeof(before));
        scan_mode = "1"; b = G_WriteClientDatagram(&g_edicts[viewer], after, sizeof(after));
        T_EQ(a, b); T_EQ(memcmp(before, after, a), 0);
    }
    gi.CvarString = scan_cvar_orig; G_FowShutdown();
}

/* Exercise both owned-list transports while preserving remote-player alliance refresh cadence. */
static void scan_aura_case(bool cached) {
    bool const old_cache = g_ability_cache;
    UnitAbilities_t abilities = { .abilList = "XUau" };
    slkTestData_t *rows = parse_slk_string(scan_aura_slk), *old;
    edict_t *source, *own, *remote;
    reset_entities(); setup_test_world();
    old = G_SetSLKRows("AbilityData", rows); g_ability_cache = cached;
    ((mapInfo_t *)level.mapinfo)->players[2].playerType = kPlayerTypeHuman;
    ((mapInfo_t *)level.mapinfo)->players[5].playerType = kPlayerTypeHuman;
    source = scan_unit(2, 0); own = scan_unit(2, 32); remote = scan_unit(5, 64);
    own->targtype = remote->targtype = TARG_GROUND;
    own->health.max_value = remote->health.max_value = 1000;
    FOR_LOOP(i, 64) scan_unit(PLAYER_NEUTRAL_PASSIVE, (float)i * 4);
    source->data.UnitAbilities = &abilities;
    scan_cvar_orig = gi.CvarString; gi.CvarString = scan_cvar; scan_mode = "1";
    level.time = 1000; level.framenum++;
    S_TestResetHeroAuraAliasResolves();
    T_FEQ(S_UnholyHealthRegen(own), 17, 0.001f); T_FEQ(S_UnholyHealthRegen(remote), 0, 0.001f);
    T_EQ(S_TestHeroAuraAliasResolves(), 1);
    level.alliances[2][5] |= 1u << ALLIANCE_PASSIVE;
    level.alliances[5][2] |= 1u << ALLIANCE_PASSIVE;
    level.time += AURA_UPDATE_MS; level.framenum++;
    T_FEQ(S_UnholyHealthRegen(remote), 17, 0.001f);
    source->data.UnitAbilities = NULL; T_ASSERT(G_ActorAddSkill(source, aura_id));
    level.time += AURA_UPDATE_MS; level.framenum++; S_TestResetHeroAuraAliasResolves();
    T_FEQ(S_UnholyHealthRegen(own), 17, 0.001f); T_EQ(S_TestHeroAuraAliasResolves(), 1);
    T_ASSERT(G_ActorRemoveSkill(source, aura_id));
    source->heroabilities[0] = (heroability_t){ .code = aura_id, .level = 1 };
    level.time += AURA_UPDATE_MS; level.framenum++; S_TestResetHeroAuraAliasResolves();
    T_FEQ(S_UnholyHealthRegen(own), 17, 0.001f); T_EQ(S_TestHeroAuraAliasResolves(), 1);
    source->heroabilities[0].level = 0;
    level.time += AURA_UPDATE_MS; level.framenum++; S_TestResetHeroAuraAliasResolves();
    T_FEQ(S_UnholyHealthRegen(own), 0, 0.001f); T_EQ(S_TestHeroAuraAliasResolves(), 0);
    scan_mode = "0"; level.time += AURA_UPDATE_MS; level.framenum++; S_TestResetHeroAuraAliasResolves();
    T_FEQ(S_UnholyHealthRegen(own), 0, 0.001f); T_EQ(S_TestHeroAuraAliasResolves(), globals.num_edicts);
    gi.CvarString = scan_cvar_orig; g_ability_cache = old_cache; G_SetSLKRows("AbilityData", old); free_slk_rows(rows);
}
TEST(wc3_server_work, aura_candidates_keep_native_added_hero_and_remote_alliance_sources) {
    scan_aura_case(false); scan_aura_case(true);
}
#endif
