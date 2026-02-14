/*
 * QUICKEN Engine - Gameplay Internal Definitions
 *
 * Types and function declarations used across gameplay .c files.
 * NOT a public header. Supplements g_local.h.
 */

#ifndef G_INTERNAL_H
#define G_INTERNAL_H

#include "g_local.h"
#include "gameplay/qk_gameplay.h"
#include "physics/qk_physics.h"
#include <string.h>

/* ---- Weapon fire mode ---- */
typedef enum {
    FIRE_HITSCAN = 0,
    FIRE_PROJECTILE,
    FIRE_BEAM
} fire_mode_t;

/* ---- Extended weapon definition ---- */
typedef struct {
    qk_weapon_id_t  id;
    const char     *name;
    fire_mode_t     fire_mode;
    u32             fire_interval_ms;
    u32             switch_time_ms;
    f32             damage;
    f32             splash_radius;
    f32             splash_damage;
    f32             self_damage_mult;
    f32             knockback;
    f32             self_knockback;
    f32             speed;
    f32             projectile_lifetime;
    u16             ammo_per_shot;
    u16             max_ammo;
    f32             range;
} g_weapon_def_t;

/* Global weapon definitions table (defined in g_weapons.c) */
extern const g_weapon_def_t g_weapon_defs[QK_WEAPON_COUNT];

/* ---- Game Event Types ---- */
typedef enum {
    GEVT_KILL = 0,
    GEVT_HIT,
    GEVT_ROUND_START,
    GEVT_ROUND_END,
    GEVT_MATCH_END,
    GEVT_COUNT
} game_event_type_t;

typedef struct {
    game_event_type_t type;
    u32 server_time;
    union {
        struct { u8 attacker; u8 victim; qk_weapon_id_t weapon; } kill;
        struct { u8 target; i16 damage; } hit;
        struct { u8 round_number; } round_start;
        struct { u8 winner_team; u8 score_a; u8 score_b; } round_end;
        struct { u8 winner_team; } match_end;
    } data;
} game_event_t;

#define MAX_GAME_EVENTS_PER_TICK 32

typedef struct {
    game_event_t events[MAX_GAME_EVENTS_PER_TICK];
    u32 count;
} game_event_queue_t;

/* ---- Damage Event ---- */
typedef struct {
    u8              attacker_id;
    u8              victim_id;
    i16             damage;
    vec3_t          dir;
    f32             knockback;
    qk_weapon_id_t weapon;
    bool            is_self;
} damage_event_t;

/* ---- Game State (opaque struct definition) ---- */
struct qk_game_state {
    entity_pool_t       entities;
    qk_ca_state_t       ca;
    game_event_queue_t  events;
    u32                 server_time_ms;
    u8                  num_clients;
    i32                 player_entity[QK_MAX_PLAYERS]; /* entity index per client, -1 = none */

    /* config (copied from init) */
    u8                  max_players;
    u8                  rounds_to_win;
    u32                 round_time_limit_ms;
    u32                 countdown_time_ms;
};

/* ---- Entity functions (g_entity.c) ---- */
void      g_entity_pool_init(entity_pool_t *pool);
entity_t *g_entity_alloc(entity_pool_t *pool, entity_type_t type);
void      g_entity_free(entity_pool_t *pool, entity_t *ent);
entity_t *g_entity_find(entity_pool_t *pool, u8 id);
entity_t *g_entity_first(entity_pool_t *pool, entity_type_t type);
entity_t *g_entity_next(entity_pool_t *pool, entity_t *after, entity_type_t type);

/* ---- Player functions (g_player.c) ---- */
void g_player_spawn_ca(entity_t *ent, vec3_t spawn_origin, f32 spawn_yaw);
void g_player_apply_armor(qk_player_state_t *ps, i16 raw_damage,
                           i16 *out_health_dmg, i16 *out_armor_dmg);

/* ---- Weapon functions (g_weapons.c) ---- */
void g_weapon_tick(qk_game_state_t *gs, entity_t *player_ent, u32 tick_dt_ms);
bool g_weapon_fire(qk_game_state_t *gs, entity_t *player_ent);
void g_weapon_switch(entity_t *player_ent, qk_weapon_id_t new_weapon);

/* ---- Combat functions (g_combat.c) ---- */
void g_combat_apply_damage(qk_game_state_t *gs, const damage_event_t *dmg);
void g_combat_kill(qk_game_state_t *gs, u8 attacker_id, u8 victim_id,
                    qk_weapon_id_t weapon);
void g_combat_hitscan_trace(qk_game_state_t *gs, entity_t *attacker,
                             vec3_t start, vec3_t dir, f32 range,
                             qk_weapon_id_t weapon);
void g_combat_beam_trace(qk_game_state_t *gs, entity_t *attacker,
                          vec3_t start, vec3_t dir, f32 range,
                          qk_weapon_id_t weapon);
void g_combat_splash_damage(qk_game_state_t *gs, vec3_t origin,
                             f32 radius, f32 max_damage, f32 knockback,
                             u8 attacker_id, qk_weapon_id_t weapon,
                             u8 skip_id);

/* ---- Projectile functions (g_projectile.c) ---- */
entity_t *g_projectile_spawn(qk_game_state_t *gs, entity_t *owner,
                              qk_weapon_id_t weapon, vec3_t origin,
                              vec3_t direction);
void g_projectile_tick(qk_game_state_t *gs, f32 dt,
                       const qk_phys_world_t *world);

/* ---- Clan Arena functions (g_ca.c) ---- */
void g_ca_init(qk_game_state_t *gs);
void g_ca_tick(qk_game_state_t *gs, u32 dt_ms);
void g_ca_start_countdown(qk_game_state_t *gs);
void g_ca_begin_round(qk_game_state_t *gs);
void g_ca_end_round(qk_game_state_t *gs);
void g_ca_end_round_timeout(qk_game_state_t *gs);
void g_ca_count_alive(qk_game_state_t *gs);

/* ---- Event functions (g_event.c) ---- */
void g_event_push(game_event_queue_t *queue, const game_event_t *event);
void g_event_clear(game_event_queue_t *queue);

/* ---- Process commands (gameplay.c) ---- */
void g_process_commands(qk_game_state_t *gs, u32 tick_dt_ms);

/* ---- Utility ---- */
static inline u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }

/* Forward direction from angles (pitch=x, yaw=y in degrees) */
static inline vec3_t angles_to_forward(f32 pitch, f32 yaw) {
    f32 cp = cosf(pitch * 3.14159265f / 180.0f);
    f32 sp = sinf(pitch * 3.14159265f / 180.0f);
    f32 cy = cosf(yaw * 3.14159265f / 180.0f);
    f32 sy = sinf(yaw * 3.14159265f / 180.0f);
    return (vec3_t){ cp * cy, cp * sy, sp };
}

/*
 * Ray-AABB intersection (slab method).
 * ray_origin + t * ray_dir, t in [0, max_t].
 * Returns true if hit, writes nearest t to *out_t.
 */
static inline bool ray_aabb_intersect(vec3_t ray_origin, vec3_t ray_dir,
                                       f32 max_t, vec3_t aabb_min,
                                       vec3_t aabb_max, f32 *out_t) {
    f32 tmin = 0.0f;
    f32 tmax = max_t;

    /* X slab */
    if (ray_dir.x != 0.0f) {
        f32 inv = 1.0f / ray_dir.x;
        f32 t1 = (aabb_min.x - ray_origin.x) * inv;
        f32 t2 = (aabb_max.x - ray_origin.x) * inv;
        if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    } else {
        if (ray_origin.x < aabb_min.x || ray_origin.x > aabb_max.x) return false;
    }

    /* Y slab */
    if (ray_dir.y != 0.0f) {
        f32 inv = 1.0f / ray_dir.y;
        f32 t1 = (aabb_min.y - ray_origin.y) * inv;
        f32 t2 = (aabb_max.y - ray_origin.y) * inv;
        if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    } else {
        if (ray_origin.y < aabb_min.y || ray_origin.y > aabb_max.y) return false;
    }

    /* Z slab */
    if (ray_dir.z != 0.0f) {
        f32 inv = 1.0f / ray_dir.z;
        f32 t1 = (aabb_min.z - ray_origin.z) * inv;
        f32 t2 = (aabb_max.z - ray_origin.z) * inv;
        if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    } else {
        if (ray_origin.z < aabb_min.z || ray_origin.z > aabb_max.z) return false;
    }

    if (tmin >= 0.0f) {
        *out_t = tmin;
        return true;
    }
    return false;
}

#endif /* G_INTERNAL_H */
