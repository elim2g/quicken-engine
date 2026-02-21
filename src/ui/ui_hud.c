/*
 * QUICKEN Engine - HUD Rendering
 *
 * Immediate-mode HUD: health, armor, ammo, timer, team scores, crosshair.
 * Reads player_state_t and ca_state_t, issues draw calls to renderer.
 */

#include "ui/qk_ui.h"
#include "renderer/qk_renderer.h"
#include <stdio.h>

// --- HUD Color Constants ---
static const u32 COLOR_WHITE      = 0xFFFFFFFF;
static const u32 COLOR_RED        = 0xFF0000FF;
static const u32 COLOR_GREEN      = 0x00FF00FF;
static const u32 COLOR_YELLOW     = 0xFFFF00FF;
static const u32 COLOR_CYAN       = 0x00FFFFFF;
static const u32 COLOR_BLUE       = 0x4444FFFF;
static const u32 COLOR_ORANGE     = 0xFF8800FF;
static const u32 COLOR_GRAY       = 0x888888FF;
static const u32 COLOR_TEAM_ALPHA = 0xFF0000FF;
static const u32 COLOR_TEAM_BETA  = 0x4444FFFF;

// --- Crosshair ---
static void ui_draw_crosshair(f32 screen_w, f32 screen_h) {
    f32 cx = screen_w * 0.5f;
    f32 cy = screen_h * 0.5f;
    f32 gap = 3.0f;
    f32 len = 8.0f;
    f32 thick = 2.0f;

    // four lines: top, bottom, left, right
    qk_ui_draw_rect(cx - thick * 0.5f, cy - gap - len, thick, len, COLOR_WHITE);
    qk_ui_draw_rect(cx - thick * 0.5f, cy + gap,       thick, len, COLOR_WHITE);
    qk_ui_draw_rect(cx - gap - len,    cy - thick * 0.5f, len, thick, COLOR_WHITE);
    qk_ui_draw_rect(cx + gap,          cy - thick * 0.5f, len, thick, COLOR_WHITE);
}

// --- Hit Marker ---
static const u32 HITMARKER_DURATION_MS = 200;

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

    // four diagonal lines
    qk_ui_draw_rect(cx - offset - len, cy - offset - len, len, thick, color);
    qk_ui_draw_rect(cx + offset,       cy - offset - len, len, thick, color);
    qk_ui_draw_rect(cx - offset - len, cy + offset,       len, thick, color);
    qk_ui_draw_rect(cx + offset,       cy + offset,       len, thick, color);
}

// --- Killfeed ---
#define KILLFEED_MAX_ENTRIES    5
static const u32 KILLFEED_DISPLAY_MS = 5000;

typedef struct {
    char        attacker_name[32];
    char        victim_name[32];
    qk_weapon_id_t weapon;
    u32         time_remaining_ms;
    bool        active;
} killfeed_entry_t;

static killfeed_entry_t s_killfeed[KILLFEED_MAX_ENTRIES];

void ui_killfeed_push(const char *attacker, const char *victim, qk_weapon_id_t weapon) {
    // shift entries down
    for (int i = KILLFEED_MAX_ENTRIES - 1; i > 0; i--) {
        s_killfeed[i] = s_killfeed[i - 1];
    }

    // insert new at index 0
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

        // fade out in last 1000ms
        f32 alpha = 1.0f;
        if (s_killfeed[i].time_remaining_ms < 1000) {
            alpha = (f32)s_killfeed[i].time_remaining_ms / 1000.0f;
        }
        u32 text_color = ((u32)(alpha * 255.0f)) | 0xFFFFFF00;

        // "attacker > victim" right-aligned
        char line[80];
        snprintf(line, sizeof(line), "%s > %s",
                 s_killfeed[i].attacker_name, s_killfeed[i].victim_name);

        f32 tw = qk_ui_text_width(line, 14.0f);
        qk_ui_draw_text(screen_w - 20.0f - tw, y, line, 14.0f, text_color);

        y += entry_height;
    }
}

// --- Weapon name lookup ---
static const char *weapon_name(qk_weapon_id_t w) {
    switch (w) {
    case QK_WEAPON_ROCKET: return "Rocket Launcher";
    case QK_WEAPON_RAIL:   return "Railgun";
    case QK_WEAPON_LG:     return "Lightning Gun";
    default:               return "";
    }
}

// --- Main HUD Draw ---
void qk_ui_draw_hud(const qk_player_state_t *ps,
                     const qk_ca_state_t *ca,
                     f32 screen_w, f32 screen_h) {
    if (!ps || !ca) return;

    // --- Top bar: [ red_score | 1-char gap | timer (5 chars) | 1-char gap | blue_score ] ---
    {
        f32 font_size = 32.0f;
        f32 glyph_w = font_size; // 8px base * (32/8) scale = 32px per char
        f32 top_y = 16.0f;

        // Timer: 5-character bounding box, text centered within it
        f32 timer_box_w = 5.0f * glyph_w;
        f32 timer_box_x = screen_w * 0.5f - timer_box_w * 0.5f;

        u32 time_sec = ca->state_timer_ms / 1000;
        char timer_buf[12];
        snprintf(timer_buf, sizeof(timer_buf), "%u:%02u", time_sec / 60, time_sec % 60);
        f32 timer_text_w = qk_ui_text_width(timer_buf, font_size);
        f32 timer_x = timer_box_x + (timer_box_w - timer_text_w) * 0.5f;
        qk_ui_draw_text(timer_x, top_y, timer_buf, font_size, COLOR_WHITE);

        // Team scores: 1-char gap from timer box edges
        f32 gap = glyph_w;

        // Alpha (red) score: right-aligned, ending at timer_box_x - gap
        char score_a[8];
        snprintf(score_a, sizeof(score_a), "%u", ca->score_alpha);
        f32 score_a_w = qk_ui_text_width(score_a, font_size);
        f32 score_a_x = timer_box_x - gap - score_a_w;
        qk_ui_draw_text(score_a_x, top_y, score_a, font_size, COLOR_TEAM_ALPHA);

        // Beta (blue) score: left-aligned, starting at timer_box_x + timer_box_w + gap
        char score_b[8];
        snprintf(score_b, sizeof(score_b), "%u", ca->score_beta);
        f32 score_b_x = timer_box_x + timer_box_w + gap;
        qk_ui_draw_text(score_b_x, top_y, score_b, font_size, COLOR_TEAM_BETA);
    }

    // --- Bottom bar: centered stack ---
    // Layer 1 (bottom): Health + Armor side by side, centered
    // Layer 2: Weapon name, centered above
    // Layer 3 (top): Ammo count, centered above weapon name
    {
        f32 stat_size = 48.0f;
        f32 label_size = 14.0f;
        f32 bottom_margin = 8.0f;
        f32 row_gap = 4.0f;

        // Vertical positions (y = top of text, drawn upward from bottom)
        f32 stat_y = screen_h - bottom_margin - stat_size;
        f32 weapon_y = stat_y - row_gap - label_size;
        f32 ammo_y = weapon_y - row_gap - stat_size;

        // Health + Armor: centered as "[hp]  [armor]" with 1-char gap
        char hp_buf[8], ap_buf[8];
        snprintf(hp_buf, sizeof(hp_buf), "%d", ps->health);
        snprintf(ap_buf, sizeof(ap_buf), "%d", ps->armor);
        f32 hp_w = qk_ui_text_width(hp_buf, stat_size);
        f32 ap_w = qk_ui_text_width(ap_buf, stat_size);
        f32 stat_gap = stat_size; // 1 character at stat_size
        f32 total_w = hp_w + stat_gap + ap_w;
        f32 base_x = screen_w * 0.5f - total_w * 0.5f;

        u32 hp_color = (ps->health <= 25) ? COLOR_RED :
                       (ps->health <= 50) ? COLOR_ORANGE : COLOR_WHITE;
        qk_ui_draw_text(base_x, stat_y, hp_buf, stat_size, hp_color);

        u32 ap_color = (ps->armor <= 25) ? COLOR_RED :
                       (ps->armor <= 50) ? COLOR_YELLOW : COLOR_GREEN;
        qk_ui_draw_text(base_x + hp_w + stat_gap, stat_y, ap_buf, stat_size, ap_color);

        // Weapon name + ammo (only if weapon equipped)
        if (ps->weapon > QK_WEAPON_NONE && ps->weapon < QK_WEAPON_COUNT) {
            const char *wname = weapon_name(ps->weapon);
            f32 wname_w = qk_ui_text_width(wname, label_size);
            qk_ui_draw_text(screen_w * 0.5f - wname_w * 0.5f, weapon_y,
                             wname, label_size, COLOR_GRAY);

            char ammo_buf[8];
            snprintf(ammo_buf, sizeof(ammo_buf), "%d", ps->ammo[ps->weapon]);
            f32 ammo_w = qk_ui_text_width(ammo_buf, stat_size);
            u32 ammo_color = (ps->ammo[ps->weapon] <= 5) ? COLOR_RED : COLOR_YELLOW;
            qk_ui_draw_text(screen_w * 0.5f - ammo_w * 0.5f, ammo_y,
                             ammo_buf, stat_size, ammo_color);
        }
    }

    // --- Crosshair ---
    ui_draw_crosshair(screen_w, screen_h);

    // --- Hit marker ---
    ui_draw_hitmarker(screen_w, screen_h);

    // --- Killfeed ---
    ui_draw_killfeed(screen_w);
}
