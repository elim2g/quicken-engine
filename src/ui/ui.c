/*
 * QUICKEN UI Module - Stub Implementation
 *
 * All functions are no-ops.
 * The gameplay agent replaces this with real code on feat/gameplay.
 */

#include "ui/qk_ui.h"

void qk_ui_draw_hud(const qk_player_state_t *ps,
                     const qk_ca_state_t *ca,
                     f32 screen_w, f32 screen_h) {
    QK_UNUSED(ps); QK_UNUSED(ca); QK_UNUSED(screen_w); QK_UNUSED(screen_h);
}

void qk_ui_draw_scoreboard(const qk_ca_state_t *ca,
                             f32 screen_w, f32 screen_h) {
    QK_UNUSED(ca); QK_UNUSED(screen_w); QK_UNUSED(screen_h);
}

void qk_ui_event_kill(const char *attacker, const char *victim,
                       qk_weapon_id_t weapon) {
    QK_UNUSED(attacker); QK_UNUSED(victim); QK_UNUSED(weapon);
}

void qk_ui_event_hit(i16 damage) {
    QK_UNUSED(damage);
}

void qk_ui_tick(u32 dt_ms) {
    QK_UNUSED(dt_ms);
}
