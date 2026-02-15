/*
 * QUICKEN Engine - Shared Game Types
 *
 * Canonical types used across modules: usercmd, player state, trace result,
 * enums, and all QK_ constants.
 */

#ifndef QK_TYPES_H
#define QK_TYPES_H

#include "quicken.h"
#include "qk_math.h"

/* ---- Tick rate (ALL modules use this) ---- */
#define QK_TICK_RATE            128
#define QK_TICK_DT              (1.0f / 128.0f)
#define QK_TICK_DT_F64          (1.0 / 128.0)
#define QK_TICK_DT_MS_NOM       8

/* ---- Entity limits ---- */
#define QK_MAX_ENTITIES         256
#define QK_MAX_PLAYERS          16

/* ---- Physics constants ---- */
#define QK_PM_GROUND_ACCEL      10.0f
#define QK_PM_AIR_ACCEL         1.0f
#define QK_PM_GROUND_FRICTION   6.0f
#define QK_PM_MAX_SPEED         320.0f
#define QK_PM_JUMP_VELOCITY     270.0f
#define QK_PM_GRAVITY           800.0f
#define QK_PM_STEP_HEIGHT       18.0f
#define QK_PM_OVERCLIP          1.001f
#define QK_PM_STOP_SPEED        100.0f
#define QK_PM_MIN_WALK_NORMAL   0.7f
#define QK_PM_AIR_WISHSPEED_CAP 30.0f
#define QK_PM_JUMP_BUFFER_TICKS 4       /* ~31ms at 128Hz */
#define QK_TRACE_EPSILON        0.03125f

/* ---- Player bounding box ---- */
#define QK_PLAYER_MINS_X        (-15.0f)
#define QK_PLAYER_MINS_Y        (-15.0f)
#define QK_PLAYER_MINS_Z        (-24.0f)
#define QK_PLAYER_MAXS_X        (15.0f)
#define QK_PLAYER_MAXS_Y        (15.0f)
#define QK_PLAYER_MAXS_Z        (32.0f)
#define QK_PLAYER_MINS          ((vec3_t){QK_PLAYER_MINS_X, QK_PLAYER_MINS_Y, QK_PLAYER_MINS_Z})
#define QK_PLAYER_MAXS          ((vec3_t){QK_PLAYER_MAXS_X, QK_PLAYER_MAXS_Y, QK_PLAYER_MAXS_Z})

/* ---- Button flags ---- */
#define QK_BUTTON_ATTACK        (1 << 0)
#define QK_BUTTON_JUMP          (1 << 1)
#define QK_BUTTON_CROUCH        (1 << 2)
#define QK_BUTTON_USE           (1 << 3)

/* ---- Clan Arena constants ---- */
#define QK_CA_ROUNDS_TO_WIN     10
#define QK_CA_COUNTDOWN_MS      5000
#define QK_CA_ROUND_TIME_MS     120000
#define QK_CA_ROUND_END_MS      3000
#define QK_CA_SPAWN_HEALTH      200
#define QK_CA_SPAWN_ARMOR       200

/* ---- Enums ---- */

typedef enum {
    QK_TEAM_NONE = 0,
    QK_TEAM_ALPHA,
    QK_TEAM_BETA,
    QK_TEAM_SPECTATOR
} qk_team_t;

typedef enum {
    QK_WEAPON_NONE = 0,
    QK_WEAPON_ROCKET,
    QK_WEAPON_RAIL,
    QK_WEAPON_LG,
    QK_WEAPON_COUNT
} qk_weapon_id_t;

typedef enum {
    QK_PSTATE_ALIVE = 0,
    QK_PSTATE_DEAD,
    QK_PSTATE_SPECTATING
} qk_player_alive_state_t;

/* ---- User Command ---- */

typedef struct {
    u32     server_time;
    f32     forward_move;       /* -1.0 to 1.0 (normalized) */
    f32     side_move;
    f32     up_move;
    f32     pitch;              /* view angles in degrees */
    f32     yaw;
    u32     buttons;
    u8      weapon_select;      /* 0 = no change, otherwise weapon_id_t */
} qk_usercmd_t;

/* ---- Player State ---- */

typedef struct {
    /* --- Physics fields (authoritative movement state) --- */
    vec3_t      origin;
    vec3_t      velocity;
    vec3_t      mins;
    vec3_t      maxs;
    bool        on_ground;
    vec3_t      ground_normal;
    bool        jump_held;
    u8          jump_buffer_ticks;
    u8          splash_slick_ticks;
    f32         max_speed;
    f32         gravity;
    u32         command_time;

    /* --- Gameplay fields (combat/identity state) --- */
    u8                          client_num;
    qk_team_t                   team;
    qk_player_alive_state_t     alive_state;

    i16         health;
    i16         armor;

    qk_weapon_id_t  weapon;
    qk_weapon_id_t  pending_weapon;
    u32             weapon_time;
    u32             switch_time;
    u16             ammo[QK_WEAPON_COUNT];

    f32         pitch;
    f32         yaw;

    u16         frags;
    u16         deaths;
    u16         damage_given;
    u16         damage_taken;

    u32         respawn_time;
    u8          teleport_bit;   /* XOR-toggled on each teleport (never cleared) */
    qk_usercmd_t last_cmd;
} qk_player_state_t;

/* ---- Trace Result ---- */

typedef struct {
    f32     fraction;           /* 0.0 = start in solid, 1.0 = no hit */
    vec3_t  end_pos;
    vec3_t  hit_normal;
    f32     hit_dist;
    bool    start_solid;
    bool    all_solid;
    i32     brush_index;        /* -1 if none */
    i32     entity_id;          /* -1 if none */
} qk_trace_result_t;

/* ---- Spawn Point ---- */

typedef struct {
    vec3_t  origin;
    f32     yaw;
} qk_spawn_point_t;

/* ---- Teleporter ---- */

typedef struct {
    vec3_t  origin;         /* trigger volume center */
    vec3_t  mins;           /* trigger volume AABB min */
    vec3_t  maxs;           /* trigger volume AABB max */
    vec3_t  destination;    /* destination position */
    f32     dest_yaw;       /* destination facing angle */
} qk_teleporter_t;

/* ---- Jump Pad ---- */

typedef struct {
    vec3_t  origin;         /* trigger volume center */
    vec3_t  mins;           /* trigger volume AABB min */
    vec3_t  maxs;           /* trigger volume AABB max */
    vec3_t  target;         /* target position (apex or destination) */
} qk_jump_pad_t;

/* ---- Entity state flags (n_entity_state_t.flags, shared between netcode and gameplay) ---- */

#define QK_ENT_FLAG_ON_GROUND   (1 << 0)
#define QK_ENT_FLAG_JUMP_HELD   (1 << 1)
#define QK_ENT_FLAG_TELEPORTED  (1 << 2)  /* toggle bit: XOR between snapshots detects teleport */

#endif /* QK_TYPES_H */
