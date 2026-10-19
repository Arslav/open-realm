#ifdef BZ_TESTS
#include "test.h"
#include "../g_local.h"
#include "../game/skills/s_skills.h"

edict_t *alloc_test_unit(uint32_t class_id, float x, float y);
void reset_entities(void);
void setup_test_world(void);
slkTestData_t *parse_slk_string(char const *slk_text);
void free_slk_rows(slkTestData_t *rows);

static bool query_cache_on;
static cstring_t query_cache_cvar(cstring_t name, cstring_t fallback) {
    return !strcmp(name, "wc3_ability_cache") ? (query_cache_on ? "1" : "0") : fallback;
}

/* A cold decode inside an owned-ability walk must not overwrite that caller's current token. */
TEST(wc3_ability_dispatch, query_cache_keeps_outer_parser_token_on_cold_decode) {
    cstring_t (*old)(cstring_t, cstring_t) = gi.CvarString;
    UnitAbilities_t row = { .abilList = "A001,A002" };
    reset_entities(); setup_test_world();
    edict_t *unit = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 0, 0);
    unit->data.UnitAbilities = &row;
    query_cache_on = true; gi.CvarString = query_cache_cvar; InitAbilities();
    PARSE_LIST(row.abilList, token, parse_segment) {
        uint32_t before = FS_SLKKey(token);
        T_ASSERT(G_ActorHasSkill(unit, token));
        T_EQ(FS_SLKKey(token), before);
    }
    gi.CvarString = old; InitAbilities();
}

/* Repeated membership must reuse a decode while live add/remove and row rebinding remain immediate. */
TEST(wc3_ability_dispatch, query_cache_reuses_lists_and_tracks_runtime_membership) {
    extern uint32_t G_TestAbilityListParses;
    cstring_t (*old)(cstring_t, cstring_t) = gi.CvarString;
    char text[] = "AInv,\"A001\",bad, A002";
    UnitAbilities_t row = { .abilList = text }, other = { .abilList = "A003" };
    reset_entities(); setup_test_world();
    edict_t *unit = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 0, 0);
    unit->data.UnitAbilities = &row;
    query_cache_on = true; gi.CvarString = query_cache_cvar; InitAbilities();
    G_TestAbilityListParses = 0;
    FOR_LOOP(i, 100) { T_ASSERT(G_ActorHasSkill(unit, "A001")); T_ASSERT(!G_ActorHasSkill(unit, "A003")); }
    T_EQ(G_TestAbilityListParses, 1);
    T_ASSERT(G_ActorRemoveSkill(unit, MAKEFOURCC('A','0','0','1')));
    T_ASSERT(!G_ActorHasSkill(unit, "A001"));
    T_ASSERT(G_ActorAddSkill(unit, MAKEFOURCC('A','0','0','1')));
    T_ASSERT(G_ActorHasSkill(unit, "A001"));
    unit->data.UnitAbilities = &other;
    T_ASSERT(G_ActorHasSkill(unit, "A003")); T_ASSERT(!G_ActorHasSkill(unit, "A002"));
    unit->data.UnitAbilities = &row;
    memcpy(text, "A003", 4);
    T_ASSERT(G_ActorHasSkill(unit, "A003")); T_ASSERT(!G_ActorHasSkill(unit, "AInv"));
    query_cache_on = false; InitAbilities();
    G_TestAbilityListParses = 0;
    FOR_LOOP(i, 10) T_ASSERT(G_ActorHasSkill(unit, "A002"));
    T_EQ(G_TestAbilityListParses, 10);
    gi.CvarString = old; InitAbilities();
}

/* Rank queries on ordinary units must not resolve an alias before finding any ranked entry. */
TEST(wc3_ability_dispatch, query_cache_skips_empty_ranks_and_invalidates_alias_rows) {
    extern uint32_t G_TestAbilityCodeLookups;
    cstring_t (*old)(cstring_t, cstring_t) = gi.CvarString;
    UnitAbilities_t row = { .abilList = "AInv" };
    reset_entities(); setup_test_world();
    edict_t *unit = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 0, 0);
    unit->data.UnitAbilities = &row;
    query_cache_on = true; gi.CvarString = query_cache_cvar; InitAbilities();
    G_TestAbilityCodeLookups = 0;
    FOR_LOOP(i, 100) T_EQ(G_UnitAbilityLevel(unit, MAKEFOURCC('A','I','n','v')), 1);
    T_EQ(G_TestAbilityCodeLookups, 0);
    uint32_t const id = MAKEFOURCC('A','0','0','1');
    slkTestData_t *rows = parse_slk_string("ID;PWXL;N;EBB;Y2;X2\nC;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\nC;Y2;X1;K\"A001\"\nC;Y2;X2;K\"Ahea\"\nE\n");
    slkTestData_t *saved = G_SetSLKRows("AbilityData", rows);
    G_TestAbilityCodeLookups = 0;
    FOR_LOOP(i, 100) T_EQ(G_AbilityCode(id), MAKEFOURCC('A','h','e','a'));
    T_EQ(G_TestAbilityCodeLookups, 1);
    G_SetSLKRows("AbilityData", saved); free_slk_rows(rows);
    T_EQ(G_AbilityCode(id), G_AbilityData(id)->code ? G_AbilityData(id)->code : id);
    gi.CvarString = old; InitAbilities();
}

/* Registry indexing must preserve first registration, full command names, and concrete alias procedures. */
TEST(wc3_ability_dispatch, query_cache_registry_matches_original_and_handles_long_lists) {
    cstring_t (*old)(cstring_t, cstring_t) = gi.CvarString;
    query_cache_on = true; gi.CvarString = query_cache_cvar; InitAbilities();
    FOR_LOOP(i, game.num_abilities) {
        ability_t const *item = GetAbilityByIndex(i);
        ability_t const *fast = FindAbilityByClassname(item->classname);
        query_cache_on = false; S_UpdateAbilityQueries();
        T_ASSERT(fast == FindAbilityByClassname(item->classname));
        query_cache_on = true; S_UpdateAbilityQueries();
    }
    T_NULL(FindAbilityByClassname("zzzz"));
    T_ASSERT(FindAbilityForCommand(STR_CmdMove) == FindAbilityByClassname(STR_CmdMove));
    reset_entities(); setup_test_world();
    char text[801];
    FOR_LOOP(i, 160) memcpy(text + i * 5, "A001,", 5);
    text[799] = 0;
    UnitAbilities_t row = { .abilList = text };
    edict_t *unit = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 0, 0);
    unit->data.UnitAbilities = &row;
    T_ASSERT(G_ActorHasSkill(unit, "A001")); T_ASSERT(!G_ActorHasSkill(unit, "A002"));
    gi.CvarString = old; InitAbilities();
}

/* An object-data Heal alias must retain its authored amount and cost under autocast. */
TEST(wc3_ability_dispatch, autocast_keeps_authored_alias) {
    const char slk[] =
        "ID;PWXL;N;EBB;Y4;X6\n"
        "C;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\nC;Y1;X3;K\"targs\"\n"
        "C;Y1;X4;K\"Cost1\"\nC;Y1;X5;K\"Rng1\"\nC;Y1;X6;K\"DataA1\"\n"
        "C;Y2;X1;K\"Ahea\"\nC;Y2;X2;K\"Ahea\"\nC;Y2;X3;K\"air,ground,friend\"\n"
        "C;Y2;X4;K\"5\"\nC;Y2;X5;K\"600\"\nC;Y2;X6;K\"25\"\n"
        "C;Y3;X1;K\"A001\"\nC;Y3;X2;K\"Ahea\"\nC;Y3;X3;K\"air,ground,friend\"\n"
        "C;Y3;X4;K\"13\"\nC;Y3;X5;K\"600\"\nC;Y3;X6;K\"37\"\n"
        "C;Y4;X1;K\"A002\"\nC;Y4;X2;K\"Ahea\"\nC;Y4;X3;K\"air,ground,friend\"\n"
        "C;Y4;X4;K\"23\"\nC;Y4;X5;K\"600\"\nC;Y4;X6;K\"89\"\nE\n";
    UnitAbilities_t list = { .abilList = "A001,A002" };
    slkTestData_t *rows = parse_slk_string(slk), *old = G_SetSLKRows("AbilityData", rows);
    reset_entities();
    setup_test_world();
    edict_t *caster = alloc_test_unit(MAKEFOURCC('h','p','r','i'), 0, 0);
    edict_t *target = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 50, 0);
    caster->data.UnitAbilities = &list;
    caster->mana.value = caster->mana.max_value = 200;
    caster->s.player = target->s.player = 0;
    caster->svflags |= SVF_MONSTER;
    target->svflags |= SVF_MONSTER;
    target->targtype = TARG_GROUND;
    target->health.value = 100; target->health.max_value = 1000;
    T_ASSERT(G_SetUnitAutocast(caster, FS_SLKKey("A001"), true));
    T_ASSERT(G_TryUnitAutocast(caster));
    T_FEQ(target->health.value, 137, 0.001f);
    T_FEQ(caster->mana.value, 187, 0.001f);
    T_ASSERT(G_UnitAutocastIsOn(caster, FS_SLKKey("A001")));
    T_ASSERT(!G_UnitAutocastIsOn(caster, FS_SLKKey("A002")));
    T_ASSERT(G_SetUnitAutocast(caster, FS_SLKKey("A001"), false));
    T_ASSERT(!G_TryUnitAutocast(caster));
    T_ASSERT(G_SetUnitAutocast(caster, FS_SLKKey("A002"), true));
    T_ASSERT(G_TryUnitAutocast(caster));
    T_FEQ(target->health.value, 226, 0.001f);
    T_FEQ(caster->mana.value, 164, 0.001f);
    T_ASSERT(G_ActorRemoveSkill(caster, FS_SLKKey("A002")));
    T_EQ(caster->autocast_code, 0);
    T_ASSERT(!G_TryUnitAutocast(caster));
    T_ASSERT(!G_SetUnitAutocast(caster, FS_SLKKey("A002"), true));
    T_ASSERT(G_ActorAddSkill(caster, FS_SLKKey("A002")));
    T_ASSERT(G_SetUnitAutocast(caster, FS_SLKKey("A002"), true));
    T_ASSERT(G_TryUnitAutocast(caster));
    T_FEQ(target->health.value, 315, 0.001f);
    G_SetSLKRows("AbilityData", old);
    free_slk_rows(rows);
}
/* All Human boolean-message handlers and Repair must switch without leaking the old policy. */
TEST(wc3_ability_dispatch, autocast_boolean_messages_switch_and_remove) {
    const char slk[] =
        "ID;PWXL;N;EBB;Y8;X2\nC;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\n"
        "C;Y2;X1;K\"Ahea\"\nC;Y2;X2;K\"Ahea\"\n"
        "C;Y3;X1;K\"Ainf\"\nC;Y3;X2;K\"Ainf\"\n"
        "C;Y4;X1;K\"Aslo\"\nC;Y4;X2;K\"Aslo\"\n"
        "C;Y5;X1;K\"Asps\"\nC;Y5;X2;K\"Asps\"\n"
        "C;Y6;X1;K\"Arep\"\nC;Y6;X2;K\"Arep\"\n"
        "C;Y7;X1;K\"Aren\"\nC;Y7;X2;K\"Aren\"\n"
        "C;Y8;X1;K\"AEpa\"\nC;Y8;X2;K\"AEpa\"\nE\n";
    slkTestData_t *rows = parse_slk_string(slk), *old = G_SetSLKRows("AbilityData", rows);
    uint32_t const codes[] = { MAKEFOURCC('A','h','e','a'), MAKEFOURCC('A','i','n','f'),
        MAKEFOURCC('A','s','l','o'), MAKEFOURCC('A','s','p','s'),
        MAKEFOURCC('A','r','e','p'), MAKEFOURCC('A','r','e','n') };
    reset_entities(); setup_test_world();
    edict_t *caster = alloc_test_unit(MAKEFOURCC('h','p','r','i'), 0, 0);
    UnitAbilities_t list = { .abilList = "" };
    caster->data.UnitAbilities = &list;
    FOR_LOOP(i, sizeof(codes) / sizeof(codes[0])) {
        T_ASSERT(G_ActorAddSkill(caster, codes[i]));
        T_ASSERT(G_SetUnitAutocast(caster, codes[i], true));
        T_ASSERT(G_UnitAutocastIsOn(caster, codes[i]));
        if (i) T_ASSERT(!G_UnitAutocastIsOn(caster, codes[i - 1]));
    }
    T_ASSERT(G_SetUnitAutocast(caster, codes[4], false));
    T_ASSERT(G_UnitAutocastIsOn(caster, codes[5]));
    T_ASSERT(G_ActorAddSkill(caster, FS_SLKKey("AEpa")));
    T_ASSERT(!G_SetUnitAutocast(caster, FS_SLKKey("AEpa"), true));
    T_ASSERT(G_UnitAutocastIsOn(caster, codes[5]));
    T_ASSERT(G_ActorRemoveSkill(caster, codes[5]));
    T_ASSERT(!(caster->aiflags & AI_AUTOCAST_ACTIVE));
    T_ASSERT(!(caster->aiflags & AI_AUTOCAST_REPAIR));
    T_EQ(caster->autocast_code, 0);
    G_SetSLKRows("AbilityData", old); free_slk_rows(rows);
}

/* Stock Shackles supplies separate caster/target buffs; cancellation must release only this cast's target. */
TEST(wc3_ability_dispatch, shackles_locks_target_until_its_channel_ends) {
    const char slk[] =
        "ID;PWXL;N;EBB;Y2;X8\n"
        "C;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\nC;Y1;X3;K\"targs\"\nC;Y1;X4;K\"Rng1\"\n"
        "C;Y1;X5;K\"Dur1\"\nC;Y1;X6;K\"HeroDur1\"\nC;Y1;X7;K\"DataA1\"\nC;Y1;X8;K\"BuffID1\"\n"
        "C;Y2;X1;K\"Amls\"\nC;Y2;X2;K\"Amls\"\nC;Y2;X3;K\"air,enemy\"\nC;Y2;X4;K\"800\"\n"
        "C;Y2;X5;K\"10\"\nC;Y2;X6;K\"10\"\nC;Y2;X7;K\"30\"\nC;Y2;X8;K\"Bmlc,Bmlt\"\nE\n";
    slkTestData_t *rows = parse_slk_string(slk), *old = G_SetSLKRows("AbilityData", rows);
    UnitAbilities_t list = { .abilList = "Amls" };
    reset_entities(); setup_test_world(); level.time = 1000;
    ((mapInfo_t *)level.mapinfo)->players[0].playerType = kPlayerTypeHuman;
    ((mapInfo_t *)level.mapinfo)->players[1].playerType = kPlayerTypeHuman;
    edict_t *caster = alloc_test_unit(MAKEFOURCC('h','p','r','i'), 0, 0);
    edict_t *target = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 100, 0), *first = NULL, *second = NULL;
    caster->data.UnitAbilities = &list; caster->s.player = 0; target->s.player = 1;
    caster->svflags |= SVF_MONSTER;
    target->svflags |= SVF_MONSTER; target->targtype = TARG_AIR;
    target->health.value = target->health.max_value = 1000;
    T_ASSERT(S_CastUnitTargetSpell(caster, FS_SLKKey("Amls"), target));
    FILTER_EDICTS(ent, ent->owner == caster && ent->think == human_ability_think) { first = ent; break; }
    T_NOT_NULL(first);
    T_ASSERT(!S_HumanCanAttack(target)); T_FEQ(S_HumanMoveFactor(target), 0, 0.001f);
    T_ASSERT(!G_UnitStatusLevel(target, FS_SLKKey("Bmlc")));
    T_ASSERT(S_CastUnitTargetSpell(caster, FS_SLKKey("Amls"), target));
    FILTER_EDICTS(ent, ent != first && ent->owner == caster && ent->think == human_ability_think) {
        second = ent; break;
    }
    T_NOT_NULL(second);
    if (first) G_RunEntity(first);
    T_ASSERT(S_SpellIsChanneling(caster)); T_ASSERT(!S_HumanCanAttack(target));
    S_SpellCancelChannel(caster);
    if (second) G_RunEntity(second);
    T_ASSERT(S_HumanCanAttack(target)); T_FEQ(S_HumanMoveFactor(target), 1, 0.001f);
    G_SetSLKRows("AbilityData", old); free_slk_rows(rows);
}

/* Replacing the buff while the old thinker exists must not let that thinker
 * damage the target or remove the newer buff during channel cleanup. */
TEST(wc3_ability_dispatch, shackles_old_thinker_does_not_own_replacement_status) {
    const char slk[] =
        "ID;PWXL;N;EBB;Y2;X8\n"
        "C;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\nC;Y1;X3;K\"targs\"\nC;Y1;X4;K\"Rng1\"\n"
        "C;Y1;X5;K\"Dur1\"\nC;Y1;X6;K\"HeroDur1\"\nC;Y1;X7;K\"DataA1\"\nC;Y1;X8;K\"BuffID1\"\n"
        "C;Y2;X1;K\"Amls\"\nC;Y2;X2;K\"Amls\"\nC;Y2;X3;K\"air,enemy\"\nC;Y2;X4;K\"800\"\n"
        "C;Y2;X5;K\"10\"\nC;Y2;X6;K\"10\"\nC;Y2;X7;K\"30\"\nC;Y2;X8;K\"Bmlc,Bmlt\"\nE\n";
    slkTestData_t *rows = parse_slk_string(slk), *old = G_SetSLKRows("AbilityData", rows);
    UnitAbilities_t list = { .abilList = "Amls" };
    edict_t *caster, *target, *thinker = NULL;
    heroabilitystatus_t *slot;
    uint32_t new_instance;
    float before;
    reset_entities(); setup_test_world(); level.time = 1000;
    ((mapInfo_t *)level.mapinfo)->players[0].playerType = kPlayerTypeHuman;
    ((mapInfo_t *)level.mapinfo)->players[1].playerType = kPlayerTypeHuman;
    caster = alloc_test_unit(MAKEFOURCC('h','p','r','i'), 0, 0);
    target = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 100, 0);
    caster->data.UnitAbilities = &list; caster->s.player = 0; target->s.player = 1;
    caster->svflags |= SVF_MONSTER;
    target->svflags |= SVF_MONSTER; target->targtype = TARG_AIR;
    target->health.value = target->health.max_value = 1000;
    T_ASSERT(S_CastUnitTargetSpell(caster, FS_SLKKey("Amls"), target));
    FILTER_EDICTS(ent, ent->owner == caster && ent->think == human_ability_think) {
        thinker = ent; break;
    }
    T_NOT_NULL(thinker);
    slot = unit_findstatus(target, FS_SLKKey("Bmlt"));
    T_NOT_NULL(slot);
    T_EQ(thinker->variation, slot->instance_id);
    T_ASSERT(unit_addtimedstatus(target, "Bmlt", 1, 10.0f));
    slot = unit_findstatus(target, FS_SLKKey("Bmlt"));
    T_NOT_NULL(slot);
    new_instance = slot->instance_id;
    T_NE(thinker->variation, new_instance);
    before = target->health.value;
    G_RunEntity(thinker);
    T_FEQ(target->health.value, before, 0.001f);
    slot = unit_findstatus(target, FS_SLKKey("Bmlt"));
    T_NOT_NULL(slot);
    T_EQ(slot->instance_id, new_instance);
    G_SetSLKRows("AbilityData", old); free_slk_rows(rows);
}

#endif
