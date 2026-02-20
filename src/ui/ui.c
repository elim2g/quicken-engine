/*
 * QUICKEN Engine - UI Module Main
 *
 * Wires UI subsystems: killfeed push, hitmarker trigger, tick timers.
 * qk_ui_draw_hud and qk_ui_draw_scoreboard are in ui_hud.c / ui_scoreboard.c.
 * This file implements the event/tick functions declared in include/ui/qk_ui.h.
 */

#include "ui/qk_ui.h"

// Functions defined in ui_hud.c
extern void ui_hitmarker_trigger(i16 damage);
extern void ui_hitmarker_tick(u32 dt_ms);
extern void ui_killfeed_push(const char *attacker, const char *victim,
                              qk_weapon_id_t weapon);
extern void ui_killfeed_tick(u32 dt_ms);

// --- Event Push ---

void qk_ui_event_kill(const char *attacker, const char *victim,
                       qk_weapon_id_t weapon) {
    ui_killfeed_push(attacker, victim, weapon);
}

void qk_ui_event_hit(i16 damage) {
    ui_hitmarker_trigger(damage);
}

// --- Tick ---

void qk_ui_tick(u32 dt_ms) {
    ui_hitmarker_tick(dt_ms);
    ui_killfeed_tick(dt_ms);
}
