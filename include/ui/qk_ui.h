/*
 * QUICKEN Engine - UI Public API
 *
 * HUD drawing, game events, fade timers.
 */

#ifndef QK_UI_H
#define QK_UI_H

#include "quicken.h"
#include "qk_types.h"
#include "gameplay/qk_gameplay.h"

/* HUD drawing (called by client main loop after world render) */
void qk_ui_draw_hud(const qk_player_state_t *ps,
                     const qk_ca_state_t *ca,
                     f32 screen_w, f32 screen_h);
void qk_ui_draw_scoreboard(const qk_ca_state_t *ca,
                             f32 screen_w, f32 screen_h);

/* Event push (called when receiving game events from server) */
void qk_ui_event_kill(const char *attacker, const char *victim,
                       qk_weapon_id_t weapon);
void qk_ui_event_hit(i16 damage);

/* Tick fade timers (called once per client frame) */
void qk_ui_tick(u32 dt_ms);

#endif /* QK_UI_H */
