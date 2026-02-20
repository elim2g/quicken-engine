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

// --- Tick Rate ---
#define QK_TICK_RATE            128
static const f32 QK_TICK_DT     = 1.0f / 128.0f;
static const f64 QK_TICK_DT_F64 = 1.0 / 128.0;
static const u32 QK_TICK_DT_MS_NOM = 8;

// --- Entity Limits ---
#define QK_MAX_ENTITIES         256
#define QK_MAX_PLAYERS          32

// --- Physics Constants ---
static const f32 QK_PM_GROUND_ACCEL    = 10.0f;
static const f32 QK_PM_AIR_ACCEL       = 1.0f;
static const f32 QK_PM_GROUND_FRICTION = 6.0f;
static const f32 QK_PM_MAX_SPEED       = 320.0f;
static const f32 QK_PM_JUMP_VELOCITY   = 270.0f;
static const f32 QK_PM_GRAVITY         = 800.0f;
static const f32 QK_PM_STEP_HEIGHT     = 18.0f;
static const f32 QK_PM_OVERCLIP        = 1.001f;
static const f32 QK_PM_STOP_SPEED      = 100.0f;
static const f32 QK_PM_MIN_WALK_NORMAL = 0.7f;
static const f32 QK_PM_AIR_SPEED       = 270.0f;  // air wish speed (0.84 * max_speed)
static const u32 QK_PM_JUMP_BUFFER_TICKS = 4;     // ~31ms at 128Hz
static const u32 QK_PM_SKIM_TICKS        = 25;    // ~195ms at 128Hz (Quake-style 200ms skim window)
static const f32 QK_TRACE_EPSILON       = 0.03125f;

// --- CPM (Challenge ProMode) Movement ---
static const f32 QK_PM_CPM_AIR_ACCEL         = 70.0f;   // air accel for A/D strafing
static const f32 QK_PM_CPM_WISH_SPEED        = 30.0f;   // wish speed for CPM air control
static const f32 QK_PM_CPM_STRAFE_ACCEL      = 70.0f;   // strafe-only air acceleration
static const f32 QK_PM_CPM_GROUND_ACCEL      = 15.0f;   // CPM ground acceleration (higher than VQ3)
static const f32 QK_PM_CPM_GROUND_SPEED      = 320.0f;  // CPM ground speed
static const u32 QK_PM_CPM_DOUBLE_JUMP_WINDOW = 400;    // ms window for double-jump after last jump
static const u32 QK_PM_CPM_DOUBLE_JUMP_BOOST  = 100;    // additive impulse for double jumps

// --- Player Bounding Box ---
static const f32 QK_PLAYER_MINS_X = -15.0f;
static const f32 QK_PLAYER_MINS_Y = -15.0f;
static const f32 QK_PLAYER_MINS_Z = -24.0f;
static const f32 QK_PLAYER_MAXS_X =  15.0f;
static const f32 QK_PLAYER_MAXS_Y =  15.0f;
static const f32 QK_PLAYER_MAXS_Z =  32.0f;
#define QK_PLAYER_MINS          ((vec3_t){QK_PLAYER_MINS_X, QK_PLAYER_MINS_Y, QK_PLAYER_MINS_Z})
#define QK_PLAYER_MAXS          ((vec3_t){QK_PLAYER_MAXS_X, QK_PLAYER_MAXS_Y, QK_PLAYER_MAXS_Z})

// --- Button Flags ---
enum {
    QK_BUTTON_ATTACK  = (1 << 0),
    QK_BUTTON_JUMP    = (1 << 1),
    QK_BUTTON_CROUCH  = (1 << 2),
    QK_BUTTON_USE     = (1 << 3),
};

// --- Clan Arena Constants ---
static const u32 QK_CA_ROUNDS_TO_WIN  = 10;
static const u32 QK_CA_COUNTDOWN_MS   = 5000;
static const u32 QK_CA_ROUND_TIME_MS  = 120000;
static const u32 QK_CA_ROUND_END_MS   = 3000;
static const i16 QK_CA_SPAWN_HEALTH   = 200;
static const i16 QK_CA_SPAWN_ARMOR    = 200;

// --- Enums ---

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

// --- User Command ---

typedef struct {
    u32     server_time;
    f32     forward_move;       // -1.0 to 1.0 (normalized)
    f32     side_move;
    f32     up_move;
    f32     pitch;              // view angles in degrees
    f32     yaw;
    u32     buttons;
    u8      weapon_select;      // 0 = no change, otherwise weapon_id_t
} qk_usercmd_t;

// --- Player State ---

typedef struct {
    // --- Physics fields (authoritative movement state) ---
    vec3_t      origin;
    vec3_t      velocity;
    vec3_t      mins;
    vec3_t      maxs;
    bool        on_ground;
    vec3_t      ground_normal;
    bool        jump_held;
    u8          jump_buffer_ticks;
    u8          splash_slick_ticks;
    u8          skim_ticks;         // ground skim: slide along walls without velocity penalty
    u32         last_jump_tick;     // tick when player last jumped (for CPM double-jump timing)
    f32         max_speed;
    f32         gravity;
    u32         command_time;

    // --- Gameplay fields (combat/identity state) ---
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
    u8          teleport_bit;   // XOR-toggled on each teleport (never cleared)
    qk_usercmd_t last_cmd;
} qk_player_state_t;

// --- Trace Result ---

typedef struct {
    f32     fraction;           // 0.0 = start in solid, 1.0 = no hit
    vec3_t  end_pos;
    vec3_t  hit_normal;
    f32     hit_dist;
    bool    start_solid;
    bool    all_solid;
    i32     brush_index;        // -1 if none
    i32     entity_id;          // -1 if none
} qk_trace_result_t;

// --- Spawn Point ---

typedef struct {
    vec3_t  origin;
    f32     yaw;
} qk_spawn_point_t;

// --- Teleporter ---

typedef struct {
    vec3_t  origin;         // trigger volume center
    vec3_t  mins;           // trigger volume AABB min
    vec3_t  maxs;           // trigger volume AABB max
    vec3_t  destination;    // destination position
    f32     dest_yaw;       // destination facing angle
} qk_teleporter_t;

// --- Jump Pad ---

typedef struct {
    vec3_t  origin;         // trigger volume center
    vec3_t  mins;           // trigger volume AABB min
    vec3_t  maxs;           // trigger volume AABB max
    vec3_t  target;         // target position (apex or destination)
} qk_jump_pad_t;

// --- Entity State Flags ---
enum {
    QK_ENT_FLAG_ON_GROUND  = (1 << 0),
    QK_ENT_FLAG_JUMP_HELD  = (1 << 1),
    QK_ENT_FLAG_TELEPORTED = (1 << 2),  // toggle bit: XOR between snapshots detects teleport
    QK_ENT_FLAG_FIRING     = (1 << 3),  // entity is currently firing its weapon
};

// --- Layout Assertions ---
_Static_assert(sizeof(qk_usercmd_t) == 32,
               "qk_usercmd_t size changed — update netcode serialization");
_Static_assert(sizeof(qk_trace_result_t) == 44,
               "qk_trace_result_t size changed — check physics/gameplay boundary");

#endif // QK_TYPES_H
