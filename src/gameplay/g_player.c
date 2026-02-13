/*
 * QUICKEN Engine - Player Spawn and Armor
 *
 * CA spawn values, armor absorption (Quake-style 66% red armor).
 */

#include "g_internal.h"

void g_player_spawn_ca(entity_t *ent, vec3_t spawn_origin, f32 spawn_yaw) {
    QK_ASSERT(ent && ent->type == ENTITY_PLAYER);
    qk_player_state_t *ps = &ent->data.player;

    ps->origin = spawn_origin;
    ps->velocity = (vec3_t){0, 0, 0};
    ps->yaw = spawn_yaw;
    ps->pitch = 0.0f;

    ps->mins = QK_PLAYER_MINS;
    ps->maxs = QK_PLAYER_MAXS;
    ps->on_ground = false;
    ps->jump_held = false;
    ps->max_speed = QK_PM_MAX_SPEED;
    ps->gravity = QK_PM_GRAVITY;

    ps->health = QK_CA_SPAWN_HEALTH;
    ps->armor = QK_CA_SPAWN_ARMOR;
    ps->alive_state = QK_PSTATE_ALIVE;

    ps->weapon = QK_WEAPON_ROCKET;
    ps->pending_weapon = QK_WEAPON_NONE;
    ps->weapon_time = 0;
    ps->switch_time = 0;

    ps->ammo[QK_WEAPON_ROCKET] = 25;
    ps->ammo[QK_WEAPON_RAIL] = 10;
    ps->ammo[QK_WEAPON_LG] = 150;

    ps->frags = 0;
    ps->deaths = 0;
    ps->damage_given = 0;
    ps->damage_taken = 0;

    memset(&ps->last_cmd, 0, sizeof(ps->last_cmd));
}

void g_player_apply_armor(qk_player_state_t *ps, i16 raw_damage,
                           i16 *out_health_dmg, i16 *out_armor_dmg) {
    f32 absorb = (f32)raw_damage * 0.66f;
    i16 armor_dmg = (i16)absorb;
    if (armor_dmg > ps->armor) {
        armor_dmg = ps->armor;
    }
    *out_armor_dmg = armor_dmg;
    *out_health_dmg = raw_damage - armor_dmg;
}
