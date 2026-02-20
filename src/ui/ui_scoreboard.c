/*
 * QUICKEN Engine - Scoreboard Overlay
 *
 * Shows all connected players, team scores, individual stats.
 * Drawn when the player holds the scoreboard key.
 */

#include "ui/qk_ui.h"
#include "gameplay/qk_gameplay.h"
#include "renderer/qk_renderer.h"
#include <stdio.h>

// --- Colors ---
static const u32 SB_COLOR_BG     = 0x00000099;
static const u32 SB_COLOR_HEADER = 0xFFFFFFFF;
static const u32 SB_COLOR_ALPHA  = 0xFF4444FF;
static const u32 SB_COLOR_BETA   = 0x4444FFFF;
static const u32 SB_COLOR_TEXT   = 0xCCCCCCFF;
static const u32 SB_COLOR_DEAD   = 0x666666FF;

void qk_ui_draw_scoreboard(const qk_ca_state_t *ca,
                             f32 screen_w, f32 screen_h) {
    if (!ca) return;

    // background panel centered on screen
    f32 panel_w = 500.0f;
    f32 panel_h = 400.0f;
    f32 px = (screen_w - panel_w) * 0.5f;
    f32 py = (screen_h - panel_h) * 0.5f;

    qk_ui_draw_rect(px, py, panel_w, panel_h, SB_COLOR_BG);

    // title
    char title[64];
    snprintf(title, sizeof(title), "CLAN ARENA - Round %u", ca->round_number);
    f32 tw = qk_ui_text_width(title, 24.0f);
    qk_ui_draw_text(screen_w * 0.5f - tw * 0.5f, py + 10.0f, title, 24.0f, SB_COLOR_HEADER);

    // team scores
    char score_line[64];
    snprintf(score_line, sizeof(score_line), "ALPHA: %u   BETA: %u",
             ca->score_alpha, ca->score_beta);
    f32 sw = qk_ui_text_width(score_line, 20.0f);
    qk_ui_draw_text(screen_w * 0.5f - sw * 0.5f, py + 40.0f, score_line, 20.0f, SB_COLOR_TEXT);

    // column headers
    f32 col_x = px + 20.0f;
    f32 row_y = py + 80.0f;
    qk_ui_draw_text(col_x, row_y, "Player", 14.0f, SB_COLOR_HEADER);
    qk_ui_draw_text(col_x + 180.0f, row_y, "Frags", 14.0f, SB_COLOR_HEADER);
    qk_ui_draw_text(col_x + 260.0f, row_y, "Deaths", 14.0f, SB_COLOR_HEADER);
    qk_ui_draw_text(col_x + 350.0f, row_y, "Dmg", 14.0f, SB_COLOR_HEADER);

    // player rows
    f32 entry_y = row_y + 24.0f;
    f32 row_height = 20.0f;

    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        const qk_player_state_t *ps = qk_game_get_player_state(i);
        if (!ps) continue;

        u32 color;
        if (ps->alive_state == QK_PSTATE_DEAD) {
            color = SB_COLOR_DEAD;
        } else if (ps->team == QK_TEAM_ALPHA) {
            color = SB_COLOR_ALPHA;
        } else if (ps->team == QK_TEAM_BETA) {
            color = SB_COLOR_BETA;
        } else {
            color = SB_COLOR_TEXT;
        }

        char player_label[16];
        snprintf(player_label, sizeof(player_label), "Player %u", i);
        qk_ui_draw_text(col_x, entry_y, player_label, 14.0f, color);

        qk_ui_draw_number(col_x + 180.0f, entry_y, ps->frags, 14.0f, color);
        qk_ui_draw_number(col_x + 260.0f, entry_y, ps->deaths, 14.0f, color);
        qk_ui_draw_number(col_x + 350.0f, entry_y, ps->damage_given, 14.0f, color);

        entry_y += row_height;
    }

    // alive counts
    char alive_line[64];
    snprintf(alive_line, sizeof(alive_line), "Alive: Alpha %u  Beta %u",
             ca->alive_alpha, ca->alive_beta);
    f32 aw = qk_ui_text_width(alive_line, 14.0f);
    qk_ui_draw_text(screen_w * 0.5f - aw * 0.5f,
                     py + panel_h - 30.0f, alive_line, 14.0f, SB_COLOR_TEXT);
}
