/*
 * QUICKEN Engine - Gameplay Internal Types
 *
 * Entity system, weapon definitions, round state.
 * NOT a public header -- only included by src/gameplay/ files.
 */

#ifndef G_LOCAL_H
#define G_LOCAL_H

#include "quicken.h"
#include "qk_math.h"
#include "qk_types.h"

/* Entity types */
typedef enum {
    ENTITY_NONE = 0,
    ENTITY_PLAYER,
    ENTITY_PROJECTILE,
    ENTITY_TYPE_COUNT
} entity_type_t;

/* Projectile data */
typedef struct {
    vec3_t          origin;
    vec3_t          velocity;
    u8              owner;
    qk_weapon_id_t weapon;
    u32             spawn_time;
    f32             damage;
    f32             splash_radius;
    f32             splash_damage;
} projectile_t;

/* Entity (tagged union) */
typedef struct {
    entity_type_t   type;
    u8              id;
    bool            active;
    union {
        qk_player_state_t  player;
        projectile_t        projectile;
    } data;
} entity_t;

/* Entity pool */
typedef struct {
    entity_t    entities[QK_MAX_ENTITIES];
    u32         count;
    u32         high_water;
} entity_pool_t;

/* Round state for Clan Arena */
typedef enum {
    CA_STATE_WARMUP = 0,
    CA_STATE_COUNTDOWN,
    CA_STATE_PLAYING,
    CA_STATE_ROUND_END,
    CA_STATE_MATCH_END
} ca_round_state_t;

/* Weapon definition table entry */
typedef struct {
    qk_weapon_id_t  id;
    const char     *name;
    u32             fire_interval_ms;
    f32             damage;
    f32             splash_damage;
    f32             splash_radius;
    f32             speed;          /* projectile speed, 0 for hitscan */
    u16             start_ammo;
} weapon_def_t;

#endif /* G_LOCAL_H */
