/*
 * QUICKEN Engine - HUD Rendering
 *
 * Immediate-mode HUD: health, armor, ammo, timer, team scores, crosshair.
 * Reads player_state_t and ca_state_t, issues draw calls to renderer.
 */

#include "ui/qk_ui.h"
#include "renderer/qk_renderer.h"
#include <stdio.h>

/* ---- HUD Color Constants ---- */
#define COLOR_WHITE         0xFFFFFFFF
#define COLOR_RED           0xFF0000FF
#define COLOR_GREEN         0x00FF00FF
#define COLOR_YELLOW        0xFFFF00FF
#define COLOR_CYAN          0x00FFFFFF
#define COLOR_BLUE          0x4444FFFF
#define COLOR_ORANGE        0xFF8800FF
#define COLOR_GRAY          0x888888FF
#define COLOR_TEAM_ALPHA    COLOR_RED
#define COLOR_TEAM_BETA     COLOR_BLUE

/* ---- Crosshair ---- */
static void ui_draw_crosshair(f32 screen_w, f32 screen_h) {
    f32 cx = screen_w * 0.5f;
    f32 cy = screen_h * 0.5f;
    f32 gap = 3.0f;
    f32 len = 8.0f;
    f32 thick = 2.0f;

    /* four lines: top, bottom, left, right */
    qk_ui_draw_rect(cx - thick * 0.5f, cy - gap - len, thick, len, COLOR_WHITE);
    qk_ui_draw_rect(cx - thick * 0.5f, cy + gap,       thick, len, COLOR_WHITE);
    qk_ui_draw_rect(cx - gap - len,    cy - thick * 0.5f, len, thick, COLOR_WHITE);
    qk_ui_draw_rect(cx + gap,          cy - thick * 0.5f, len, thick, COLOR_WHITE);
}

/* ---- Hit Marker ---- */
#define HITMARKER_DURATION_MS 200

typedef struct {
    u32     time_remaining_ms;
    i16     damage;
} hitmarker_state_t;

static hitmarker_state_t s_hitmarker;

void ui_hitmarker_trigger(i16 damage) {
    s_hitmarker.time_remaining_ms = HITMARKER_DURATION_MS;
    s_hitmarker.damage = damage;
}

void ui_hitmarker_tick(u32 dt_ms) {
    if (s_hitmarker.time_remaining_ms > 0) {
        if (dt_ms >= s_hitmarker.time_remaining_ms) {
            s_hitmarker.time_remaining_ms = 0;
        } else {
            s_hitmarker.time_remaining_ms -= dt_ms;
        }
    }
}

static void ui_draw_hitmarker(f32 screen_w, f32 screen_h) {
    if (s_hitmarker.time_remaining_ms == 0) return;

    f32 cx = screen_w * 0.5f;
    f32 cy = screen_h * 0.5f;
    f32 alpha = (f32)s_hitmarker.time_remaining_ms / (f32)HITMARKER_DURATION_MS;
    u32 color = ((u32)(alpha * 255.0f)) | 0xFFFFFF00;

    f32 offset = 6.0f;
    f32 len = 10.0f;
    f32 thick = 2.0f;

    /* four diagonal lines */
    qk_ui_draw_rect(cx - offset - len, cy - offset - len, len, thick, color);
    qk_ui_draw_rect(cx + offset,       cy - offset - len, len, thick, color);
    qk_ui_draw_rect(cx - offset - len, cy + offset,       len, thick, color);
    qk_ui_draw_rect(cx + offset,       cy + offset,       len, thick, color);
}

/* ---- Killfeed ---- */
#define KILLFEED_MAX_ENTRIES    5
#define KILLFEED_DISPLAY_MS     5000

typedef struct {
    char        attacker_name[32];
    char        victim_name[32];
    qk_weapon_id_t weapon;
    u32         time_remaining_ms;
    bool        active;
} killfeed_entry_t;

static killfeed_entry_t s_killfeed[KILLFEED_MAX_ENTRIES];

void ui_killfeed_push(const char *attacker, const char *victim, qk_weapon_id_t weapon) {
    /* shift entries down */
    for (int i = KILLFEED_MAX_ENTRIES - 1; i > 0; i--) {
        s_killfeed[i] = s_killfeed[i - 1];
    }

    /* insert new at index 0 */
    killfeed_entry_t *entry = &s_killfeed[0];
    snprintf(entry->attacker_name, sizeof(entry->attacker_name), "%s",
             attacker ? attacker : "???");
    snprintf(entry->victim_name, sizeof(entry->victim_name), "%s",
             victim ? victim : "???");
    entry->weapon = weapon;
    entry->time_remaining_ms = KILLFEED_DISPLAY_MS;
    entry->active = true;
}

void ui_killfeed_tick(u32 dt_ms) {
    for (int i = 0; i < KILLFEED_MAX_ENTRIES; i++) {
        if (!s_killfeed[i].active) continue;
        if (dt_ms >= s_killfeed[i].time_remaining_ms) {
            s_killfeed[i].time_remaining_ms = 0;
            s_killfeed[i].active = false;
        } else {
            s_killfeed[i].time_remaining_ms -= dt_ms;
        }
    }
}

static void ui_draw_killfeed(f32 screen_w) {
    f32 y = 60.0f;
    f32 entry_height = 20.0f;

    for (int i = 0; i < KILLFEED_MAX_ENTRIES; i++) {
        if (!s_killfeed[i].active) continue;

        /* fade out in last 1000ms */
        f32 alpha = 1.0f;
        if (s_killfeed[i].time_remaining_ms < 1000) {
            alpha = (f32)s_killfeed[i].time_remaining_ms / 1000.0f;
        }
        u32 text_color = ((u32)(alpha * 255.0f)) | 0xFFFFFF00;

        /* "attacker > victim" right-aligned */
        char line[80];
        snprintf(line, sizeof(line), "%s > %s",
                 s_killfeed[i].attacker_name, s_killfeed[i].victim_name);

        f32 tw = qk_ui_text_width(line, 14.0f);
        qk_ui_draw_text(screen_w - 20.0f - tw, y, line, 14.0f, text_color);

        y += entry_height;
    }
}

/* ---- Weapon name lookup ---- */
static const char *weapon_name(qk_weapon_id_t w) {
    switch (w) {
    case QK_WEAPON_ROCKET: return "Rocket Launcher";
    case QK_WEAPON_RAIL:   return "Railgun";
    case QK_WEAPON_LG:     return "Lightning Gun";
    default:               return "";
    }
}

/* ---- Main HUD Draw ---- */
void qk_ui_draw_hud(const qk_player_state_t *ps,
                     const qk_ca_state_t *ca,
                     f32 screen_w, f32 screen_h) {
    if (!ps || !ca) return;

    /* ---- Bottom bar ---- */

    /* Health: bottom-left */
    u32 hp_color = (ps->health <= 25) ? COLOR_RED :
                   (ps->health <= 50) ? COLOR_ORANGE : COLOR_WHITE;
    qk_ui_draw_number(20.0f, screen_h - 60.0f, ps->health, 48.0f, hp_color);

    /* Armor: next to health */
    u32 ap_color = (ps->armor <= 25) ? COLOR_RED :
                   (ps->armor <= 50) ? COLOR_YELLOW : COLOR_GREEN;
    qk_ui_draw_number(160.0f, screen_h - 60.0f, ps->armor, 48.0f, ap_color);

    /* Ammo: bottom-right */
    if (ps->weapon > QK_WEAPON_NONE && ps->weapon < QK_WEAPON_COUNT) {
        u32 ammo_color = (ps->ammo[ps->weapon] <= 5) ? COLOR_RED : COLOR_YELLOW;
        qk_ui_draw_number(screen_w - 120.0f, screen_h - 60.0f,
                           ps->ammo[ps->weapon], 48.0f, ammo_color);
        const char *wname = weapon_name(ps->weapon);
        qk_ui_draw_text(screen_w - 120.0f, screen_h - 24.0f, wname, 14.0f, COLOR_GRAY);
    }

    /* ---- Top bar ---- */

    /* Round timer: top-center */
    u32 time_sec = ca->state_timer_ms / 1000;
    char timer_buf[8];
    snprintf(timer_buf, sizeof(timer_buf), "%u:%02u", time_sec / 60, time_sec % 60);
    f32 tw = qk_ui_text_width(timer_buf, 32.0f);
    qk_ui_draw_text(screen_w * 0.5f - tw * 0.5f, 16.0f, timer_buf, 32.0f, COLOR_WHITE);

    /* Team scores */
    char score_a[4], score_b[4];
    snprintf(score_a, sizeof(score_a), "%u", ca->score_alpha);
    snprintf(score_b, sizeof(score_b), "%u", ca->score_beta);
    qk_ui_draw_text(screen_w * 0.5f - 80.0f, 16.0f, score_a, 32.0f, COLOR_TEAM_ALPHA);
    qk_ui_draw_text(screen_w * 0.5f + 60.0f, 16.0f, score_b, 32.0f, COLOR_TEAM_BETA);

    /* ---- Crosshair ---- */
    ui_draw_crosshair(screen_w, screen_h);

    /* ---- Hit marker ---- */
    ui_draw_hitmarker(screen_w, screen_h);

    /* ---- Killfeed ---- */
    ui_draw_killfeed(screen_w);
}
