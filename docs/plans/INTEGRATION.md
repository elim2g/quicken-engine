# QUICKEN Integration Plan -- Master Vertical Slice Blueprint

**Author**: Principal Engineer
**Status**: Canonical (this document supersedes conflicting statements in module plans)
**Date**: 2026-02-12

This is the single source of truth for the vertical slice. When a module plan
contradicts this document, this document wins.

---

## Table of Contents

1. [Shared Data Types](#1-shared-data-types)
2. [Public API Headers](#2-public-api-headers)
3. [Game Loop](#3-game-loop)
4. [Conflicts and Gaps](#4-conflicts-and-gaps)
5. [Module Dependency DAG](#5-module-dependency-dag)
6. [Build System (premake5.lua)](#6-build-system-premake5lua)
7. [Implementation Phases](#7-implementation-phases)
8. [Open Questions](#8-open-questions)

---

## 1. Shared Data Types

Four engineers independently defined types. Below are the canonical definitions,
where they live, and why.

### 1.1 vec3_t -- The Foundation Type

Physics defines `vec3_t` as `{ f32 x, y, z; }`. Gameplay defines the same.
Renderer uses raw `f32[3]` arrays inside its vertex structs but does not define
a `vec3_t`. Netcode does not use `vec3_t` directly (it quantizes to `i16`).

**Decision**: `vec3_t` is a shared engine type. It lives in a new header
`include/qk_math.h`, not inside any module header. Every module that needs a
3D vector includes `qk_math.h`. The renderer may continue using `f32[3]` in
GPU-facing structs (matching Vulkan's layout expectations) but uses `vec3_t`
for any logic it does on the CPU side.

```c
/* include/qk_math.h */

typedef struct { f32 x, y, z; } vec3_t;
typedef struct { vec3_t min, max; } bbox_t;

/* Inline math operations -- defined in the header for inlining */
static inline vec3_t vec3_add(vec3_t a, vec3_t b);
static inline vec3_t vec3_sub(vec3_t a, vec3_t b);
static inline vec3_t vec3_scale(vec3_t v, f32 s);
static inline f32    vec3_dot(vec3_t a, vec3_t b);
static inline vec3_t vec3_cross(vec3_t a, vec3_t b);
static inline f32    vec3_length(vec3_t v);
static inline vec3_t vec3_normalize(vec3_t v);
```

The deterministic `p_sinf`/`p_cosf` and `p_angle_vectors` stay in
`src/physics/p_math.c` (precise-float compilation unit). They are NOT in the
shared math header because they must only be called from precise-float code
paths. The shared `qk_math.h` contains only operations that are safe under both
fast and precise float: add, sub, mul, dot, cross, length, normalize. These
are all composed of additions and multiplications with no reassociation risk
under fast-math (the compiler may reorder additions, but for rendering this is
acceptable; for physics, the precise-float compilation unit protects us).

**Why not put vec3_t in quicken.h?** `quicken.h` is the platform/config header.
Math types get their own header to keep includes granular.

### 1.2 usercmd_t -- Player Input Command

Three modules define input commands independently:

| Module   | Type Name     | Movement Fields                    | Angles            | Buttons        |
|----------|---------------|------------------------------------|--------------------|----------------|
| Physics  | `p_usercmd_t` | `f32 forward_move, side_move, up_move` | `f32 view_angles[3]` | `u32 buttons`  |
| Netcode  | `n_input_t`   | `i8 forward_move, side_move`       | `u16 yaw, pitch`   | `u16 buttons`  |
| Gameplay | `usercmd_t`   | `i8 forward_move, right_move, up_move` | `f32 pitch, yaw`  | `u16 buttons`  |

These are NOT the same thing and should NOT be unified into one struct.
There are two distinct representations:

1. **Wire format** (`n_input_t`): Compact, quantized, sent over the network.
   Owned by netcode. This is serialization, not game logic.

2. **Game-side command** (`qk_usercmd_t`): Full-precision, used by physics and
   gameplay. This is what the server simulation actually consumes.

**Decision**: Define the canonical game-side command as `qk_usercmd_t` in
`include/qk_types.h`. Netcode keeps `n_input_t` as its wire format and
provides conversion functions.

```c
/* include/qk_types.h */

typedef struct {
    u32     server_time;        /* server tick time in ms */

    /* movement intentions: -1.0 to 1.0 (normalized) */
    f32     forward_move;
    f32     side_move;
    f32     up_move;

    /* view angles in degrees (full precision) */
    f32     pitch;
    f32     yaw;

    /* buttons bitmask */
    u32     buttons;

    /* weapon selection (0 = no change, otherwise weapon_id_t) */
    u8      weapon_select;
} qk_usercmd_t;

/* Button flags -- unified across all modules */
#define QK_BUTTON_ATTACK    (1 << 0)
#define QK_BUTTON_JUMP      (1 << 1)
#define QK_BUTTON_CROUCH    (1 << 2)
#define QK_BUTTON_USE       (1 << 3)
```

**Reconciliation notes:**

- Physics plan uses `f32` for movement which is correct -- the normalization
  from `i8` (-127..127) to `f32` (-1..1) happens at the boundary, inside
  netcode's conversion function. Physics never sees `i8`.
- Gameplay plan uses `i8` for movement -- this changes. Gameplay now uses
  the same `qk_usercmd_t` with `f32` movement fields. The `i8` representation
  only exists on the wire.
- Physics plan's `u32 buttons` vs Gameplay/Netcode's `u16 buttons`: Use `u32`.
  Two extra bytes per command is trivial, and we avoid running out of button
  bits. The wire format can still pack to `u16`.
- Gameplay's `sequence` field: removed from the command struct. Sequence numbers
  are a transport concern, tracked by netcode, not by the game command.
- Physics' `view_angles[3]` (including roll): changed to separate `pitch` and
  `yaw`. We do not support roll in player view. If we ever need it (death
  camera), we add it then.

**Netcode conversion:**

```c
/* In src/netcode/n_convert.c */
qk_usercmd_t n_input_to_usercmd(const n_input_t *input, u32 tick);
n_input_t    n_usercmd_to_input(const qk_usercmd_t *cmd);
```

### 1.3 Player State

Physics defines `p_player_state_t` (movement-focused). Gameplay defines
`player_state_t` (combat-focused, embeds `p_player_state_t`-level data plus
health/armor/weapons). Netcode does not define a player state -- it packs
entity state for the wire.

**Decision**: There is one canonical player state, `qk_player_state_t`, that
lives in `include/qk_types.h`. It contains ALL fields needed by physics,
gameplay, and netcode. No separate `p_player_state_t`.

```c
/* include/qk_types.h */

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

typedef struct {
    /* --- Physics fields (authoritative movement state) --- */
    vec3_t      origin;
    vec3_t      velocity;
    vec3_t      mins;           /* bounding box min (changes with crouch) */
    vec3_t      maxs;           /* bounding box max */
    bool        on_ground;
    vec3_t      ground_normal;
    bool        jump_held;      /* for jump edge detection */
    f32         max_speed;      /* PM_MAX_SPEED, can be overridden per-player */
    f32         gravity;        /* PM_GRAVITY, can be overridden per-player */
    u32         command_time;   /* last processed command time in ms */

    /* --- Gameplay fields (combat/identity state) --- */
    u8                          client_num;
    qk_team_t                   team;
    qk_player_alive_state_t     alive_state;

    i16         health;
    i16         armor;

    qk_weapon_id_t  weapon;             /* current weapon */
    qk_weapon_id_t  pending_weapon;     /* weapon being switched to */
    u32             weapon_time;        /* ms until weapon can fire */
    u32             switch_time;        /* ms remaining on switch */
    u16             ammo[QK_WEAPON_COUNT];

    /* view angles (authoritative, from last usercmd) */
    f32         pitch;
    f32         yaw;

    /* stats (per-round) */
    u16         frags;
    u16         deaths;
    u16         damage_given;
    u16         damage_taken;

    /* server bookkeeping */
    u32         respawn_time;
    qk_usercmd_t last_cmd;     /* most recent command from client */
} qk_player_state_t;
```

**Why merge?** Physics needs `origin`, `velocity`, `on_ground`, etc. Gameplay
needs `health`, `weapon`, `ammo`, etc. Both operate on the same entity in the
same tick. Having two separate structs means either:
(a) copying fields back and forth every tick (wasteful, bug-prone), or
(b) giving physics a pointer into the gameplay struct (coupling).
A single struct eliminates both problems. Physics reads/writes the movement
fields. Gameplay reads/writes the combat fields. No overlap except `origin`
and `velocity`, which both read but only physics writes during movement.

### 1.4 Entity State (Netcode Wire Format)

Netcode defines `n_entity_state_t` (22 bytes, quantized). Gameplay defines
`entity_t` with a union of player/projectile data. These serve different
purposes and should remain separate.

**Decision**: `n_entity_state_t` stays in `include/netcode/n_types.h` as the
wire format. It is the netcode engineer's responsibility. The gameplay module
provides a function to pack a game entity into the wire format:

```c
/* Called by netcode during snapshot generation */
void qk_game_pack_entity(const entity_t *ent, n_entity_state_t *out);
```

And the client unpacks it back:

```c
/* Called by client after receiving a snapshot */
void qk_game_unpack_entity(const n_entity_state_t *net, entity_t *out);
```

These functions handle quantization (float -> fixed-point) and dequantization.
They live in `src/gameplay/g_netpack.c`.

### 1.5 Entity (Game Logic)

Gameplay defines `entity_t` with the full tagged-union entity. This struct is
NOT shared across module boundaries. Instead, other modules access entities
through the gameplay API:

```c
/* Physics accesses player state directly via pointer */
qk_player_state_t *qk_game_get_player_state_mut(u8 client_num);

/* Netcode reads entity state through the packing function */
void qk_game_pack_entity(const entity_t *ent, n_entity_state_t *out);
```

The `entity_t` struct and `entity_pool_t` live in `include/gameplay/g_local.h`
(internal to gameplay). Other modules never include this header.

### 1.6 Result Codes

Physics plan does not define result codes. Renderer defines `qk_result_t` with
renderer-specific errors. Netcode defines `qk_net_result_t`. Gameplay defines
`qk_result_t` with different values than the renderer.

**Decision**: One unified `qk_result_t` enum in `include/quicken.h`:

```c
typedef enum {
    QK_SUCCESS = 0,

    /* General errors */
    QK_ERROR_INIT_FAILED,
    QK_ERROR_OUT_OF_MEMORY,
    QK_ERROR_INVALID_PARAM,
    QK_ERROR_NOT_FOUND,
    QK_ERROR_FULL,

    /* Renderer errors */
    QK_ERROR_VULKAN_INIT,
    QK_ERROR_NO_SUITABLE_GPU,
    QK_ERROR_SWAPCHAIN,
    QK_ERROR_PIPELINE,

    /* Netcode errors */
    QK_ERROR_SOCKET,
    QK_ERROR_TIMEOUT,
    QK_ERROR_REJECTED,

    QK_RESULT_COUNT
} qk_result_t;
```

All modules return `qk_result_t`. No per-module result enums.

### 1.7 Trace Result

Physics defines `p_trace_result_t`. Gameplay defines `trace_result_t` with
slightly different fields (adds `hit_entity_id`, `hit_normal` naming).

**Decision**: One trace result type in `include/qk_types.h`:

```c
typedef struct {
    f32     fraction;           /* 0.0 = start in solid, 1.0 = no hit */
    vec3_t  end_pos;            /* final position */
    vec3_t  hit_normal;         /* surface normal at impact */
    f32     hit_dist;           /* distance of the hit plane */
    bool    start_solid;        /* started inside a brush */
    bool    all_solid;          /* entire trace inside solid */
    i32     brush_index;        /* which brush was hit (-1 if none) */
    i32     entity_id;          /* which entity was hit (-1 if none) */
} qk_trace_result_t;
```

Physics populates `brush_index` and leaves `entity_id = -1`. Gameplay's
entity-aware trace wraps the physics trace and additionally checks entity
bounding boxes, populating `entity_id`.

### 1.8 Summary: Shared Type Locations

| Type                    | Header                    | Used By                    |
|-------------------------|---------------------------|----------------------------|
| `vec3_t`, `bbox_t`     | `include/qk_math.h`      | All modules                |
| `qk_result_t`          | `include/quicken.h`       | All modules                |
| `qk_usercmd_t`         | `include/qk_types.h`     | Physics, Gameplay, Netcode |
| `qk_player_state_t`    | `include/qk_types.h`     | Physics, Gameplay, Netcode |
| `qk_trace_result_t`    | `include/qk_types.h`     | Physics, Gameplay          |
| `qk_weapon_id_t`       | `include/qk_types.h`     | Gameplay, Netcode, UI      |
| `qk_team_t`            | `include/qk_types.h`     | Gameplay, Netcode, UI      |
| `n_entity_state_t`     | `include/netcode/n_types.h` | Netcode, Gameplay (pack/unpack) |
| `n_input_t`            | `include/netcode/n_types.h` | Netcode only (wire format) |
| `entity_t`             | `include/gameplay/g_local.h` | Gameplay only (internal)  |

---

## 2. Public API Headers

### 2.1 Header Layout

```
include/
    quicken.h               Platform detection, base types (u8..f64), result codes, assert macro
    qk_math.h               vec3_t, bbox_t, inline math operations
    qk_types.h              Shared game types: usercmd, player state, trace result, enums

    core/
        qk_platform.h       Platform time, sleep, filesystem basics
        qk_input.h          Input sampling API (SDL abstraction)
        qk_window.h         Window creation/management

    physics/
        qk_physics.h        Public physics API (world create, player move, trace)

    renderer/
        qk_renderer.h       Public renderer API (init, frame, world upload, UI quads)

    netcode/
        qk_netcode.h        Public netcode API (server/client lifecycle, tick, interp)
        n_types.h           Netcode-specific types (entity wire format, input wire format)

    gameplay/
        qk_gameplay.h       Public gameplay API (game init/tick, player connect, CA state)
        g_local.h           Internal gameplay types (entity_t, weapon defs) -- NOT public

    ui/
        qk_ui.h             Public UI API (draw HUD, events)
```

### 2.2 Naming Conventions

All public functions use the `qk_` prefix followed by the module name:

| Module   | Prefix               | Example                              |
|----------|----------------------|--------------------------------------|
| Engine   | `qk_engine_`        | `qk_engine_init()`                   |
| Physics  | `qk_physics_`       | `qk_physics_world_create()`          |
| Renderer | `qk_renderer_`      | `qk_renderer_begin_frame()`          |
| Netcode  | `qk_net_`           | `qk_net_server_tick()`               |
| Gameplay | `qk_game_`          | `qk_game_tick()`                     |
| UI       | `qk_ui_`            | `qk_ui_draw_hud()`                   |
| Core     | `qk_platform_`      | `qk_platform_time_now()`             |
|          | `qk_input_`         | `qk_input_poll()`                    |
|          | `qk_window_`        | `qk_window_create()`                 |

**Corrections from module plans:**

- Physics plan uses bare `physics_init()` in the existing stub code. This
  becomes `qk_physics_init()`. The plan's proposed API already uses `qk_physics_`
  prefix -- good.
- Renderer plan uses `qk_renderer_` prefix -- good, keep it.
- Netcode plan uses `qk_net_` prefix -- good, keep it.
- Gameplay plan uses `qk_game_` and `qk_ui_` prefixes -- good, keep it.
- Gameplay's UI draw primitives (`ui_draw_rect`, `ui_draw_text`, etc.) need
  the `qk_` prefix: `qk_ui_draw_rect()`, `qk_ui_draw_text()`, etc.

### 2.3 Error Handling

All public functions that can fail return `qk_result_t`. Functions that cannot
fail return `void` or the requested value. No exceptions. No errno. No global
error state.

### 2.4 Config Structs

All config structs follow the convention: zero-initialization means sensible
defaults.

```c
qk_renderer_config_t config = {0};  /* all defaults */
config.render_width = 1920;         /* override just what you need */
```

### 2.5 Canonical Public APIs

#### include/qk_physics.h

```c
#ifndef QK_PHYSICS_H
#define QK_PHYSICS_H

#include "quicken.h"
#include "qk_math.h"
#include "qk_types.h"

/* Opaque world handle */
typedef struct qk_phys_world qk_phys_world_t;

/* Collision model (provided by map loader) */
typedef struct {
    vec3_t  normal;
    f32     dist;
} qk_plane_t;

typedef struct {
    qk_plane_t *planes;
    u32          plane_count;
    vec3_t       mins;
    vec3_t       maxs;
} qk_brush_t;

typedef struct {
    qk_brush_t *brushes;
    u32          brush_count;
} qk_collision_model_t;

/* Physics time state */
typedef struct {
    f32     accumulator;
    u32     tick_count;
} qk_phys_time_t;

/* Lifecycle */
qk_phys_world_t *qk_physics_world_create(qk_collision_model_t *cm);
void              qk_physics_world_destroy(qk_phys_world_t *world);

/* Player init */
void qk_physics_player_init(qk_player_state_t *ps, vec3_t spawn_origin);

/* Run one physics tick (fixed timestep, called from game tick) */
void qk_physics_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                      const qk_phys_world_t *world);

/* Fixed-timestep wrapper (accumulates real time, runs fixed ticks) */
void qk_physics_update(qk_phys_time_t *ts, f32 frame_dt,
                        qk_player_state_t *ps, const qk_usercmd_t *cmd,
                        const qk_phys_world_t *world);

/* Trace a box through the world */
qk_trace_result_t qk_physics_trace(const qk_phys_world_t *world,
                                     vec3_t start, vec3_t end,
                                     vec3_t mins, vec3_t maxs);

/* Get interpolation alpha for rendering */
f32 qk_physics_get_alpha(const qk_phys_time_t *ts);

/* Constants */
#define QK_TICK_RATE        128
#define QK_TICK_DT          (1.0f / 128.0f)

#endif /* QK_PHYSICS_H */
```

#### include/qk_renderer.h

Kept as specified in the renderer plan, with these corrections:
- Remove the `qk_result_t` redefinition (use the one from `quicken.h`).
- Add `#include "qk_math.h"` and `#include "qk_types.h"`.
- Add the higher-level UI draw functions that gameplay expects.

```c
#ifndef QK_RENDERER_H
#define QK_RENDERER_H

#include "quicken.h"
#include "qk_math.h"

/* Configuration */
typedef struct {
    void    *sdl_window;
    u32      render_width;      /* 0 = default (1920) */
    u32      render_height;     /* 0 = default (1080) */
    u32      window_width;
    u32      window_height;
    bool     aspect_fit;
    bool     vsync;
} qk_renderer_config_t;

/* Camera */
typedef struct {
    f32     view_projection[16];    /* column-major 4x4 */
    f32     position[3];
} qk_camera_t;

/* World vertex (produced by map loader, consumed by renderer) */
typedef struct {
    f32     position[3];
    f32     normal[3];
    f32     uv[2];
    u32     texture_id;
} qk_world_vertex_t;

/* Surface draw info */
typedef struct {
    u32     index_offset;
    u32     index_count;
    u32     vertex_offset;
    u32     texture_index;
} qk_draw_surface_t;

/* UI quad (low-level, used by UI module internally) */
typedef struct {
    f32     x, y, w, h;
    f32     u0, v0, u1, v1;
    u32     color;
    u32     texture_id;
} qk_ui_quad_t;

typedef u32 qk_texture_id_t;

/* GPU stats */
typedef struct {
    f64     gpu_frame_ms;
    f64     world_pass_ms;
    f64     ui_pass_ms;
    f64     compose_pass_ms;
    u32     draw_calls;
    u32     triangles;
} qk_gpu_stats_t;

/* Lifecycle */
qk_result_t qk_renderer_init(const qk_renderer_config_t *config);
void        qk_renderer_shutdown(void);

/* Resolution */
void qk_renderer_set_render_resolution(u32 width, u32 height);
void qk_renderer_set_aspect_mode(bool aspect_fit);
void qk_renderer_handle_window_resize(u32 new_width, u32 new_height);

/* Resource upload (map load) */
qk_result_t qk_renderer_upload_world(
    const qk_world_vertex_t *vertices, u32 vertex_count,
    const u32 *indices, u32 index_count,
    const qk_draw_surface_t *surfaces, u32 surface_count);
qk_texture_id_t qk_renderer_upload_texture(
    const u8 *pixels, u32 width, u32 height, u32 channels);
void qk_renderer_free_world(void);

/* Frame rendering */
void qk_renderer_begin_frame(const qk_camera_t *camera);
void qk_renderer_draw_world(void);
void qk_renderer_push_ui_quad(const qk_ui_quad_t *quad);
void qk_renderer_end_frame(void);

/* Debug */
void qk_renderer_get_stats(qk_gpu_stats_t *out_stats);

/* High-level UI drawing (convenience functions built on push_ui_quad) */
void qk_ui_draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color_rgba);
void qk_ui_draw_text(f32 x, f32 y, const char *text, f32 size,
                      u32 color_rgba);
void qk_ui_draw_number(f32 x, f32 y, i32 value, f32 size, u32 color_rgba);
f32  qk_ui_text_width(const char *text, f32 size);

#endif /* QK_RENDERER_H */
```

**Note on UI draw functions**: Gameplay's plan expected functions like
`ui_draw_rect()`, `ui_draw_text()`, etc. These are now `qk_ui_draw_rect()`,
`qk_ui_draw_text()`, etc. They live in `src/ui/ui_draw.c` which calls into
`qk_renderer_push_ui_quad()` internally. They are declared in
`include/renderer/qk_renderer.h` because they operate on the renderer's UI
layer. The UI module (`src/ui/`) contains the HUD logic (what to draw), while
these functions provide the mechanism (how to draw a rect/text).

#### include/netcode/qk_netcode.h

Kept as specified in the netcode plan, with these corrections:
- Use `qk_result_t` from `quicken.h` instead of `qk_net_result_t`.
- Input functions accept/return `qk_usercmd_t`, with internal conversion
  to/from `n_input_t` for the wire.

```c
#ifndef QK_NETCODE_H
#define QK_NETCODE_H

#include "quicken.h"
#include "qk_types.h"
#include "netcode/n_types.h"

/* Server config */
typedef struct {
    u16     server_port;        /* 0 = don't bind */
    u32     max_clients;        /* up to 16 */
    f64     tick_rate;          /* 0 = default (128.0) */
} qk_net_server_config_t;

/* Client config */
typedef struct {
    f64     interp_delay;       /* 0 = default (0.020) */
} qk_net_client_config_t;

/* Connection state */
typedef enum {
    QK_CONN_DISCONNECTED,
    QK_CONN_CONNECTING,
    QK_CONN_CONNECTED,
    QK_CONN_DISCONNECTING
} qk_conn_state_t;

/* Interpolated entity (what the renderer sees) */
typedef struct {
    f32     pos_x, pos_y, pos_z;
    f32     vel_x, vel_y, vel_z;
    f32     yaw, pitch;
    u8      entity_type;
    u8      flags;
    u8      health;
    u8      armor;
    u8      weapon;
    u8      ammo;
    bool    active;
} qk_interp_entity_t;

#define QK_MAX_ENTITIES     256

typedef struct {
    qk_interp_entity_t entities[QK_MAX_ENTITIES];
} qk_interp_state_t;

/* Server API */
qk_result_t qk_net_server_init(const qk_net_server_config_t *config);
void        qk_net_server_tick(void);
void        qk_net_server_shutdown(void);
u32         qk_net_server_get_tick(void);
u32         qk_net_server_client_count(void);

void        qk_net_server_set_entity(u8 entity_id,
                                      const n_entity_state_t *state);
void        qk_net_server_remove_entity(u8 entity_id);
bool        qk_net_server_get_input(u8 client_id, qk_usercmd_t *out_cmd);

/* Client API */
qk_result_t qk_net_client_init(const qk_net_client_config_t *config);
qk_result_t qk_net_client_connect_remote(const char *address, u16 port);
qk_result_t qk_net_client_connect_local(void);
void        qk_net_client_disconnect(void);
void        qk_net_client_tick(void);
void        qk_net_client_interpolate(f64 render_time);
void        qk_net_client_shutdown(void);

void        qk_net_client_send_input(const qk_usercmd_t *cmd);
const qk_interp_state_t *qk_net_client_get_interp_state(void);

qk_conn_state_t qk_net_client_get_state(void);
i32             qk_net_client_get_rtt(void);
u8              qk_net_client_get_id(void);

#endif /* QK_NETCODE_H */
```

#### include/gameplay/qk_gameplay.h

```c
#ifndef QK_GAMEPLAY_H
#define QK_GAMEPLAY_H

#include "quicken.h"
#include "qk_types.h"

/* Forward declaration */
typedef struct qk_phys_world qk_phys_world_t;

/* Game config */
typedef struct {
    u8      max_players;            /* 0 = default (16) */
    u8      rounds_to_win;          /* 0 = default (10) */
    u32     round_time_limit_ms;    /* 0 = default (120000) */
    u32     countdown_time_ms;      /* 0 = default (5000) */
} qk_game_config_t;

/* Clan Arena state (read-only for UI) */
typedef struct {
    u8      state;              /* round_state_t enum */
    u32     state_timer_ms;
    u8      score_alpha;
    u8      score_beta;
    u8      round_number;
    u8      alive_alpha;
    u8      alive_beta;
} qk_ca_state_t;

/* Opaque game state */
typedef struct qk_game_state qk_game_state_t;

/* Lifecycle */
qk_result_t         qk_game_init(const qk_game_config_t *config);
void                qk_game_tick(qk_phys_world_t *world, f32 dt);
void                qk_game_shutdown(void);

/* Player management */
qk_result_t         qk_game_player_connect(u8 client_num,
                                             const char *name, qk_team_t team);
void                qk_game_player_disconnect(u8 client_num);
void                qk_game_player_command(u8 client_num,
                                            const qk_usercmd_t *cmd);

/* State queries */
const qk_player_state_t *qk_game_get_player_state(u8 client_num);
qk_player_state_t       *qk_game_get_player_state_mut(u8 client_num);
const qk_ca_state_t     *qk_game_get_ca_state(void);
qk_game_state_t         *qk_game_get_state(void);

/* Entity packing for netcode */
void qk_game_pack_entity(u8 entity_id, n_entity_state_t *out);
u32  qk_game_get_entity_count(void);

#endif /* QK_GAMEPLAY_H */
```

#### include/ui/qk_ui.h

```c
#ifndef QK_UI_H
#define QK_UI_H

#include "quicken.h"
#include "qk_types.h"

/* Forward declarations */
typedef struct qk_ca_state qk_ca_state_t;

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
```

---

## 3. Game Loop

### 3.1 Tick Rate Resolution

**The conflict**: Gameplay says 125 Hz. Physics and netcode say 128 Hz.

**Decision: 128 Hz.** Rationale:

1. 128 is a power of two. This makes bitwise modular arithmetic trivial for
   ring buffers (index & 127 instead of index % 125). Every snapshot history
   buffer, input queue, and tick counter benefits.

2. 128 Hz gives 7.8125 ms per tick. 125 Hz gives 8.0 ms per tick. The
   difference is negligible for gameplay feel, but the power-of-two alignment
   is a genuine engineering advantage.

3. Physics and netcode already agree on 128 Hz. Gameplay is the outlier and
   cited "classic Quake rate" as the reason for 125. Q3 actually ran at
   various rates (sv_fps was configurable, 20 was default, competitive
   servers used 30 or 40, and the "125 fps" was a client-side physics
   exploit, not a server tick rate). We are not bound by Q3's defaults.

4. Networking at 128 Hz is slightly more bandwidth than 125 Hz (+2.4%) which
   is immaterial.

**All modules now use `QK_TICK_RATE = 128` and `QK_TICK_DT = 1.0f / 128.0f`.**

The gameplay plan's `SV_TICK_RATE`, `SV_TICK_DT`, `SV_TICK_DT_MS` constants
are replaced by the unified constants in `include/qk_types.h`:

```c
#define QK_TICK_RATE        128
#define QK_TICK_DT          (1.0f / 128.0f)
#define QK_TICK_DT_MS_NOM   8       /* nominal ms per tick (actual is 7.8125) */
```

### 3.2 Authoritative Game Tick (Server Side)

The server tick is the heartbeat of the simulation. Called exactly once per
128 Hz fixed timestep:

```c
void qk_server_tick(qk_phys_world_t *phys_world) {
    /* 1. Read inputs from all connected clients */
    for (u8 i = 0; i < QK_MAX_CLIENTS; i++) {
        qk_usercmd_t cmd;
        if (qk_net_server_get_input(i, &cmd)) {
            qk_game_player_command(i, &cmd);
        }
    }

    /* 2. Run gameplay tick (mode logic, weapons, combat) */
    qk_game_tick(phys_world, QK_TICK_DT);
    /*
     * Inside qk_game_tick:
     *   a. g_ca_tick()              -- round state machine
     *   b. g_process_commands()     -- weapon switch, view angles
     *   c. For each alive player:
     *        qk_physics_move(ps, &ps->last_cmd, phys_world)
     *   d. g_projectile_tick()      -- move projectiles, check collisions
     *   e. g_combat_resolve()       -- apply queued damage events
     *   f. g_ca_check_end()         -- check win conditions
     */

    /* 3. Pack entity states for netcode snapshot */
    for (u32 i = 0; i < qk_game_get_entity_count(); i++) {
        n_entity_state_t net_state;
        qk_game_pack_entity((u8)i, &net_state);
        qk_net_server_set_entity((u8)i, &net_state);
    }

    /* 4. Netcode broadcasts snapshots to all clients */
    qk_net_server_tick();
}
```

**Key detail**: Physics movement (`qk_physics_move`) is called from INSIDE
`qk_game_tick`, not as a separate step. The gameplay module orchestrates the
tick order. Physics is a library that gameplay calls, not an independent system
that runs on its own schedule.

### 3.3 Local Play (Loopback)

```
main_loop {
    real_dt = measure_elapsed_time();

    /* ---- Server-side (runs in same process) ---- */
    server_accumulator += real_dt;
    while (server_accumulator >= QK_TICK_DT) {
        qk_server_tick(phys_world);
        server_accumulator -= QK_TICK_DT;
    }

    /* ---- Client-side ---- */

    /* 1. Poll OS input (SDL events) */
    qk_input_poll(&input_state);

    /* 2. Build usercmd from raw input */
    qk_usercmd_t cmd = qk_input_build_usercmd(&input_state, server_time);

    /* 3. Send input to server (via loopback -- instant delivery) */
    qk_net_client_send_input(&cmd);

    /* 4. Client tick (processes received snapshots) */
    client_accumulator += real_dt;
    while (client_accumulator >= QK_TICK_DT) {
        qk_net_client_tick();
        client_accumulator -= QK_TICK_DT;
    }

    /* 5. Interpolate for rendering */
    f64 render_time = qk_net_client_render_time();
    qk_net_client_interpolate(render_time);

    /* 6. Build camera from interpolated local player state */
    const qk_interp_state_t *interp = qk_net_client_get_interp_state();
    qk_camera_t camera = build_camera_from_interp(interp, local_client_id);

    /* 7. Render */
    qk_renderer_begin_frame(&camera);
    qk_renderer_draw_world();

    /* 8. HUD (reads player state + CA state) */
    const qk_player_state_t *ps = qk_game_get_player_state(local_client_id);
    const qk_ca_state_t *ca = qk_game_get_ca_state();
    qk_ui_draw_hud(ps, ca, render_width, render_height);
    qk_ui_tick((u32)(real_dt * 1000.0f));

    /* 9. Present */
    qk_renderer_end_frame();
}
```

In local play, the server tick and client tick run in the same loop iteration.
Because loopback transport delivers packets instantly, the client always has
fresh snapshots. Interpolation delay still applies (the client renders ~20ms
in the past) to keep the code path identical to networked play.

### 3.4 Networked Play

**Server process** (dedicated or listen):

```
server_main_loop {
    real_dt = measure_elapsed_time();

    server_accumulator += real_dt;
    while (server_accumulator >= QK_TICK_DT) {
        qk_server_tick(phys_world);
        server_accumulator -= QK_TICK_DT;
    }
}
```

**Client process**:

```
client_main_loop {
    real_dt = measure_elapsed_time();

    /* 1. Poll input */
    qk_input_poll(&input_state);

    /* 2. Build and send usercmd */
    qk_usercmd_t cmd = qk_input_build_usercmd(&input_state, estimated_server_time);
    qk_net_client_send_input(&cmd);

    /* 3. Client tick (receive snapshots from network) */
    client_accumulator += real_dt;
    while (client_accumulator >= QK_TICK_DT) {
        qk_net_client_tick();
        client_accumulator -= QK_TICK_DT;
    }

    /* 4. Interpolate */
    f64 render_time = qk_net_client_render_time();
    qk_net_client_interpolate(render_time);

    /* 5. Build camera + render */
    const qk_interp_state_t *interp = qk_net_client_get_interp_state();
    qk_camera_t camera = build_camera_from_interp(interp, local_client_id);

    qk_renderer_begin_frame(&camera);
    qk_renderer_draw_world();

    /* HUD reads from interpolated state (no local game state on pure client) */
    /* For vertical slice without prediction, the client reconstructs a
       qk_player_state_t from the interp state for HUD display. */
    qk_player_state_t hud_ps = reconstruct_player_state_from_interp(interp, local_id);
    qk_ca_state_t hud_ca = ...; /* received via reliable channel from server */
    qk_ui_draw_hud(&hud_ps, &hud_ca, render_width, render_height);
    qk_ui_tick((u32)(real_dt * 1000.0f));

    qk_renderer_end_frame();
}
```

**Listen server** is the hybrid: one client slot is loopback, others are UDP.
The server and the listen-server client share a process. The listen-server
client gets zero-latency loopback while remote clients go through UDP.

### 3.5 Frame Budget

At 1000 fps target, total frame time budget is 1.0 ms.

| Component           | Budget   | Notes                                  |
|---------------------|----------|----------------------------------------|
| Input polling       | 0.01 ms  | SDL_PollEvent, trivial                 |
| Server tick (when)  | 0.05 ms  | 128 Hz amortized, ~6.4 ticks/frame at 1000fps = 0 or 1 tick |
| Client tick          | 0.01 ms  | Snapshot reassembly, mostly memcpy     |
| Interpolation        | 0.02 ms  | Linear lerp over active entities       |
| Camera build         | 0.001 ms | Matrix construction                    |
| Renderer begin+world | 0.3 ms   | Command buffer recording               |
| UI draw              | 0.1 ms   | Quad generation                        |
| Renderer end+present | 0.1 ms   | Submit + present (async with GPU)      |
| **Total CPU**        | **~0.6 ms** |                                     |

Physics ticks consume ~0.05ms per tick (trace against ~1500 brushes with AABB
broadphase). At 128 Hz and 1000 fps, physics runs ~0.128 ticks per frame on
average (about 1 tick every 8 frames). This is negligible.

---

## 4. Conflicts and Gaps

### 4.1 Renderer Vertex Format vs Map Loader Output

The renderer expects `qk_world_vertex_t` with position (3 floats), normal
(3 floats), UV (2 floats), and texture_id (1 u32) = 36 bytes.

**Gap**: Nobody owns the map loader. The renderer plan says "the engine core
parses .map files and produces brush geometry." The physics plan says "the
engine core provides us with a flat array of brushes." Neither plan defines
who writes the .map parser or how it produces BOTH collision brushes (planes)
for physics AND triangle-soup vertices for rendering.

**Resolution**: The map loader is a new module (`src/core/map_loader.c`) owned
by the Principal Engineer (me) or assigned to whichever engineer has bandwidth
first. It reads .map files and produces:

1. `qk_collision_model_t` (array of `qk_brush_t`) for physics
2. Arrays of `qk_world_vertex_t` + `u32` indices + `qk_draw_surface_t` for renderer
3. Texture references (names) for the asset pipeline to resolve

The map loader performs CSG (Constructive Solid Geometry) to compute brush
intersections, generates face polygons, triangulates them, computes UV
coordinates from Quake's texture projection data, and computes face normals.

This is a significant piece of work. See Phase 3 in the implementation timeline.

### 4.2 Netcode entity_state_t vs Gameplay Entity Output

Netcode's `n_entity_state_t` has:
- `pos_x/y/z` as `i16` (fixed-point 13.3)
- `vel_x/y/z` as `i16` (1 unit/sec precision)
- `yaw/pitch` as `u16` (0..65535 -> 0..360)
- `health/armor/weapon/ammo` as `u8`

Gameplay's entity has:
- `origin` as `vec3_t` (f32)
- `velocity` as `vec3_t` (f32)
- `angles` as `vec3_t` (f32)
- `health/armor` as `i16`
- weapon enum, ammo as `u16`

**Status**: These are intentionally different representations. The pack/unpack
functions in `src/gameplay/g_netpack.c` handle conversion. The quantization
ranges are sufficient:
- Position: +/-4096 units at 0.125 precision -- fine for any arena map.
- Velocity: +/-32768 at 1 unit/sec -- max strafejump speed is ~800 u/s,
  rocket knockback peaks at ~1500 u/s. Sufficient.
- Health 0..255: gameplay uses `i16` (can go negative on overkill) but net
  clamps to 0..255 for transmission. The negative overkill value is only needed
  server-side for damage tracking, never transmitted.
- Armor 0..255: CA spawns with 200, max is 200. Fits.

**One issue**: Gameplay's `ammo` is `u16[WEAPON_COUNT]` (per-weapon) but
netcode's entity state only has a single `u8 ammo` field for current weapon.
The client needs to know ammo for ALL weapons (for the HUD weapon bar in the
future). For the vertical slice, sending only current-weapon ammo is acceptable
since the HUD only shows current weapon ammo. Post-vertical-slice, add a
per-player reliable message for full ammo state, or expand the entity state.

### 4.3 Physics usercmd_t vs Gameplay's usercmd_t

**Resolved** in Section 1.2. One canonical `qk_usercmd_t` used by both.

Physics reads: `forward_move`, `side_move`, `pitch`, `yaw`, `buttons`
(specifically `QK_BUTTON_JUMP`).

Gameplay reads: `buttons` (attack, weapon select), `weapon_select`, `pitch`,
`yaw`.

No conflict. Both read from the same struct.

### 4.4 Renderer UI API vs Gameplay HUD Expectations

Gameplay expects:
- `ui_draw_rect(x, y, w, h, color)`
- `ui_draw_text(x, y, text, size, color)`
- `ui_draw_number(x, y, value, size, color)`
- `ui_draw_icon(x, y, size, icon_id)`
- `ui_draw_rect_outline(x, y, w, h, color, thickness)`
- `ui_text_width(text, size)` -> `f32`

Renderer provides:
- `qk_renderer_push_ui_quad(quad)` -- low-level, one textured quad at a time.

**Gap**: The renderer does not provide text rendering, number rendering, icon
rendering, or outline rectangles directly. These are higher-level operations
that compose multiple quads.

**Resolution**: The functions `qk_ui_draw_rect`, `qk_ui_draw_text`,
`qk_ui_draw_number`, `qk_ui_text_width` are implemented in `src/ui/ui_draw.c`
and declared in `include/renderer/qk_renderer.h` (alongside the low-level quad
API). They internally call `qk_renderer_push_ui_quad()`.

`qk_ui_draw_text` requires a bitmap font atlas loaded as a texture. The font
atlas is loaded at init time, and `ui_draw.c` maintains a static glyph table
mapping ASCII codes to UV rects.

`qk_ui_draw_icon` requires an icon atlas texture. For the vertical slice, we
need at minimum: health icon, armor icon, and weapon icons (RL, RG, LG). These
can be part of the font atlas or a separate icon atlas.

`qk_ui_draw_rect_outline` is implemented as 4 thin `qk_ui_draw_rect` calls.

The gameplay plan's `renderer_spawn_rail_trail`, `renderer_spawn_explosion`,
`renderer_spawn_beam` (visual effects in world space) are OUT OF SCOPE for the
vertical slice. Weapons will fire and deal damage, but visual effects will be
minimal (no particle systems, no trail rendering). Add a stub that does nothing.

### 4.5 Missing Modules

| Module          | Status                                                    |
|-----------------|-----------------------------------------------------------|
| **Map loader**  | NOT owned by any plan. Needed by Phase 3. See 4.1.       |
| **Asset pipeline** | NOT owned. Need WAD texture extraction for Quake maps. |
| **Audio**       | NOT covered by any plan. Not in vertical slice scope.     |
| **Core/platform** | Partially implied. Need: time, input polling, window.  |
| **Font/text**   | NOT owned. Need bitmap font renderer for HUD text.       |

**Assignments** (see Phase details in Section 7):

- **Map loader**: Principal Engineer, with support from Physics Engineer for
  collision model format requirements.
- **Asset pipeline (textures)**: Principal Engineer. For the vertical slice,
  this is a minimal WAD parser or hardcoded test textures.
- **Audio**: Deferred. Not in vertical slice.
- **Core/platform**: Principal Engineer. This is `qk_platform_time_now()`,
  `qk_input_poll()`, `qk_window_create()` -- thin wrappers over SDL3.
- **Font/text**: Renderer Engineer provides the bitmap font loading mechanism.
  A pre-made bitmap font atlas (e.g., a fixed-width font rasterized offline) is
  checked into `assets/fonts/`.

### 4.6 Float Precision Agreement

| Module                | Plan Says          | Correct?                        |
|-----------------------|--------------------|---------------------------------|
| `src/physics/`        | Precise            | Yes                             |
| `src/renderer/`       | Fast               | Yes                             |
| `src/core/`           | Precise            | Yes                             |
| `src/` (main)         | Precise            | Yes                             |
| `src/netcode/`        | Precise            | Yes (clock sync, quantization)  |
| `src/gameplay/`       | Precise            | Yes (damage calc, combat)       |
| `src/ui/`             | Precise            | Yes (part of main exe)          |

**All modules agree.** Only the renderer uses fast float.

**Missing from premake5.lua**: The physics plan correctly identifies that
`-ffp-contract=off` is needed for GCC/Clang to prevent FMA contraction. This
must be added to ALL precise-float compilation units (physics, netcode,
gameplay, core, main).

---

## 5. Module Dependency DAG

```
                    quicken.h
                    qk_math.h
                    qk_types.h
                        |
          +-------------+-------------+
          |             |             |
          v             v             v
    qk_physics     qk_netcode    qk_renderer
          |             |             |
          |             |             |  (no cross-deps between these three)
          |             |             |
          +------+------+             |
                 |                    |
                 v                    |
            qk_gameplay               |
                 |                    |
                 +----+---------------+
                      |
                      v
                    qk_ui
                      |
                      v
                quicken (exe)
                      |
                      v
                  core (input, window, platform, map loader)
```

**Dependency rules** (strictly enforced):

1. `qk_physics` depends on: `quicken.h`, `qk_math.h`, `qk_types.h`. Nothing else.
2. `qk_renderer` depends on: `quicken.h`, `qk_math.h`, SDL3 headers, Vulkan headers. Does NOT depend on physics, netcode, or gameplay.
3. `qk_netcode` depends on: `quicken.h`, `qk_types.h`. Does NOT depend on physics, renderer, or gameplay directly. It receives entity states through function pointers or the gameplay API.
4. `qk_gameplay` depends on: `quicken.h`, `qk_math.h`, `qk_types.h`, `qk_physics.h` (calls `qk_physics_move`), `netcode/n_types.h` (for `n_entity_state_t` packing).
5. `qk_ui` depends on: `quicken.h`, `qk_types.h`, `qk_renderer.h` (calls `qk_ui_draw_rect` etc.), `qk_gameplay.h` (reads `qk_ca_state_t`).
6. `quicken` (exe) depends on everything. It is the composition root.

**No cycles.** Physics never includes renderer, netcode, or gameplay. Renderer
never includes physics, netcode, or gameplay. Netcode never includes renderer
or physics. Gameplay calls into physics but physics does not call into gameplay.

**Enforcement**: Each static library's premake `includedirs` is restricted to
only the headers it needs. If a source file in `qk_physics` tries to
`#include "renderer/qk_renderer.h"`, the build fails.

---

## 6. Build System (premake5.lua)

### 6.1 Full Target Structure

```
Workspace: QUICKEN
    |
    +-- quicken-physics      (StaticLib, precise float)
    +-- quicken-renderer     (StaticLib, fast float)
    +-- quicken-netcode      (StaticLib, precise float)
    +-- quicken              (ConsoleApp, precise float)
    |    Links: quicken-physics, quicken-renderer, quicken-netcode,
    |           SDL3, vulkan-1/vulkan, ws2_32 (Win), pthread (Linux)
    |    Files: src/*.c, src/core/**, src/gameplay/**, src/ui/**
    |
    +-- quicken-server       (ConsoleApp, precise float, headless)
         Links: quicken-physics, quicken-netcode,
                ws2_32 (Win), pthread (Linux)
         Files: src/server_main.c, src/core/**, src/gameplay/**
         Defines: QK_HEADLESS
         Does NOT link: quicken-renderer, SDL3, vulkan
```

### Two executables: client and dedicated server

The workspace produces two binaries:

1. **`quicken`** -- Full client. Opens a window, renders, connects to a server
   (local or remote). For local play it creates a loopback server internally.
2. **`quicken-server`** -- Headless dedicated server. No window, no renderer,
   no SDL3, no Vulkan. Runs the game simulation and netcode only. Also useful
   for headless physics testing.

The `QK_HEADLESS` define gates all renderer/SDL3/UI code:

```c
// In gameplay code that touches rendering:
#ifndef QK_HEADLESS
    qk_ui_draw_hud(&player_state);
#endif

// In core platform code:
#ifdef QK_HEADLESS
    // Use raw OS timers (QueryPerformanceCounter / clock_gettime)
    // instead of SDL3 for timing
#else
    // Use SDL3 for window, input, and timing
#endif
```

The headless server shares the same gameplay and physics code as the client.
This ensures the authoritative simulation is identical in both binaries. The
server binary simply has no renderer, no UI, and no SDL3 dependency.

**`src/server_main.c`** is the dedicated server entry point. It:
1. Initializes the arena allocator
2. Loads the map (collision model only, no render geometry)
3. Initializes physics and netcode
4. Runs the server tick loop (receive inputs → game tick → broadcast snapshots)
5. Has no frame rate cap — ticks as fast as the tick rate demands (128Hz)

**Note**: Gameplay and UI compile as part of the main executable, NOT as
separate static libraries. Rationale:

1. They use precise float, same as the main exe. No compilation boundary needed.
2. They depend on each other and on multiple other modules. Splitting them into
   separate libs adds build complexity with no benefit.
3. They are not independently reusable. They exist for this game only.

The headless server includes gameplay (`src/gameplay/**`) but NOT UI
(`src/ui/**`). UI code is gated behind `#ifndef QK_HEADLESS` where needed.

Netcode IS a separate static library because:
1. It has a clean interface boundary (transport abstraction).
2. It needs platform-specific link libraries (ws2_32).
3. It can be tested independently (loopback test without renderer).
4. It is linked by BOTH the client and the dedicated server.

### 6.2 premake5.lua Changes

```lua
-- QUICKEN Engine Build Configuration

workspace "QUICKEN"
    architecture "x86_64"
    configurations { "Debug", "Release", "RelWithDebInfo" }
    startproject "quicken"

    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

    filter "configurations:Debug"
        defines { "QUICKEN_DEBUG" }
        runtime "Debug"
        symbols "On"
        optimize "Off"

    filter "configurations:Release"
        defines { "QUICKEN_RELEASE", "NDEBUG" }
        runtime "Release"
        symbols "Off"
        optimize "Speed"

    filter "configurations:RelWithDebInfo"
        defines { "QUICKEN_RELEASE", "NDEBUG" }
        runtime "Release"
        symbols "On"
        optimize "Speed"

    filter {}

--------------------------------------------------------------
-- Physics (precise float, cross-platform determinism)
--------------------------------------------------------------
project "quicken-physics"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/physics")

    files {
        "src/physics/**.c",
        "include/qk_physics.h",
        "include/qk_math.h",
        "include/qk_types.h"
    }

    includedirs {
        "include"
    }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"     -- REQUIRED for determinism
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Renderer (fast float, aggressive optimizations)
--------------------------------------------------------------
project "quicken-renderer"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/renderer")

    files {
        "src/renderer/**.c",
        "include/renderer/**.h"
    }

    includedirs {
        "include",
        "external/SDL3/include"
    }

    filter "system:windows"
        includedirs { "$(VULKAN_SDK)/Include" }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-ffast-math",
            "-std=c11"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:fast"
        }

    filter {}

--------------------------------------------------------------
-- Netcode (precise float, platform sockets)
--------------------------------------------------------------
project "quicken-netcode"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/netcode")

    files {
        "src/netcode/**.c",
        "include/netcode/**.h"
    }

    includedirs {
        "include"
    }

    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Main executable
--------------------------------------------------------------
project "quicken"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/main")

    files {
        "src/*.c",
        "src/core/**.c",
        "src/gameplay/**.c",
        "src/ui/**.c",
        "include/**.h"
    }

    includedirs {
        "include",
        "external/SDL3/include"
    }

    links {
        "quicken-physics",
        "quicken-renderer",
        "quicken-netcode"
    }

    -- Platform: Windows
    filter "system:windows"
        system "windows"
        libdirs {
            "external/SDL3/build/Release",
            "$(VULKAN_SDK)/Lib"
        }
        links { "SDL3", "vulkan-1", "ws2_32" }
        postbuildcommands {
            "{MKDIR} %{cfg.targetdir}",
            "{COPY} external/SDL3/build/Release/SDL3.dll %{cfg.targetdir}"
        }

    -- Platform: Linux
    filter "system:linux"
        system "linux"
        links { "SDL3", "vulkan", "m", "pthread" }
        libdirs { "external/SDL3/build-linux" }
        runpathdirs { "external/SDL3/build-linux" }

    -- Compiler: GCC/Clang (precise float for game logic)
    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    -- Compiler: MSVC (precise float for game logic)
    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}

--------------------------------------------------------------
-- Dedicated server (headless, no renderer/SDL3/Vulkan)
--------------------------------------------------------------
project "quicken-server"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/server")

    defines { "QK_HEADLESS" }

    files {
        "src/server_main.c",
        "src/core/**.c",
        "src/gameplay/**.c",
        "include/**.h"
    }

    removefiles {
        "src/core/qk_window.c",
        "src/core/qk_input.c"
    }

    includedirs {
        "include"
    }

    links {
        "quicken-physics",
        "quicken-netcode"
    }

    -- Platform: Windows
    filter "system:windows"
        system "windows"
        links { "ws2_32" }

    -- Platform: Linux
    filter "system:linux"
        system "linux"
        links { "m", "pthread" }

    -- Compiler: GCC/Clang
    filter "toolset:gcc or toolset:clang"
        buildoptions {
            "-Wall", "-Wextra", "-Wpedantic",
            "-march=native",
            "-std=c11",
            "-ffp-contract=off"
        }

    -- Compiler: MSVC
    filter "toolset:msc"
        buildoptions {
            "/W4",
            "/arch:AVX2",
            "/fp:precise"
        }

    filter {}
```

---

## 7. Implementation Phases

Each phase produces a testable artifact. Phases are ordered by dependency:
later phases build on earlier ones. Where phases have no dependency on each
other, engineers work in parallel.

### Phase 1: Foundation (Week 1)

**Goal**: Build system compiles. Shared types exist. Basic SDL3 window opens.

**Work items**:
1. Write `include/quicken.h` (update with unified `qk_result_t`).
2. Write `include/qk_math.h` (vec3_t, bbox_t, inline math).
3. Write `include/qk_types.h` (usercmd, player state, trace result, enums, constants).
4. Update `premake5.lua` to the full structure above.
5. Write `src/core/qk_platform.c` -- `qk_platform_time_now()` (monotonic clock via SDL3 or `QueryPerformanceCounter`/`clock_gettime`).
6. Write `src/core/qk_window.c` -- SDL3 window creation.
7. Write `src/core/qk_input.c` -- SDL3 event polling, raw input state, mouse delta.
8. Update `src/main.c` -- open window, run empty loop, measure frame time.

**Owner**: Principal Engineer
**Test**: Window opens. Frame time is measured. Build succeeds on both Windows and Linux.

### Phase 2: Renderer -- Colored Screen (Week 2)

**Goal**: Vulkan initialized, solid color rendered to screen.

**Work items**:
1. `r_vulkan.c` -- Instance, physical device, logical device, swapchain.
2. `r_commands.c` -- Command pool, command buffer, frame sync objects.
3. Clear swapchain image to a solid color each frame and present.
4. Handle window resize (swapchain recreation).

**Owner**: Renderer Engineer
**Dependency**: Phase 1 (SDL3 window handle)
**Test**: Window shows a solid color. Resizing works. Frame rate is uncapped.

### Phase 3: Renderer -- Composition + Offscreen Targets (Week 3)

**Goal**: Two offscreen render targets composed to swapchain via fullscreen triangle.

**Work items**:
1. `r_memory.c` -- Bump allocator for GPU memory.
2. `r_texture.c` -- Image/view creation helpers.
3. Create world and UI offscreen render targets.
4. `r_pipeline.c` -- Shader module loading, pipeline cache.
5. `r_compose.c` -- Composition pipeline, fullscreen triangle, descriptor sets.
6. Write and compile composition shaders (compose.vert, compose.frag).

**Owner**: Renderer Engineer
**Dependency**: Phase 2
**Test**: Window shows composed offscreen targets (different clear colors). Render resolution is independent of window size. Aspect fit works.

### Phase 4: Physics -- Math + Trace (Weeks 2-3, parallel with Phases 2-3)

**Goal**: Deterministic math verified. Trace against hardcoded room works.

**Work items**:
1. `src/physics/p_math.c` -- vec3 operations, deterministic `p_sinf`/`p_cosf`, `p_angle_vectors`.
2. Add `-ffp-contract=off` to premake (already done in Phase 1).
3. Cross-platform math verification test.
4. `src/physics/p_brush.c` -- Brush AABB computation.
5. `src/physics/p_trace.c` -- `p_trace_brush`, `p_trace_world` (brute force).
6. Hardcode a test room (6 brushes). Verify trace results.

**Owner**: Physics Engineer
**Dependency**: Phase 1 (shared types)
**Test**: Automated test: trace rays in a box room, verify fraction and normal. Cross-platform identical results.

### Phase 5: Physics -- Movement (Week 4)

**Goal**: Player can run, jump, and strafejump in a hardcoded room.

**Work items**:
1. `p_accel.c` -- `p_accelerate`, `p_air_accelerate`.
2. `p_slide.c` -- `p_clip_velocity`, `p_slide_move`, `p_step_slide_move`.
3. `p_move.c` -- `p_categorize_position`, `p_check_jump`, `p_apply_friction`, `p_move`.
4. `p_time.c` -- Fixed timestep accumulator.
5. `physics.c` -- Public API wrappers.
6. Strafejump verification: synthetic input sequence, measure speed gain.

**Owner**: Physics Engineer
**Dependency**: Phase 4
**Test**: Player spawns, falls, lands, walks, jumps, strafejumps. Speed matches Q3 expectations.

### Phase 6: Renderer -- World Triangles (Week 4, parallel with Phase 5)

**Goal**: 3D triangle rendering with camera and depth.

**Work items**:
1. `r_world.c` -- World pipeline, vertex format, UBO for camera.
2. Upload a hardcoded cube or room (matching physics test room).
3. Render to world offscreen target, compose to swapchain.
4. Camera controlled by keyboard/mouse (temporary, before physics integration).
5. Write and compile world shaders (world.vert, world.frag).

**Owner**: Renderer Engineer
**Dependency**: Phase 3
**Test**: A colored cube or room rendered on screen. Camera moves with WASD+mouse.

### Phase 7: Map Loader + Integrated Renderer + Physics (Week 5-6)

**Goal**: Load a .map file. See it rendered. Walk around in it with physics.

**Work items**:
1. `src/core/map_loader.c` -- Parse Quake .map format (or a subset).
   - Read brush definitions (planes from three points per face).
   - Compute brush plane equations.
   - Build `qk_collision_model_t` for physics.
   - Compute face polygons via plane-plane intersections and clipping.
   - Triangulate faces.
   - Compute UV coordinates from Quake texture projection data.
   - Build `qk_world_vertex_t` arrays + indices + `qk_draw_surface_t` for renderer.
2. `r_texture.c` -- Texture upload, mipmap generation.
3. Minimal texture loading (solid colors or WAD parser -- see Open Questions).
4. Wire physics world + renderer world from the same map data.
5. Player spawns in map, moves with physics, camera follows player.

**Owner**: Principal Engineer (map loader), Renderer Engineer (texture upload + world rendering from real data), Physics Engineer (physics world from real collision data)
**Dependency**: Phases 5 and 6
**Test**: A real Quake .map file loaded, rendered, and walkable. Strafejumping works.

### Phase 8: Netcode -- Foundation (Weeks 4-6, parallel with Phases 5-7)

**Goal**: Loopback transport works. Packets encode/decode correctly.

**Work items**:
1. `n_platform.c` -- Socket abstraction.
2. `n_transport.c` -- Loopback ring buffer + UDP transport.
3. `n_compress.c` -- Bitpacker (write/read bits). Round-trip tests.
4. `n_protocol.c` -- Packet header encode/decode, message framing.
5. `n_channel.c` -- Reliable channel with retransmit.

**Owner**: Netcode Engineer
**Dependency**: Phase 1 (shared types)
**Test**: Loopback: send/receive bytes. Bitpacker: round-trip bit patterns. Reliable channel: delivery over simulated loss.

### Phase 9: Netcode -- Server + Client (Weeks 6-7)

**Goal**: Client connects to server via loopback. Server ticks. Snapshots flow.

**Work items**:
1. `n_server.c` -- Server init, tick loop, client slot management, handshake.
2. `n_snapshot.c` -- Snapshot capture + delta encode/decode.
3. `n_client.c` -- Client connect, snapshot receive, interp buffer.
4. `n_clock.c` -- Clock sync (fast convergence over loopback).
5. Client input -> server, server -> snapshot -> client -> interpolation.

**Owner**: Netcode Engineer
**Dependency**: Phase 8
**Test**: Loopback: client connects, server accepts. Client sends input, server echoes entity state in snapshot. Client interpolates. Verify with debug prints.

### Phase 10: Gameplay -- Foundation (Weeks 5-7, parallel with Phases 7-9)

**Goal**: Entity system, player spawn, weapons fire, damage applies.

**Work items**:
1. `g_entity.c` -- Entity pool: alloc, free, iterate.
2. `g_player.c` -- Player spawn (CA), armor absorption.
3. `g_event.c` -- Event queue.
4. `g_weapons.c` -- Weapon defs table, weapon tick, fire dispatch.
5. `g_combat.c` -- Damage pipeline, hitscan trace, splash damage, kill.
6. `g_projectile.c` -- Projectile spawn, tick, collision.
7. `g_ca.c` -- Clan Arena state machine (all round states).
8. `g_main.c` -- `qk_game_init`, `qk_game_tick`, `qk_game_shutdown`.
9. `g_netpack.c` -- Entity packing/unpacking for netcode.

**Owner**: Gameplay Engineer
**Dependency**: Phase 1 (shared types), Phase 4 (physics trace, for combat traces)
**Test**: Unit tests for each subsystem. Full CA round: warmup -> countdown -> playing -> one team dies -> round end -> next round -> match end.

### Phase 11: Integration -- Full Loop with Loopback (Week 8)

**Goal**: Single player plays in a Quake map with movement, weapons, and netcode loopback.

**Work items**:
1. Wire `main.c` game loop as specified in Section 3.3.
2. Server creates physics world from map loader.
3. Server runs gameplay ticks with physics.
4. Client sends input via loopback, receives snapshots, interpolates.
5. Renderer draws interpolated world + player.
6. Basic debug visualization (player position, velocity, FPS counter).

**Owner**: Principal Engineer (integration), all engineers (debugging their modules)
**Dependency**: Phases 7, 9, 10
**Test**: Player loads into a Quake map, moves around, strafejumps. Rockets fire and explode. Rails hit. LG damages. It feels like Quake.

### Phase 12: HUD + UI (Weeks 8-9, parallel with Phase 11 debugging)

**Goal**: Full HUD: health, armor, ammo, timer, scores, crosshair, killfeed.

**Work items**:
1. `src/ui/ui_draw.c` -- Implement `qk_ui_draw_rect`, `qk_ui_draw_text`, `qk_ui_draw_number`, `qk_ui_text_width`.
2. Create/integrate bitmap font atlas (checked into `assets/fonts/`).
3. `src/ui/ui_hud.c` -- Main HUD draw function.
4. `src/ui/ui_crosshair.c` -- Crosshair.
5. `src/ui/ui_hitmarker.c` -- Hit marker.
6. `src/ui/ui_killfeed.c` -- Kill feed.
7. `src/ui/ui_scoreboard.c` -- Scoreboard overlay.

**Owner**: Gameplay Engineer (HUD logic), Renderer Engineer (font atlas loading, UI quad batching)
**Dependency**: Phase 6 (UI pipeline working), Phase 10 (gameplay state to display)
**Test**: Full HUD visible during gameplay. Numbers update when damaged. Killfeed shows kills. Scoreboard shows scores.

### Phase 13: Networked Play (Weeks 9-10)

**Goal**: Two clients connect to a server over UDP and play.

**Work items**:
1. Netcode: UDP transport tested between two processes.
2. Dedicated server mode (server-only process, no renderer).
3. Listen server mode (one loopback client + UDP clients).
4. Test with artificial latency (50ms, 100ms) to verify interpolation.
5. Verify clock sync convergence.

**Owner**: Netcode Engineer
**Dependency**: Phase 11
**Test**: Two players in the same map, shooting each other. 128 Hz server. Interpolation is smooth at 50ms latency.

### Phase 14: Polish + Performance (Week 10-11)

**Goal**: Sub-1ms frame times. No crashes. Clean startup/shutdown.

**Work items**:
1. `r_debug.c` -- GPU timestamp queries, stats overlay.
2. Pipeline cache save/load.
3. Profile and optimize: draw call batching, trace performance.
4. Verify mailbox present mode.
5. Memory leak audit (Vulkan validation, manual review).
6. Clean error handling on all init paths.
7. Cross-platform build verification (Windows MSVC + Linux GCC).
8. Cross-platform determinism test (record inputs, diff output).

**Owner**: All engineers
**Dependency**: Phase 13
**Test**: 1000+ fps on high-end hardware with a Quake map. No validation errors. No crashes on startup/shutdown. Deterministic physics across platforms.

### Phase Summary Timeline

```
Week  1:  [Phase 1: Foundation]
Week  2:  [Phase 2: Vulkan boot]   [Phase 4: Physics math+trace]
Week  3:  [Phase 3: Composition]   [Phase 4 cont.]   [Phase 8: Netcode foundation]
Week  4:  [Phase 5: Movement]      [Phase 6: World render]   [Phase 8 cont.]
Week  5:  [Phase 7: Map loader]    [Phase 9: Net server/client]   [Phase 10: Gameplay]
Week  6:  [Phase 7 cont.]          [Phase 9 cont.]                [Phase 10 cont.]
Week  7:  [Phase 7 cont.]          [Phase 9 cont.]                [Phase 10 cont.]
Week  8:  [Phase 11: Integration]  [Phase 12: HUD]
Week  9:  [Phase 11 cont.]         [Phase 12 cont.]    [Phase 13: Networked play]
Week 10:  [Phase 13 cont.]         [Phase 14: Polish]
Week 11:  [Phase 14 cont.]
```

**Engineer assignments summary**:

| Engineer          | Primary Phases        | Support Phases    |
|-------------------|-----------------------|-------------------|
| Principal         | 1, 7, 11             | 14                |
| Renderer          | 2, 3, 6, 12 (font)   | 7, 14             |
| Physics           | 4, 5                  | 7, 14             |
| Netcode           | 8, 9, 13             | 11, 14            |
| Gameplay          | 10, 12               | 11, 14            |

---

## 8. Decisions (Resolved)

All questions answered by project owner. These are final.

### 8.1 Map Format and Parser

**Decision**: Quake 3 .map format (id Tech 3). Supports Bezier patches and
entity definitions. Shader/material system is deferred — for the vertical
slice, texture references in the .map are used only to generate solid colors.
TrenchBroom is the expected mapping tool.

### 8.2 Asset Pipeline for Textures

**Decision**: Generate a solid color per texture name (hash the name to a
deterministic color). This unblocks development with zero asset dependencies.
Real texture loading (loose files or pk3 archives) is post-vertical-slice.

### 8.3 Audio

**Decision**: Audio is NOT in scope for the vertical slice. Deferred entirely.

### 8.4 Spawn Points

**Decision**: The map loader extracts `info_player_deathmatch` entities from
the .map file and outputs a list of spawn points:

```c
typedef struct {
    vec3_t  origin;
    f32     yaw;
} qk_spawn_point_t;
```

The gameplay module uses these for player spawning in Clan Arena.

### 8.5 Player Bounding Box

**Decision**: Use Quake 3 dimensions. Define as constants in `qk_types.h`:

```c
#define QK_PLAYER_MINS_X    (-15.0f)
#define QK_PLAYER_MINS_Y    (-15.0f)
#define QK_PLAYER_MINS_Z    (-24.0f)
#define QK_PLAYER_MAXS_X    (15.0f)
#define QK_PLAYER_MAXS_Y    (15.0f)
#define QK_PLAYER_MAXS_Z    (32.0f)
```

### 8.6 Coordinate System

**Decision**: Use Vulkan's native coordinate convention throughout the entire
engine for consistency. Y-up in world space, Y-down in clip space (Vulkan
handles this). All game code, physics, and gameplay use the same convention.
No per-module coordinate system conversions. Quake .map coordinates are
converted once at map load time.

### 8.7 Entity Rendering (Player Models, Projectiles)

**Decision**: For the vertical slice:
- Render players as **capsules** matching their hitbox dimensions (following
  Quake 4 / QuakeLive convention, not Q3 boxes). The renderer needs a
  `qk_renderer_draw_capsule()` function.
- Render projectiles as **spheres**.
- The renderer needs a `qk_renderer_draw_sphere()` function.

These are temporary debug visuals. Post-vertical-slice, add a dynamic geometry
pipeline with proper mesh loading.

### 8.8 Existing Stub Code

**Decision**: Replace all existing unprefixed stubs (`physics_init`,
`renderer_init`) wholesale with `qk_`-prefixed APIs. Delete or rewrite the
current .h and .c files. No backward compatibility concern — this is not a
general-purpose engine.

### 8.9 Thread Model

**Decision**: Single-threaded main loop for the vertical slice. The renderer
submits GPU work asynchronously (Vulkan's async nature handles this), but all
CPU work (input, physics, gameplay, netcode, command buffer recording) happens
on one thread. Multi-threading is post-vertical-slice.

### 8.10 Memory Allocation Strategy

**Decision**: Implement basic arena allocation from the start. This is core
architecture that should not be deferred. No `malloc`/`free` in hot paths.

- **Per-system arenas**: Each major system (physics, renderer, netcode,
  gameplay) gets its own arena, allocated once at init.
- **Per-frame scratch arena**: A single scratch arena reset every frame for
  transient data (command building, temporary buffers).
- **Renderer GPU memory**: Bump allocator per Vulkan memory type (as specified
  in renderer plan).
- **Entity pool**: Fixed-size array within gameplay's arena.
- **Netcode buffers**: Fixed-size within netcode's arena.
- **Snapshot history**: Ring buffer within netcode's arena.

Arena allocator lives in `include/qk_arena.h` and `src/core/qk_arena.c`.

---

## Appendix A: Master Header Include Graph

```
quicken.h  (u8..f64, platform detect, assert, result_t)
    |
    +-- qk_math.h  (vec3_t, bbox_t, inline math ops)
    |
    +-- qk_types.h  (usercmd_t, player_state_t, trace_result_t, weapon/team enums, tick constants)
    |       |
    |       +-- qk_math.h  (for vec3_t)
    |
    +-- core/
    |       qk_platform.h  (time, sleep)
    |       qk_input.h  (input polling, input_state_t)
    |       qk_window.h  (window create/destroy)
    |
    +-- physics/
    |       qk_physics.h  (world, move, trace)
    |           |-- quicken.h
    |           |-- qk_math.h
    |           +-- qk_types.h
    |
    +-- renderer/
    |       qk_renderer.h  (init, frame, world upload, UI quads, UI draw helpers)
    |           |-- quicken.h
    |           +-- qk_math.h
    |
    +-- netcode/
    |       qk_netcode.h  (server/client lifecycle, tick, interp)
    |       |   |-- quicken.h
    |       |   |-- qk_types.h
    |       |   +-- netcode/n_types.h
    |       |
    |       n_types.h  (n_entity_state_t, n_input_t, netcode constants)
    |           +-- quicken.h
    |
    +-- gameplay/
    |       qk_gameplay.h  (game init/tick, player management)
    |       |   |-- quicken.h
    |       |   +-- qk_types.h
    |       |
    |       g_local.h  (entity_t, entity_pool_t, weapon defs -- INTERNAL ONLY)
    |           |-- quicken.h
    |           |-- qk_math.h
    |           +-- qk_types.h
    |
    +-- ui/
            qk_ui.h  (HUD draw, events)
                |-- quicken.h
                +-- qk_types.h
```

## Appendix B: Constant Reconciliation

All timing/sizing constants, unified:

```c
/* include/qk_types.h -- authoritative constants */

/* Tick rate (ALL modules use this) */
#define QK_TICK_RATE            128
#define QK_TICK_DT              (1.0f / 128.0f)
#define QK_TICK_DT_F64          (1.0 / 128.0)

/* Entity limits */
#define QK_MAX_ENTITIES         256
#define QK_MAX_PLAYERS          16

/* Physics constants */
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
#define QK_TRACE_EPSILON        0.03125f

/* Player bounding box */
#define QK_PLAYER_MINS          ((vec3_t){-15.0f, -15.0f, -24.0f})
#define QK_PLAYER_MAXS          ((vec3_t){ 15.0f,  15.0f,  32.0f})

/* Button flags */
#define QK_BUTTON_ATTACK        (1 << 0)
#define QK_BUTTON_JUMP          (1 << 1)
#define QK_BUTTON_CROUCH        (1 << 2)
#define QK_BUTTON_USE           (1 << 3)

/* Clan Arena */
#define QK_CA_ROUNDS_TO_WIN     10
#define QK_CA_COUNTDOWN_MS      5000
#define QK_CA_ROUND_TIME_MS     120000
#define QK_CA_ROUND_END_MS      3000
#define QK_CA_SPAWN_HEALTH      200
#define QK_CA_SPAWN_ARMOR       200
```

Constants that previously had different prefixes (`PM_`, `SV_`, `N_`, bare
names) are now uniformly prefixed with `QK_` when they appear in shared headers.
Module-internal constants (e.g., renderer's `R_MAX_SWAPCHAIN_IMAGES`,
netcode's `N_TRANSPORT_MTU`) keep their module prefix and live in internal
headers only.

---

*End of Integration Plan.*
