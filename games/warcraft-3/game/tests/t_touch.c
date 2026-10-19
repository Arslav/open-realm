#ifdef BZ_TESTS
#include "test.h"
#include "../g_local.h"

void setup_test_world(void);
edict_t *alloc_test_unit(uint32_t class_id, float x, float y);
void SP_monster_tree(edict_t *self);
slkTestData_t *parse_slk_string(char const *slk_text);
void free_slk_rows(slkTestData_t *rows);
static void touch_test_write(pfWriteType_t type, void const *value) { (void)type; (void)value; }
static void touch_test_unicast(edict_t *ent) { (void)ent; }

/* Retain real game dispatch and omit only network delivery to an unconnected test viewer. */
static void touch_test_command(uint32_t argc, cstring_t argv[]) {
    void (*write)(pfWriteType_t, void const *) = gi.Write;
    void (*unicast)(edict_t *) = gi.unicast;
    gi.Write = touch_test_write; gi.unicast = touch_test_unicast;
    G_ClientCommand(g_edicts, argc, argv);
    gi.Write = write; gi.unicast = unicast;
}

/* Exercise the wire command, including its optional ground hit, through game dispatch. */
static void touch_test_tap(edict_t *target, bool ground) {
    char number[16];
    snprintf(number, sizeof(number), "%u", target ? target->s.number : 0);
    touch_test_command(5, (cstring_t[]){ "touch", number, ground ? "1" : "0", "256", "192" });
}

static edict_t *touch_test_unit(uint32_t owner) {
    edict_t *unit = alloc_test_unit(MAKEFOURCC('h','f','o','o'), 64, 64);
    unit->svflags |= SVF_MONSTER;
    unit->s.player = owner;
    unit->targtype = TARG_GROUND;
    ((mapInfo_t *)level.mapinfo)->players[owner].playerType = kPlayerTypeHuman;
    return unit;
}

TEST(wc3_touch, tap_selects_owned_unit_then_ground_moves_without_changing_selection) {
    setup_test_world();
    edict_t *unit = touch_test_unit(0);
    touch_test_tap(unit, true);
    T_ASSERT(G_IsEntitySelected(game.clients, unit));
    touch_test_tap(NULL, true);
    T_ASSERT(G_IsEntitySelected(game.clients, unit));
    T_NOT_NULL(unit->currentmove);
    if (unit->currentmove) T_EQ(unit->currentmove->proc, CAbilityMove);
    T_NOT_NULL(unit->goalentity);
}

TEST(wc3_touch, tap_owned_building_selects_instead_of_repairing_or_following) {
    setup_test_world();
    edict_t *unit = touch_test_unit(0);
    edict_t *building = alloc_test_unit(MAKEFOURCC('h','b','a','r'), 256, 64);
    building->svflags |= SVF_MONSTER;
    building->s.player = 0;
    unit->selected = 1;
    touch_test_tap(building, true);
    T_ASSERT(G_IsEntitySelected(game.clients, building));
    T_ASSERT(!G_IsEntitySelected(game.clients, unit));
    T_NULL(unit->goalentity);
}

TEST(wc3_touch, enemy_tap_inspects_without_army_and_attacks_with_army) {
    setup_test_world();
    edict_t *enemy = touch_test_unit(1);
    touch_test_tap(enemy, true);
    T_ASSERT(G_IsEntitySelected(game.clients, enemy));
    edict_t *unit = touch_test_unit(0);
    enemy->selected = 0; unit->selected = 1;
    unit->attack1.type = ATK_NORMAL;
    unit->attack1.targetsAllowed = WC3_TARGET_FLAG_GROUND;
    unit->attack1.range = 100;
    touch_test_tap(enemy, true);
    T_ASSERT(G_IsEntitySelected(game.clients, unit));
    T_ASSERT(!G_IsEntitySelected(game.clients, enemy));
    T_EQ(unit->goalentity, enemy);
}

TEST(wc3_touch, active_attack_accepts_owned_target_instead_of_changing_selection) {
    setup_test_world();
    edict_t *unit = touch_test_unit(0), *target = touch_test_unit(0);
    unit->selected = 1;
    unit->attack1.type = ATK_NORMAL;
    unit->attack1.targetsAllowed = WC3_TARGET_FLAG_GROUND;
    game.clients[0].menu.on_entity_selected = attack_menu_selecttarget;
    touch_test_tap(target, true);
    T_ASSERT(G_IsEntitySelected(game.clients, unit));
    T_ASSERT(!G_IsEntitySelected(game.clients, target));
    T_EQ(unit->goalentity, target);
    T_NULL(game.clients[0].menu.on_entity_selected);
}

TEST(wc3_touch, worker_tap_harvests_tree_without_trying_to_select_it) {
    setup_test_world();
    edict_t *worker = alloc_test_unit(MAKEFOURCC('h','p','e','a'), 64, 64);
    edict_t *tree = alloc_test_unit(MAKEFOURCC('L','T','l','t'), 192, 64);
    worker->svflags |= SVF_MONSTER;
    worker->s.player = 0; worker->selected = 1;
    SP_monster_tree(tree);
    tree->targtype = TARG_TREE;
    tree->health.value = tree->health.max_value = 500;
    touch_test_tap(tree, true);
    T_ASSERT(G_IsEntitySelected(game.clients, worker));
    T_EQ(worker->goalentity, tree);
    T_EQ(worker->secondarygoal, tree);
    T_NOT_NULL(worker->currentmove);
}

TEST(wc3_touch, active_point_command_uses_ground_even_when_entity_was_hit) {
    setup_test_world();
    edict_t *unit = touch_test_unit(0), *target = touch_test_unit(0);
    unit->selected = 1;
    game.clients[0].menu.on_location_selected = move_selectlocation;
    touch_test_tap(target, true);
    T_ASSERT(G_IsEntitySelected(game.clients, unit));
    T_ASSERT(!G_IsEntitySelected(game.clients, target));
    T_NOT_NULL(unit->currentmove);
    if (unit->currentmove) T_EQ(unit->currentmove->proc, CAbilityMove);
    T_NOT_NULL(unit->goalentity);
    if (unit->goalentity) {
        T_FEQ(unit->goalentity->s.origin2.x, 256, 1);
        T_FEQ(unit->goalentity->s.origin2.y, 192, 1);
    }
}

TEST(wc3_touch, holy_light_tap_heals_ally_and_keeps_hero_selected) {
    char const *slk =
        "ID;PWXL;N;EBB;Y2;X7\n"
        "C;Y1;X1;K\"alias\"\nC;Y1;X2;K\"code\"\nC;Y1;X3;K\"targs\"\n"
        "C;Y1;X4;K\"Cost1\"\nC;Y1;X5;K\"Cool1\"\nC;Y1;X6;K\"Rng1\"\nC;Y1;X7;K\"DataA1\"\n"
        "C;Y2;X1;K\"AHhb\"\nC;Y2;X2;K\"AHhb\"\nC;Y2;X3;K\"air,ground,friend,self\"\n"
        "C;Y2;X4;K\"41\"\nC;Y2;X5;K\"3\"\nC;Y2;X6;K\"700\"\nC;Y2;X7;K\"173\"\nE\n";
    UnitAbilities_t abilities = { .abilList = "AHhb" };
    setup_test_world();
    slkTestData_t *rows = parse_slk_string(slk), *old = G_SetSLKRows("AbilityData", rows);
    edict_t *hero = alloc_test_unit(MAKEFOURCC('H','p','a','l'), 64, 64), *ally = touch_test_unit(0);
    hero->svflags |= SVF_MONSTER;
    hero->data.UnitAbilities = &abilities;
    hero->mana.value = hero->mana.max_value = 200;
    hero->health.value = hero->health.max_value = 500;
    hero->stand = unit_stand; hero->movetype = MOVETYPE_NONE; unit_stand(hero);
    hero->selected = 1;
    ally->health.value = 100; ally->health.max_value = 500;
    touch_test_command(2, (cstring_t[]){ "button", "AHhb" });
    T_NOT_NULL(game.clients[0].menu.on_entity_selected);
    touch_test_tap(hero, true);
    T_FEQ(hero->mana.value, 200, 0.001f);
    T_NOT_NULL(game.clients[0].menu.on_entity_selected);
    touch_test_tap(ally, true);
    T_FEQ(ally->health.value, 273, 0.001f);
    T_FEQ(hero->mana.value, 159, 0.001f);
    T_ASSERT(G_IsEntitySelected(game.clients, hero));
    T_ASSERT(!G_IsEntitySelected(game.clients, ally));
    T_NULL(game.clients[0].menu.on_entity_selected);
    G_SetSLKRows("AbilityData", old); free_slk_rows(rows);
}

TEST(wc3_touch, misses_and_invalid_packets_do_not_replace_selection_or_issue_orders) {
    setup_test_world();
    edict_t *unit = touch_test_unit(0);
    unit->selected = 1;
    touch_test_tap(NULL, false);
    G_ClientCommand(g_edicts, 5, (cstring_t[]){ "touch", "999999", "1", "256", "192" });
    G_ClientCommand(g_edicts, 5, (cstring_t[]){ "touch", "0", "bad", "256", "192" });
    G_ClientCommand(g_edicts, 2, (cstring_t[]){ "touch", "0" });
    G_ClientCommand(g_edicts, 5, (cstring_t[]){ "touch", "0", "1", "nan", "192" });
    G_ClientCommand(g_edicts, 5, (cstring_t[]){ "touch", "0", "1", "256", "9999999999999999999999999999" });
    T_ASSERT(G_IsEntitySelected(game.clients, unit));
    T_NULL(unit->currentmove);
    T_NULL(unit->goalentity);
}
#endif
