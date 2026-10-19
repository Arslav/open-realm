#ifdef BZ_TESTS
#include "shared/test.h"
#include "../g_local.h"
#include "../hud/hud_local.h"
#include "jass/jass.h"

bool run_test_jass(cstring_t src);
extern player_t *currentplayer;

static struct {
    uint32_t buttons, labels;
    char command[64];
    float label_height;
    uiGlueTextButton_t art;
} cinematic_ui;

/* Capture the actual HUD payload, including the native button's skinned states. */
static void cinematic_ui_write(pfWriteType_t type, void const *data) {
    uiFrame_t const *frame = data;
    if (type != PF_UIFRAME || !data) return;
    if (frame->onclick && !strcmp(frame->onclick, "cinematic_skip")) {
        cinematic_ui.buttons++;
        snprintf(cinematic_ui.command, sizeof(cinematic_ui.command), "%s", frame->onclick);
        if (frame->buffer.size == sizeof(cinematic_ui.art))
            memcpy(&cinematic_ui.art, frame->buffer.data, sizeof(cinematic_ui.art));
    }
    if (frame->flags.type == FT_TEXT && frame->text && !strcmp(frame->text, "Пропустить")) {
        cinematic_ui.labels++;
        cinematic_ui.label_height = frame->size.height;
    }
}

static void cinematic_ui_unicast(edict_t *ent) { (void)ent; }

/* An empty cinematic still needs a touch-accessible skip; gameplay transmissions must not expose it. */
TEST(wc3_cinematic_ui, skip_button_is_skinned_and_only_sent_during_cutscenes) {
    __typeof__(gi.Write) write = gi.Write;
    __typeof__(gi.unicast) unicast = gi.unicast;
    stbIniCache_t theme = game.config.theme;
    edict_t *ent = &g_edicts[0];
    memset(&game.config.theme, 0, sizeof(game.config.theme));
    T_ASSERT(Stb_IniCacheLoad(&game.config.theme, "UI\\war3skins.txt"));
    UI_ResetHud();
    UI_LoadHudCinematic();
    frameDef_t *button = UI_FindChildFrame(hud.cinematic.CinematicPanel, "CinematicSkipButton");
    T_NOT_NULL(button);
    gi.Write = cinematic_ui_write; gi.unicast = cinematic_ui_unicast;
    ent->client->connected = true;
    cinematic_ui = (__typeof__(cinematic_ui)){ 0 };
    UI_ShowInterface(ent, false, 0);
    UI_WriteDialoguePresentation(ent);
    T_EQ(cinematic_ui.buttons, 1);
    T_EQ(cinematic_ui.labels, 1);
    T_ASSERT(cinematic_ui.art.normal.Background != 0 && cinematic_ui.art.normal.EdgeFile != 0);
    T_ASSERT(cinematic_ui.art.pushed.Background != 0 && cinematic_ui.art.highlight.alphaFile != 0);
    if (button) {
        frameDef_t *style = UI_FindFrame("EscMenuButtonTemplate");
        T_NOT_NULL(style);
        T_FEQ(button->Height, style->Height, 0.0001f);
        T_FEQ(cinematic_ui.label_height, button->Height, 0.0001f);
        T_ASSERT(button->Width > button->Height);
    }
    cinematic_ui = (__typeof__(cinematic_ui)){ 0 };
    UI_ShowInterface(ent, true, 0);
    UI_WriteDialoguePresentation(ent);
    T_EQ(cinematic_ui.buttons, 0);
    T_ASSERT(ent->client->ps.uiflags & (1u << LAYER_CINEMATIC));
    gi.Write = write; gi.unicast = unicast;
    UI_ResetHud();
    Stb_IniCacheFree(&game.config.theme);
    game.config.theme = theme;
}

/* Use a nonzero map player: the button must invoke authored Escape cleanup, including control and lens. */
TEST(wc3_cinematic_ui, skip_runs_map_cleanup_and_ignores_late_clicks) {
    gameClient_t *client = game.clients;
    cstring_t command[] = { "cinematic_skip" };
    game.clients[1].ps.number = 0;
    client->ps.number = 1;
    currentplayer = NULL;
    T_ASSERT(run_test_jass(
        "globals\n"
        "  integer skips = 0\n"
        "endglobals\n"
        "function cleanup takes nothing returns nothing\n"
        "  set skips = skips + 1\n"
        "  call BJassAssert(skips == 1, \"duplicate skip\")\n"
        "  if GetLocalPlayer() == Player(1) then\n"
        "    call ResetToGameCamera(0.0)\n"
        "    call ShowInterface(true, 0.0)\n"
        "    call EnableUserControl(true)\n"
        "  endif\n"
        "endfunction\n"
        "function main takes nothing returns nothing\n"
        "  local trigger t = CreateTrigger()\n"
        "  call TriggerRegisterPlayerEvent(t, Player(1), EVENT_PLAYER_END_CINEMATIC)\n"
        "  call TriggerAddAction(t, function cleanup)\n"
        "  if GetLocalPlayer() == Player(1) then\n"
        "    call ShowInterface(false, 0.0)\n"
        "    call EnableUserControl(false)\n"
        "  endif\n"
        "endfunction\n"));
    T_EQ(client->ps.client_ui_state, CLIENT_UI_CINEMATIC);
    T_ASSERT(client->no_control);
    client->camera.state.target_distance = 900;
    globals.ClientCommand(g_edicts, 1, command);
    G_RunEvents(); jass_runevents(level.vm); G_RunClients();
    T_ASSERT(!jass_rterror_pending(level.vm));
    T_EQ(client->ps.client_ui_state, CLIENT_UI_GAME);
    T_ASSERT(!client->no_control);
    T_FEQ(client->ps.distance, WC3_CAMERA_DEFAULT_DISTANCE, 0.001f);
    globals.ClientCommand(g_edicts, 1, command);
    G_RunEvents(); jass_runevents(level.vm);
    T_ASSERT(!jass_rterror_pending(level.vm));
}
#endif
