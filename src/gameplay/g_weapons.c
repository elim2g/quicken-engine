/*
 * QUICKEN Engine - Weapon System
 *
 * Data-driven weapon definitions table, weapon tick (cooldown/switch),
 * fire dispatch (hitscan, projectile, beam).
 */

#include "g_internal.h"

/* ---- Weapon Definitions Table ---- */
const g_weapon_def_t g_weapon_defs[QK_WEAPON_COUNT] = {
    [QK_WEAPON_NONE] = {0},

    [QK_WEAPON_ROCKET] = {
        .id                 = QK_WEAPON_ROCKET,
        .name               = "Rocket Launcher",
        .fire_mode          = FIRE_PROJECTILE,
        .fire_interval_ms   = 800,
        .switch_time_ms     = 50,
        .damage             = 100.0f,
        .splash_radius      = 120.0f,
        .splash_damage      = 100.0f,
        .self_damage_mult   = 0.5f,
        .knockback          = 1.0f,
        .self_knockback     = 10.0f,
        .speed              = 1000.0f,
        .projectile_lifetime = 10.0f,
        .ammo_per_shot      = 1,
        .max_ammo           = 25,
        .range              = 0.0f,
    },

    [QK_WEAPON_RAIL] = {
        .id                 = QK_WEAPON_RAIL,
        .name               = "Railgun",
        .fire_mode          = FIRE_HITSCAN,
        .fire_interval_ms   = 1500,
        .switch_time_ms     = 50,
        .damage             = 80.0f,
        .splash_radius      = 0.0f,
        .splash_damage      = 0.0f,
        .self_damage_mult   = 0.0f,
        .knockback          = 1.0f,
        .self_knockback     = 0.0f,
        .speed              = 0.0f,
        .projectile_lifetime = 0.0f,
        .ammo_per_shot      = 1,
        .max_ammo           = 10,
        .range              = 8192.0f,
    },

    [QK_WEAPON_LG] = {
        .id                 = QK_WEAPON_LG,
        .name               = "Lightning Gun",
        .fire_mode          = FIRE_BEAM,
        .fire_interval_ms   = 50,
        .switch_time_ms     = 50,
        .damage             = 7.0f,
        .splash_radius      = 0.0f,
        .splash_damage      = 0.0f,
        .self_damage_mult   = 0.0f,
        .knockback          = 0.04f,
        .self_knockback     = 0.0f,
        .speed              = 0.0f,
        .projectile_lifetime = 0.0f,
        .ammo_per_shot      = 1,
        .max_ammo           = 150,
        .range              = 768.0f,
    },
};

/* ---- Weapon Switch ---- */
void g_weapon_switch(entity_t *player_ent, qk_weapon_id_t new_weapon) {
    qk_player_state_t *ps = &player_ent->data.player;
    if (new_weapon == ps->weapon) return;
    if (new_weapon <= QK_WEAPON_NONE || new_weapon >= QK_WEAPON_COUNT) return;
    if (ps->pending_weapon != QK_WEAPON_NONE) return; /* already switching */

    const g_weapon_def_t *wdef = &g_weapon_defs[new_weapon];
    ps->pending_weapon = new_weapon;
    ps->switch_time = wdef->switch_time_ms;
}

/* ---- Weapon Fire ---- */
bool g_weapon_fire(qk_game_state_t *gs, entity_t *player_ent) {
    qk_player_state_t *ps = &player_ent->data.player;
    const g_weapon_def_t *wdef = &g_weapon_defs[ps->weapon];

    /* check ammo */
    if (ps->ammo[ps->weapon] < wdef->ammo_per_shot) return false;

    /* subtract ammo */
    ps->ammo[ps->weapon] -= wdef->ammo_per_shot;

    /* set cooldown */
    ps->weapon_time = wdef->fire_interval_ms;

    /* compute fire origin and direction */
    vec3_t forward = angles_to_forward(ps->pitch, ps->yaw);
    /* eye position: player origin + eye height offset */
    vec3_t eye = ps->origin;
    eye.z += 26.0f; /* approximate eye height */

    /* dispatch based on fire mode */
    switch (wdef->fire_mode) {
    case FIRE_HITSCAN:
        g_combat_hitscan_trace(gs, player_ent, eye, forward, wdef->range, ps->weapon);
        break;
    case FIRE_PROJECTILE:
        g_projectile_spawn(gs, player_ent, ps->weapon, eye, forward);
        break;
    case FIRE_BEAM:
        g_combat_beam_trace(gs, player_ent, eye, forward, wdef->range, ps->weapon);
        break;
    }

    return true;
}

/* ---- Weapon Tick (per player, per server tick) ---- */
void g_weapon_tick(qk_game_state_t *gs, entity_t *player_ent, u32 tick_dt_ms) {
    qk_player_state_t *ps = &player_ent->data.player;

    /* handle weapon switch */
    if (ps->switch_time > 0) {
        if (tick_dt_ms >= ps->switch_time) {
            ps->switch_time = 0;
            ps->weapon = ps->pending_weapon;
            ps->pending_weapon = QK_WEAPON_NONE;
            ps->weapon_time = 0;
        } else {
            ps->switch_time -= tick_dt_ms;
        }
        return; /* cannot fire while switching */
    }

    /* handle weapon cooldown */
    if (ps->weapon_time > 0) {
        if (tick_dt_ms >= ps->weapon_time) {
            ps->weapon_time = 0;
        } else {
            ps->weapon_time -= tick_dt_ms;
        }
        return;
    }

    /* weapon ready: check if attack button pressed */
    if (ps->last_cmd.buttons & QK_BUTTON_ATTACK) {
        g_weapon_fire(gs, player_ent);
    }
}
