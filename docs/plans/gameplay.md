# Gameplay Implementation Plan -- Clan Arena Vertical Slice

**Owner**: Gameplay Engineer
**Modules**: `src/gameplay/`, `src/ui/`, `include/gameplay/`, `include/ui/`
**Float mode**: Precise (`/fp:precise`, no `-ffast-math`)

This document is an implementation plan, not a high-level overview. It contains
exact struct definitions, function signatures, state machines, weapon stat
tables, and the call order for every frame. Another engineer (or AI agent) should
be able to implement every file from this plan alone.

---

## Table of Contents

1. [Game Loop Structure](#1-game-loop-structure)
2. [Entity System](#2-entity-system)
3. [Player State](#3-player-state)
4. [Weapon System](#4-weapon-system)
5. [Clan Arena Mode Logic](#5-clan-arena-mode-logic)
6. [Damage and Combat](#6-damage-and-combat)
7. [Input Processing](#7-input-processing)
8. [HUD Rendering](#8-hud-rendering)
9. [Data Structures and Interfaces](#9-data-structures-and-interfaces)
10. [Build Integration](#10-build-integration)
11. [File Manifest](#11-file-manifest)
12. [Implementation Order](#12-implementation-order)

---

## 1. Game Loop Structure

### Overview

QUICKEN uses a client/server model. Even in single-player, the client connects
to a local server. The server is authoritative: it runs gameplay logic at a
fixed tick rate. The client samples input, sends commands, receives snapshots,
interpolates, and renders.

### Tick Rates

| Clock               | Rate           | Delta                |
|----------------------|----------------|----------------------|
| Server tick          | 125 Hz         | 8 ms                 |
| Client input sample  | Unlocked (fps) | variable             |
| Client render        | Unlocked       | variable             |

The server tick rate of 125 Hz is the classic Quake rate and gives good
tradeoff between precision and bandwidth.

### Frame Execution Order (Server)

```
sv_frame(dt):
    1. sv_receive_commands()       -- read usercmd_t from all clients
    2. sv_tick_accumulator += dt
    3. while sv_tick_accumulator >= SV_TICK_DT:
        a. qk_game_tick(world, SV_TICK_DT)
            i.   g_mode_tick(gs, dt)           -- round state machine
            ii.  g_process_commands(gs)         -- apply usercmds to players
            iii. physics_simulate(world, dt)    -- move players + projectiles
            iv.  g_projectile_tick(gs, dt)      -- check projectile collisions
            v.   g_combat_resolve(gs)           -- apply queued damage
            vi.  g_mode_check_end(gs)           -- check win conditions
        b. sv_tick_accumulator -= SV_TICK_DT
    4. sv_build_snapshot(world)
    5. sv_send_snapshots()         -- send to all clients
```

### Frame Execution Order (Client)

```
cl_frame(dt):
    1. cl_poll_input()             -- SDL events -> raw input state
    2. cl_build_usercmd()          -- raw input -> usercmd_t
    3. cl_send_command(cmd)        -- send to server (netcode)
    4. cl_receive_snapshot()       -- get server state (netcode)
    5. cl_interpolate(dt)          -- interpolate between snapshots
    6. renderer_begin_frame()
    7. renderer_draw_world(cl.view)
    8. qk_ui_draw_hud(&cl.predicted_ps)
    9. renderer_end_frame()
```

### Key Function Signatures

```c
/* Called from main loop. dt is real elapsed wall time in seconds. */
void qk_game_tick(qk_world_t *world, f32 dt);

/* Server builds a snapshot of the world after each tick batch. */
void sv_build_snapshot(const qk_world_t *world);

/* Client constructs a usercmd from current input state. */
usercmd_t cl_build_usercmd(const input_state_t *input, u32 server_time);
```

### Fixed Timestep Accumulator

The server uses a fixed timestep accumulator. `qk_game_tick` is always called
with exactly `SV_TICK_DT` (0.008f seconds). This is critical for determinism
across client prediction and server simulation.

```c
#define SV_TICK_RATE  125
#define SV_TICK_DT    (1.0f / SV_TICK_RATE)   /* 0.008f */
```

---

## 2. Entity System

### Design

No ECS. Entities are a flat array of tagged structs. The entity type determines
which fields are valid. Maximum entities are bounded at compile time. This is
cache-friendly (linear iteration) and dead simple.

### Entity Types

```c
typedef enum {
    ENT_NONE = 0,
    ENT_PLAYER,
    ENT_PROJECTILE,
    ENT_TRIGGER,        /* future: trigger_hurt, teleporter, etc. */
    ENT_TYPE_COUNT
} entity_type_t;
```

### Entity Struct

```c
#define MAX_ENTITIES    256
#define MAX_PLAYERS     16

typedef struct {
    entity_type_t   type;           /* ENT_NONE = free slot */
    u32             id;             /* unique monotonic id */
    u32             owner_id;       /* who spawned this (for projectiles) */

    /* spatial */
    vec3_t          origin;
    vec3_t          velocity;
    vec3_t          angles;         /* pitch, yaw, roll */
    bbox_t          bounds;         /* axis-aligned bounding box */

    /* type-specific (union keeps the struct tight) */
    union {
        struct {
            player_state_t  ps;
            u8              client_num;
        } player;

        struct {
            weapon_id_t     weapon;
            f32             lifetime;       /* seconds remaining */
            u16             damage;
            f32             splash_radius;
            f32             splash_damage;
            f32             knockback;
        } projectile;
    } u;

    /* flags */
    u32             flags;
    bool            active;
} entity_t;
```

### Entity Storage

```c
typedef struct {
    entity_t    entities[MAX_ENTITIES];
    u32         num_entities;       /* high water mark */
    u32         next_id;            /* monotonic counter */

    /* fast access: player entity indices (indexed by client_num) */
    i32         player_entity[MAX_PLAYERS];  /* -1 = none */
} entity_pool_t;
```

### Entity Operations

```c
entity_t *ent_alloc(entity_pool_t *pool, entity_type_t type);
void      ent_free(entity_pool_t *pool, entity_t *ent);
entity_t *ent_find(entity_pool_t *pool, u32 id);

/* iterate all active entities of a given type */
/* usage: for (entity_t *e = ent_first(pool, ENT_PLAYER); e; e = ent_next(pool, e, ENT_PLAYER)) */
entity_t *ent_first(entity_pool_t *pool, entity_type_t type);
entity_t *ent_next(entity_pool_t *pool, entity_t *after, entity_type_t type);
```

`ent_alloc` scans for the first slot with `type == ENT_NONE`, sets the type,
assigns `next_id++`, marks `active = true`, and returns a pointer.
`ent_free` zeroes the slot and sets `type = ENT_NONE`.

---

## 3. Player State

### player_state_t

This is the authoritative state for one player. It lives inside the player
entity. It is also what the server sends to clients in snapshots, and what the
HUD reads from.

```c
typedef enum {
    TEAM_NONE = 0,
    TEAM_ALPHA,
    TEAM_BETA,
    TEAM_SPECTATOR
} team_t;

typedef enum {
    WEAPON_NONE = 0,
    WEAPON_ROCKET,
    WEAPON_RAIL,
    WEAPON_LG,
    WEAPON_COUNT
} weapon_id_t;

typedef enum {
    PSTATE_ALIVE = 0,
    PSTATE_DEAD,
    PSTATE_SPECTATING
} player_alive_state_t;

typedef struct {
    /* identity */
    u8                      client_num;
    team_t                  team;
    char                    name[32];

    /* vitals */
    i16                     health;         /* signed: can go negative on overkill */
    i16                     armor;

    /* combat state */
    player_alive_state_t    alive_state;
    weapon_id_t             weapon;         /* currently selected weapon */
    weapon_id_t             pending_weapon; /* weapon being switched to, WEAPON_NONE if none */
    u32                     weapon_time;    /* ms until weapon can fire again */
    u32                     switch_time;    /* ms remaining on weapon switch */

    /* ammo (indexed by weapon_id_t) */
    u16                     ammo[WEAPON_COUNT];

    /* stats (per-round, reset between rounds) */
    u16                     frags;
    u16                     deaths;
    u16                     damage_given;
    u16                     damage_taken;

    /* input */
    usercmd_t               last_cmd;       /* most recent usercmd from client */

    /* server bookkeeping */
    u32                     respawn_time;   /* server time when this player last spawned */
} player_state_t;
```

### Spawn Values (Clan Arena)

When a player spawns in Clan Arena, the following values are set:

```c
void g_player_spawn_ca(entity_t *ent, vec3_t spawn_origin, vec3_t spawn_angles) {
    player_state_t *ps = &ent->u.player.ps;

    ent->origin = spawn_origin;
    ent->angles = spawn_angles;
    ent->velocity = (vec3_t){0, 0, 0};

    ps->health       = 200;
    ps->armor         = 200;
    ps->alive_state   = PSTATE_ALIVE;
    ps->weapon        = WEAPON_ROCKET;
    ps->pending_weapon = WEAPON_NONE;
    ps->weapon_time   = 0;
    ps->switch_time   = 0;

    /* all weapons, full ammo */
    ps->ammo[WEAPON_ROCKET] = 25;
    ps->ammo[WEAPON_RAIL]   = 10;
    ps->ammo[WEAPON_LG]     = 150;

    ps->frags         = 0;
    ps->deaths        = 0;
    ps->damage_given  = 0;
    ps->damage_taken  = 0;
}
```

### Armor Absorption

Quake-style tiered armor. In CA, everyone spawns with Red Armor behavior:

```
absorbed = damage * 0.66
actual_armor_damage = min(absorbed, ps->armor)
actual_health_damage = damage - actual_armor_damage
```

```c
void g_player_apply_armor(player_state_t *ps, i16 raw_damage,
                          i16 *out_health_dmg, i16 *out_armor_dmg) {
    f32 absorb = (f32)raw_damage * 0.66f;
    i16 armor_dmg = (i16)absorb;
    if (armor_dmg > ps->armor) {
        armor_dmg = ps->armor;
    }
    *out_armor_dmg  = armor_dmg;
    *out_health_dmg = raw_damage - armor_dmg;
}
```

---

## 4. Weapon System

### Design

Weapons are data-driven: a static table of `g_weapon_def_t` defines stats. The
weapon system reads this table, never hardcodes per-weapon behavior in branches.
The only behavioral split is the fire function, which differs between hitscan
and projectile weapons.

### Weapon Definitions Table

```c
typedef enum {
    FIRE_HITSCAN,
    FIRE_PROJECTILE,
    FIRE_BEAM           /* continuous hitscan (LG) */
} fire_mode_t;

typedef struct {
    const char     *name;
    weapon_id_t     id;
    fire_mode_t     fire_mode;

    /* timing */
    u16             fire_interval_ms;   /* min time between shots */
    u16             switch_time_ms;     /* weapon raise time after switch */

    /* damage */
    u16             damage;             /* per hit (or per tick for beam) */
    f32             splash_radius;      /* 0 = no splash */
    f32             splash_damage_max;  /* damage at epicenter */
    f32             self_damage_mult;   /* 0.0 = no self damage */

    /* physics */
    f32             knockback;          /* knockback force multiplier */
    f32             self_knockback;     /* self-knockback multiplier (rocket jump) */
    f32             projectile_speed;   /* units/sec; 0 = hitscan */
    f32             projectile_lifetime;/* seconds before expire; 0 = hitscan */

    /* ammo */
    u16             ammo_per_shot;
    u16             max_ammo;

    /* hitscan */
    f32             range;              /* max hitscan range; 0 = infinite */
} g_weapon_def_t;
```

### Weapon Stats (Vertical Slice)

These are Quake Live-inspired values, tuned for competitive CA play.

| Stat                  | Rocket Launcher    | Railgun            | Lightning Gun      |
|-----------------------|--------------------|--------------------|---------------------|
| fire_mode             | FIRE_PROJECTILE    | FIRE_HITSCAN       | FIRE_BEAM           |
| fire_interval_ms      | 800                | 1500               | 50                  |
| switch_time_ms        | 50                 | 50                 | 50                  |
| damage                | 100                | 80                 | 7                   |
| splash_radius         | 120.0              | 0.0                | 0.0                 |
| splash_damage_max     | 100.0              | 0.0                | 0.0                 |
| self_damage_mult      | 0.5                | 0.0                | 0.0                 |
| knockback             | 1.0                | 1.0                | 0.04                |
| self_knockback        | 1.2                | 0.0                | 0.0                 |
| projectile_speed      | 1000.0             | 0.0                | 0.0                 |
| projectile_lifetime   | 10.0               | 0.0                | 0.0                 |
| ammo_per_shot         | 1                  | 1                  | 1                   |
| max_ammo              | 25                 | 10                 | 150                 |
| range                 | 0.0 (N/A)         | 8192.0             | 768.0               |

```c
static const g_weapon_def_t g_weapon_defs[WEAPON_COUNT] = {
    [WEAPON_NONE] = {0},    /* sentinel */

    [WEAPON_ROCKET] = {
        .name               = "Rocket Launcher",
        .id                 = WEAPON_ROCKET,
        .fire_mode          = FIRE_PROJECTILE,
        .fire_interval_ms   = 800,
        .switch_time_ms     = 50,
        .damage             = 100,
        .splash_radius      = 120.0f,
        .splash_damage_max  = 100.0f,
        .self_damage_mult   = 0.5f,
        .knockback          = 1.0f,
        .self_knockback     = 1.2f,
        .projectile_speed   = 1000.0f,
        .projectile_lifetime = 10.0f,
        .ammo_per_shot      = 1,
        .max_ammo           = 25,
        .range              = 0.0f,
    },

    [WEAPON_RAIL] = {
        .name               = "Railgun",
        .id                 = WEAPON_RAIL,
        .fire_mode          = FIRE_HITSCAN,
        .fire_interval_ms   = 1500,
        .switch_time_ms     = 50,
        .damage             = 80,
        .splash_radius      = 0.0f,
        .splash_damage_max  = 0.0f,
        .self_damage_mult   = 0.0f,
        .knockback          = 1.0f,
        .self_knockback     = 0.0f,
        .projectile_speed   = 0.0f,
        .projectile_lifetime = 0.0f,
        .ammo_per_shot      = 1,
        .max_ammo           = 10,
        .range              = 8192.0f,
    },

    [WEAPON_LG] = {
        .name               = "Lightning Gun",
        .id                 = WEAPON_LG,
        .fire_mode          = FIRE_BEAM,
        .fire_interval_ms   = 50,
        .switch_time_ms     = 50,
        .damage             = 7,
        .splash_radius      = 0.0f,
        .splash_damage_max  = 0.0f,
        .self_damage_mult   = 0.0f,
        .knockback          = 0.04f,
        .self_knockback     = 0.0f,
        .projectile_speed   = 0.0f,
        .projectile_lifetime = 0.0f,
        .ammo_per_shot      = 1,
        .max_ammo           = 150,
        .range              = 768.0f,
    },
};
```

### Weapon State Machine (Per-Player)

```
                   +----------+
                   |  IDLE    |  weapon_time == 0, no switch pending
                   +----+-----+
                        |
            +-----------+-----------+
            |                       |
    [+attack pressed]       [weapon switch requested]
            |                       |
            v                       v
       +---------+           +----------+
       | FIRING  |           | SWITCHING|
       +---------+           +----------+
       weapon_time =         switch_time =
       fire_interval_ms      switch_time_ms
            |                       |
            |                       | switch_time reaches 0
            |                       v
            |                 weapon = pending_weapon
            |                 pending_weapon = WEAPON_NONE
            |                 -> IDLE
            |
            | weapon_time reaches 0 -> IDLE
```

### Firing Logic

```c
/* called once per server tick per player */
void g_weapon_tick(entity_pool_t *pool, entity_t *player_ent, u32 tick_dt_ms);

/* attempt to fire the current weapon. returns true if fired. */
bool g_weapon_fire(entity_pool_t *pool, entity_t *player_ent);

/* initiate weapon switch */
void g_weapon_switch(entity_t *player_ent, weapon_id_t new_weapon);
```

`g_weapon_tick` does the following each tick:

1. If `switch_time > 0`: decrement by `tick_dt_ms`. If it reaches 0, finalize
   the switch (`weapon = pending_weapon`, `pending_weapon = WEAPON_NONE`).
   Return (cannot fire while switching).
2. If `weapon_time > 0`: decrement by `tick_dt_ms`. Return.
3. If `weapon_time == 0` and `last_cmd.buttons & BTN_ATTACK`:
   call `g_weapon_fire`.

`g_weapon_fire`:

1. Check ammo. If `ammo[weapon] < ammo_per_shot`, return false.
2. Subtract ammo.
3. Set `weapon_time = fire_interval_ms`.
4. Dispatch based on `fire_mode`:
   - `FIRE_HITSCAN`: call `g_combat_hitscan_trace(...)`.
   - `FIRE_PROJECTILE`: call `g_projectile_spawn(...)`.
   - `FIRE_BEAM`: call `g_combat_beam_trace(...)`.
5. Return true.

### Projectile Spawning

```c
entity_t *g_projectile_spawn(entity_pool_t *pool,
                             entity_t *owner,
                             weapon_id_t weapon,
                             vec3_t origin,
                             vec3_t direction);
```

This allocates an `ENT_PROJECTILE` entity with:
- `origin` = player eye position + small forward offset (avoid self-collision)
- `velocity` = `direction * projectile_speed`
- `owner_id` = player entity id
- `projectile.weapon` = weapon
- `projectile.lifetime` = `projectile_lifetime`
- `projectile.damage` = `damage`
- `projectile.splash_radius` = `splash_radius`
- `projectile.splash_damage` = `splash_damage_max`
- `projectile.knockback` = `knockback`
- `bounds` = small box (e.g. 4x4x4 units for rocket)

---

## 5. Clan Arena Mode Logic

### Round State Machine

```
    MATCH START
        |
        v
  +----------+     all players        +-----------+    countdown      +---------+
  | WARMUP   | --  readied up  --->   | COUNTDOWN | -- timer == 0 -->| PLAYING |
  +----------+  (or admin force)      +-----------+    (5 seconds)   +----+----+
       ^                                                                  |
       |                                                    one team eliminated
       |                                                    or round timer expires
       |                                                                  |
       |                                                                  v
       |          round < 10          +------------+
       +-----  (both teams alive) ---| ROUND_END  |
       |                              +-----+------+
       |                                    |
       |                          score == rounds_to_win (10)
       |                                    |
       |                                    v
       |                            +-----------+
       +---  restart / new match ---| MATCH_END |
                                    +-----------+
```

### State Enum and Struct

```c
typedef enum {
    ROUND_WARMUP = 0,
    ROUND_COUNTDOWN,
    ROUND_PLAYING,
    ROUND_END,
    MATCH_END
} round_state_t;

#define ROUNDS_TO_WIN       10
#define COUNTDOWN_TIME_MS   5000
#define ROUND_TIME_LIMIT_MS 120000      /* 2 minute round timer */
#define ROUND_END_DELAY_MS  3000        /* pause after round before next */

typedef struct {
    round_state_t   state;
    u32             state_timer_ms;     /* ms remaining in current state */

    u8              score_alpha;
    u8              score_beta;
    u8              round_number;       /* 1-based */

    u8              alive_alpha;        /* players alive on team alpha */
    u8              alive_beta;         /* players alive on team beta */
} ca_state_t;
```

### Game State (top-level)

```c
typedef struct {
    entity_pool_t       entities;
    ca_state_t          ca;             /* clan arena mode state */
    u32                 server_time_ms; /* cumulative server time */
    u8                  num_clients;
} qk_game_state_t;
```

### Mode Tick Logic

```c
void g_ca_tick(qk_game_state_t *gs, u32 dt_ms) {
    gs->server_time_ms += dt_ms;

    switch (gs->ca.state) {
    case ROUND_WARMUP:
        /* Allow free movement, no damage. Wait for ready-up or admin. */
        break;

    case ROUND_COUNTDOWN:
        gs->ca.state_timer_ms -= min_u32(dt_ms, gs->ca.state_timer_ms);
        if (gs->ca.state_timer_ms == 0) {
            g_ca_begin_round(gs);
            gs->ca.state = ROUND_PLAYING;
            gs->ca.state_timer_ms = ROUND_TIME_LIMIT_MS;
        }
        break;

    case ROUND_PLAYING:
        gs->ca.state_timer_ms -= min_u32(dt_ms, gs->ca.state_timer_ms);

        /* recount alive players */
        g_ca_count_alive(gs);

        if (gs->ca.alive_alpha == 0 || gs->ca.alive_beta == 0) {
            g_ca_end_round(gs);
        } else if (gs->ca.state_timer_ms == 0) {
            /* time expired: team with more total health wins,
               or draw (no score change) */
            g_ca_end_round_timeout(gs);
        }
        break;

    case ROUND_END:
        gs->ca.state_timer_ms -= min_u32(dt_ms, gs->ca.state_timer_ms);
        if (gs->ca.state_timer_ms == 0) {
            if (gs->ca.score_alpha >= ROUNDS_TO_WIN ||
                gs->ca.score_beta  >= ROUNDS_TO_WIN) {
                gs->ca.state = MATCH_END;
            } else {
                g_ca_start_countdown(gs);
            }
        }
        break;

    case MATCH_END:
        /* Display final scores. Wait for admin restart or auto-restart. */
        break;
    }
}
```

### Mode Functions

```c
/* Transition from WARMUP to COUNTDOWN */
void g_ca_start_countdown(qk_game_state_t *gs);

/* Transition from COUNTDOWN to PLAYING. Spawns all players. */
void g_ca_begin_round(qk_game_state_t *gs);

/* Called when one team is eliminated. Awards point, transitions to ROUND_END. */
void g_ca_end_round(qk_game_state_t *gs);

/* Called when round timer expires. Compare team HP, award point. */
void g_ca_end_round_timeout(qk_game_state_t *gs);

/* Count alive players per team. Updates ca.alive_alpha / ca.alive_beta. */
void g_ca_count_alive(qk_game_state_t *gs);
```

`g_ca_begin_round`:
1. Set `round_number++`.
2. Destroy all projectiles (iterate entities, free ENT_PROJECTILE).
3. For each player: call `g_player_spawn_ca(ent, spawn_point, angles)`.
4. `alive_alpha` = count of team alpha players. `alive_beta` = same for beta.

`g_ca_end_round`:
1. If `alive_alpha == 0` and `alive_beta > 0`: `score_beta++`.
2. If `alive_beta == 0` and `alive_alpha > 0`: `score_alpha++`.
3. If both zero (simultaneous kill): no score change (draw round).
4. `state = ROUND_END`, `state_timer_ms = ROUND_END_DELAY_MS`.

`g_ca_end_round_timeout`:
1. Sum health+armor for each team.
2. Team with higher total wins. Equal = draw.
3. Same transition as `g_ca_end_round`.

---

## 6. Damage and Combat

### Damage Pipeline

All damage flows through one function:

```c
typedef struct {
    u32         attacker_id;    /* entity id of attacker */
    u32         victim_id;      /* entity id of victim */
    i16         damage;         /* raw damage before armor */
    vec3_t      dir;            /* normalized direction of damage (for knockback) */
    f32         knockback;      /* knockback magnitude */
    weapon_id_t weapon;         /* which weapon caused this */
    bool        is_self;        /* self damage (rocket splash on self) */
} damage_event_t;
```

```c
void g_combat_apply_damage(qk_game_state_t *gs, const damage_event_t *dmg);
```

`g_combat_apply_damage`:
1. Find victim entity.
2. If victim is not `PSTATE_ALIVE`, return.
3. If round state is not `ROUND_PLAYING`, return (no damage in warmup/countdown).
4. If `is_self`: scale damage by `self_damage_mult` from weapon def.
5. Call `g_player_apply_armor` to split into health and armor damage.
6. Subtract from `ps->health` and `ps->armor`.
7. Apply knockback: `victim->velocity += dir * knockback`.
   For self-knockback: use `self_knockback` multiplier instead and invert direction.
8. Update attacker stats: `damage_given += actual_damage`.
9. Update victim stats: `damage_taken += actual_damage`.
10. If `ps->health <= 0`: call `g_combat_kill(gs, attacker_id, victim_id, weapon)`.

### Kill Processing

```c
void g_combat_kill(qk_game_state_t *gs, u32 attacker_id, u32 victim_id,
                   weapon_id_t weapon);
```

1. Set victim `alive_state = PSTATE_DEAD`.
2. Increment attacker `frags` (if not self-kill).
3. Increment victim `deaths`.
4. Push a kill event to the killfeed queue (see HUD section).
5. `g_ca_count_alive(gs)` to update alive counts (checked next tick).

### Hitscan Trace (Rail)

```c
typedef struct {
    bool        hit;
    vec3_t      end_pos;        /* trace end position */
    u32         hit_entity_id;  /* entity that was hit (0 if none) */
    vec3_t      hit_normal;     /* surface normal at impact */
    f32         fraction;       /* 0.0 - 1.0 distance fraction */
} trace_result_t;

trace_result_t g_combat_hitscan_trace(
    qk_game_state_t *gs,
    entity_t *attacker,
    vec3_t start,
    vec3_t dir,
    f32 range
);
```

1. Compute `end = start + dir * range`.
2. Trace against world geometry (call into physics: `physics_trace_line`).
3. Trace against all enemy player entities (AABB intersection test).
4. Return the closest hit.
5. If hit is a player entity: generate `damage_event_t` with full damage and
   knockback, call `g_combat_apply_damage`.

The rail trace also generates a visual event (rail trail) pushed to the client
snapshot.

### Beam Trace (LG)

Same as hitscan trace but fires every tick (50ms interval) and uses shorter
range (768 units). The beam trace is identical to hitscan mechanically; the
only difference is that it fires continuously while `BTN_ATTACK` is held and
`weapon_time` has expired.

```c
trace_result_t g_combat_beam_trace(
    qk_game_state_t *gs,
    entity_t *attacker,
    vec3_t start,
    vec3_t dir,
    f32 range
);
```

Implementation is identical to `g_combat_hitscan_trace`.

### Splash Damage (Rocket)

```c
void g_combat_splash_damage(
    qk_game_state_t *gs,
    vec3_t origin,              /* explosion center */
    f32 radius,
    f32 max_damage,
    f32 knockback,
    u32 attacker_id,
    weapon_id_t weapon
);
```

1. For each alive player entity within `radius` of `origin`:
   a. Compute `dist = distance(player.origin, origin)`.
   b. If `dist >= radius`, skip.
   c. `damage_frac = 1.0 - (dist / radius)`.
   d. `damage = (i16)(max_damage * damage_frac)`.
   e. `kb = knockback * damage_frac`.
   f. `dir = normalize(player.origin - origin)`.
   g. `is_self = (player.id == attacker_id)`.
   h. Build `damage_event_t` and call `g_combat_apply_damage`.

### Projectile Tick

```c
void g_projectile_tick(qk_game_state_t *gs, f32 dt);
```

For each active `ENT_PROJECTILE`:
1. Decrement `lifetime -= dt`. If `<= 0`, free entity.
2. Move: `new_origin = origin + velocity * dt`.
3. Trace from `origin` to `new_origin` against world + entities
   (call `physics_trace_line`).
4. If trace hit world geometry:
   a. Explode at hit point.
   b. Call `g_combat_splash_damage(...)`.
   c. Free the projectile entity.
5. If trace hit a player entity (direct hit):
   a. Apply direct damage (full `damage` from weapon def).
   b. Apply knockback.
   c. Then also call `g_combat_splash_damage(...)` from the hit point
      (splash still happens on direct hit).
   d. Free the projectile entity.
6. If no hit: update `origin = new_origin`.

NOTE: Direct rocket hit applies BOTH the direct damage AND splash. This
matches Quake behavior. Total damage on a direct hit can exceed the listed
damage value because the target is inside the splash radius at distance ~0.

---

## 7. Input Processing

### usercmd_t

This is the client-to-server command. It is produced once per client frame,
sent to the server, and consumed by the server at tick time.

```c
typedef enum {
    BTN_ATTACK      = (1 << 0),
    BTN_JUMP        = (1 << 1),
    BTN_CROUCH      = (1 << 2),
    BTN_USE         = (1 << 3),
} button_flags_t;

typedef struct {
    u32     server_time;    /* client's estimate of server time */
    u32     sequence;       /* monotonic cmd number */

    /* movement intentions (-127 to 127) */
    i8      forward_move;   /* +forward / -backward */
    i8      right_move;     /* +right / -left */
    i8      up_move;        /* +jump / -crouch (alternative) */

    /* view angles (full precision) */
    f32     pitch;
    f32     yaw;

    /* buttons (bitmask) */
    u16     buttons;

    /* weapon selection (0 = no change) */
    u8      weapon_select;
} usercmd_t;
```

### Input State (Client-Side)

```c
typedef struct {
    /* keyboard state */
    bool    key_forward;
    bool    key_backward;
    bool    key_left;
    bool    key_right;
    bool    key_jump;
    bool    key_crouch;
    bool    key_attack;
    bool    key_weapon1;    /* rocket */
    bool    key_weapon2;    /* rail */
    bool    key_weapon3;    /* lg */

    /* mouse state */
    f32     mouse_dx;       /* raw mouse delta this frame */
    f32     mouse_dy;

    /* accumulated view angles */
    f32     pitch;
    f32     yaw;

    /* sensitivity */
    f32     sensitivity;
    f32     pitch_scale;    /* usually negative for inverted, or 1.0 */
} input_state_t;
```

### Building a usercmd

```c
usercmd_t cl_build_usercmd(const input_state_t *input, u32 server_time) {
    usercmd_t cmd = {0};
    cmd.server_time = server_time;
    cmd.sequence    = ++cl.cmd_sequence;

    /* movement */
    cmd.forward_move = 0;
    if (input->key_forward)  cmd.forward_move += 127;
    if (input->key_backward) cmd.forward_move -= 127;

    cmd.right_move = 0;
    if (input->key_right) cmd.right_move += 127;
    if (input->key_left)  cmd.right_move -= 127;

    cmd.up_move = 0;
    if (input->key_jump)  cmd.up_move += 127;
    if (input->key_crouch) cmd.up_move -= 127;

    /* view angles: accumulate mouse movement */
    /* (mouse_dx/dy are raw deltas, already read and zeroed each frame) */
    cmd.yaw   = input->yaw;
    cmd.pitch  = input->pitch;

    /* buttons */
    cmd.buttons = 0;
    if (input->key_attack) cmd.buttons |= BTN_ATTACK;
    if (input->key_jump)   cmd.buttons |= BTN_JUMP;
    if (input->key_crouch) cmd.buttons |= BTN_CROUCH;

    /* weapon select */
    cmd.weapon_select = 0;
    if (input->key_weapon1) cmd.weapon_select = WEAPON_ROCKET;
    if (input->key_weapon2) cmd.weapon_select = WEAPON_RAIL;
    if (input->key_weapon3) cmd.weapon_select = WEAPON_LG;

    return cmd;
}
```

### Mouse Handling

```c
void cl_process_mouse(input_state_t *input, f32 dx, f32 dy) {
    input->yaw   -= dx * input->sensitivity;
    input->pitch  += dy * input->sensitivity * input->pitch_scale;

    /* clamp pitch to prevent flipping */
    if (input->pitch > 89.0f)  input->pitch = 89.0f;
    if (input->pitch < -89.0f) input->pitch = -89.0f;

    /* normalize yaw to [0, 360) */
    while (input->yaw < 0.0f)    input->yaw += 360.0f;
    while (input->yaw >= 360.0f) input->yaw -= 360.0f;
}
```

### Server-Side Command Processing

```c
void g_process_commands(qk_game_state_t *gs) {
    for (u8 i = 0; i < MAX_PLAYERS; i++) {
        i32 ent_idx = gs->entities.player_entity[i];
        if (ent_idx < 0) continue;

        entity_t *ent = &gs->entities.entities[ent_idx];
        player_state_t *ps = &ent->u.player.ps;

        if (ps->alive_state != PSTATE_ALIVE) continue;

        usercmd_t *cmd = &ps->last_cmd;

        /* update view angles */
        ent->angles.x = cmd->pitch;
        ent->angles.y = cmd->yaw;

        /* weapon switch request */
        if (cmd->weapon_select != 0 &&
            cmd->weapon_select != ps->weapon &&
            ps->ammo[cmd->weapon_select] > 0) {
            g_weapon_switch(ent, (weapon_id_t)cmd->weapon_select);
        }

        /* weapon tick handles firing */
        g_weapon_tick(&gs->entities, ent, SV_TICK_DT * 1000);
    }
}
```

The `forward_move`, `right_move`, and `up_move` fields are consumed by the
physics module (not gameplay). Physics reads `last_cmd` from the player state
to drive movement.

---

## 8. HUD Rendering

### Design

Immediate-mode. Every frame, the HUD reads `player_state_t` and
`qk_game_state_t`, computes screen positions, and issues draw calls to the
renderer's UI drawing API. No retained state. Must complete in under 0.1ms.

### UI Drawing Primitives (provided by renderer)

These are the functions the gameplay engineer calls. They are implemented in
`src/renderer/` or `src/ui/ui_draw.c` (which calls into the renderer).

```c
/* All coordinates are in screen pixels. (0,0) is top-left. */

void ui_draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color_rgba);
void ui_draw_rect_outline(f32 x, f32 y, f32 w, f32 h, u32 color_rgba, f32 thickness);
void ui_draw_text(f32 x, f32 y, const char *text, f32 size, u32 color_rgba);
void ui_draw_number(f32 x, f32 y, i32 value, f32 size, u32 color_rgba);
void ui_draw_icon(f32 x, f32 y, f32 size, u32 icon_id);

/* text measurement */
f32  ui_text_width(const char *text, f32 size);
```

### Color Constants

```c
#define COLOR_WHITE     0xFFFFFFFF
#define COLOR_RED       0xFF0000FF
#define COLOR_GREEN     0x00FF00FF
#define COLOR_YELLOW    0xFFFF00FF
#define COLOR_CYAN      0x00FFFFFF
#define COLOR_BLUE      0x4444FFFF
#define COLOR_ORANGE    0xFF8800FF
#define COLOR_GRAY      0x888888FF

/* team colors */
#define COLOR_TEAM_ALPHA    COLOR_RED
#define COLOR_TEAM_BETA     COLOR_BLUE
```

### HUD Layout

```
+---------------------------------------------------------------+
|  [ALPHA 5] ------- 1:23 ------- [BETA 3]                     |  <-- top bar
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                         +                                      |  <-- crosshair
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                      * hit *                                   |  <-- hit marker (brief)
|                                                                |
|                                          PlayerX fragged       |
|                                          PlayerY with RL       |  <-- killfeed (right)
|                                                                |
|  [200 HP]   [200 AP]                  [RL: 24]                |  <-- bottom bar
+---------------------------------------------------------------+
```

### HUD Rendering Function

```c
void qk_ui_draw_hud(const player_state_t *ps, const qk_game_state_t *gs,
                     f32 screen_w, f32 screen_h) {
    /* ---- Bottom bar ---- */

    /* Health: bottom-left */
    u32 hp_color = (ps->health <= 25) ? COLOR_RED :
                   (ps->health <= 50) ? COLOR_ORANGE : COLOR_WHITE;
    ui_draw_number(20.0f, screen_h - 60.0f, ps->health, 48.0f, hp_color);
    ui_draw_icon(72.0f, screen_h - 56.0f, 24.0f, ICON_HEALTH);

    /* Armor: next to health */
    u32 ap_color = (ps->armor <= 25) ? COLOR_RED :
                   (ps->armor <= 50) ? COLOR_YELLOW : COLOR_GREEN;
    ui_draw_number(140.0f, screen_h - 60.0f, ps->armor, 48.0f, ap_color);
    ui_draw_icon(192.0f, screen_h - 56.0f, 24.0f, ICON_ARMOR);

    /* Ammo: bottom-right */
    const g_weapon_def_t *wdef = &g_weapon_defs[ps->weapon];
    u32 ammo_color = (ps->ammo[ps->weapon] <= 5) ? COLOR_RED : COLOR_YELLOW;
    ui_draw_number(screen_w - 120.0f, screen_h - 60.0f,
                   ps->ammo[ps->weapon], 48.0f, ammo_color);
    ui_draw_text(screen_w - 120.0f, screen_h - 24.0f, wdef->name, 14.0f, COLOR_GRAY);

    /* ---- Top bar ---- */

    /* Round timer: top-center */
    u32 time_sec = gs->ca.state_timer_ms / 1000;
    char timer_buf[8];
    snprintf(timer_buf, sizeof(timer_buf), "%u:%02u", time_sec / 60, time_sec % 60);
    f32 tw = ui_text_width(timer_buf, 32.0f);
    ui_draw_text(screen_w * 0.5f - tw * 0.5f, 16.0f, timer_buf, 32.0f, COLOR_WHITE);

    /* Team scores */
    char score_a[4], score_b[4];
    snprintf(score_a, sizeof(score_a), "%u", gs->ca.score_alpha);
    snprintf(score_b, sizeof(score_b), "%u", gs->ca.score_beta);

    ui_draw_text(screen_w * 0.5f - 80.0f, 16.0f, score_a, 32.0f, COLOR_TEAM_ALPHA);
    ui_draw_text(screen_w * 0.5f + 60.0f, 16.0f, score_b, 32.0f, COLOR_TEAM_BETA);

    /* ---- Crosshair ---- */
    ui_draw_crosshair(screen_w, screen_h);

    /* ---- Hit marker ---- */
    ui_draw_hitmarker(screen_w, screen_h);

    /* ---- Killfeed ---- */
    ui_draw_killfeed(screen_w);
}
```

### Crosshair

```c
void ui_draw_crosshair(f32 screen_w, f32 screen_h) {
    f32 cx = screen_w * 0.5f;
    f32 cy = screen_h * 0.5f;
    f32 gap = 3.0f;
    f32 len = 8.0f;
    f32 thick = 2.0f;

    /* four lines: top, bottom, left, right */
    ui_draw_rect(cx - thick * 0.5f, cy - gap - len, thick, len, COLOR_WHITE);
    ui_draw_rect(cx - thick * 0.5f, cy + gap,       thick, len, COLOR_WHITE);
    ui_draw_rect(cx - gap - len,    cy - thick * 0.5f, len, thick, COLOR_WHITE);
    ui_draw_rect(cx + gap,          cy - thick * 0.5f, len, thick, COLOR_WHITE);
}
```

### Hit Marker

A brief (200ms) expanding cross rendered on damage dealt.

```c
#define HITMARKER_DURATION_MS   200

typedef struct {
    u32     time_remaining_ms;
    i16     damage;                 /* for future: scale by damage */
} hitmarker_state_t;

/* global (client-side) */
static hitmarker_state_t cl_hitmarker;

void ui_hitmarker_trigger(i16 damage) {
    cl_hitmarker.time_remaining_ms = HITMARKER_DURATION_MS;
    cl_hitmarker.damage = damage;
}

void ui_draw_hitmarker(f32 screen_w, f32 screen_h) {
    if (cl_hitmarker.time_remaining_ms == 0) return;

    f32 cx = screen_w * 0.5f;
    f32 cy = screen_h * 0.5f;
    f32 alpha = (f32)cl_hitmarker.time_remaining_ms / HITMARKER_DURATION_MS;
    u32 color = ((u32)(alpha * 255.0f)) | 0xFFFFFF00;

    f32 offset = 6.0f;
    f32 len = 10.0f;
    f32 thick = 2.0f;

    /* four diagonal lines */
    ui_draw_rect(cx - offset - len, cy - offset - len, len, thick, color);
    ui_draw_rect(cx + offset,       cy - offset - len, len, thick, color);
    ui_draw_rect(cx - offset - len, cy + offset,       len, thick, color);
    ui_draw_rect(cx + offset,       cy + offset,       len, thick, color);
}
```

### Killfeed

A scrolling list of recent kills, displayed in the top-right corner.

```c
#define KILLFEED_MAX_ENTRIES 5
#define KILLFEED_DISPLAY_MS  5000

typedef struct {
    char        attacker_name[32];
    char        victim_name[32];
    weapon_id_t weapon;
    u32         time_remaining_ms;
    bool        active;
} killfeed_entry_t;

typedef struct {
    killfeed_entry_t entries[KILLFEED_MAX_ENTRIES];
} killfeed_t;

/* global (client-side) */
static killfeed_t cl_killfeed;

void ui_killfeed_push(const char *attacker, const char *victim, weapon_id_t weapon);
void ui_draw_killfeed(f32 screen_w);
```

`ui_draw_killfeed` iterates from newest to oldest, drawing each active entry:
```
  attacker_name  [weapon_icon]  victim_name
```
Right-aligned at `screen_w - 20`. Entries fade out over the last 1000ms.

---

## 9. Data Structures and Interfaces

### Interfaces This Module PROVIDES (to other modules)

```c
/* ---- include/gameplay/gameplay.h ---- */

/* Game lifecycle */
qk_result_t     qk_game_init(const qk_game_config_t *config);
void            qk_game_tick(qk_world_t *world, f32 dt);
void            qk_game_shutdown(void);

/* Player management (called by netcode on connect/disconnect) */
i32             qk_game_player_connect(qk_game_state_t *gs, u8 client_num,
                                       const char *name, team_t team);
void            qk_game_player_disconnect(qk_game_state_t *gs, u8 client_num);

/* Command injection (called by netcode when usercmd arrives) */
void            qk_game_player_command(qk_game_state_t *gs, u8 client_num,
                                       const usercmd_t *cmd);

/* State queries (called by netcode for snapshots) */
const player_state_t *qk_game_get_player_state(const qk_game_state_t *gs,
                                                u8 client_num);
const ca_state_t     *qk_game_get_ca_state(const qk_game_state_t *gs);
```

```c
/* ---- include/ui/ui.h ---- */

/* HUD (called by client main loop after world render) */
void            qk_ui_draw_hud(const player_state_t *ps,
                                const qk_game_state_t *gs,
                                f32 screen_w, f32 screen_h);
void            qk_ui_draw_scoreboard(const qk_game_state_t *gs,
                                       f32 screen_w, f32 screen_h);

/* Event push (called by client when receiving game events from server) */
void            qk_ui_event_kill(const char *attacker, const char *victim,
                                 weapon_id_t weapon);
void            qk_ui_event_hit(i16 damage);

/* Tick (fade timers, called once per client frame) */
void            qk_ui_tick(u32 dt_ms);
```

### Interfaces This Module REQUIRES (from other modules)

#### From Physics (`include/physics/physics.h`)

```c
/* Trace a line segment against world geometry and entity bounding boxes. */
trace_result_t  physics_trace_line(const vec3_t *start, const vec3_t *end,
                                   u32 skip_entity_id);

/* Simulate one tick of player movement based on usercmd. */
void            physics_player_move(entity_t *ent, const usercmd_t *cmd, f32 dt);

/* Simulate one tick of projectile movement (simple ballistic). */
void            physics_projectile_move(entity_t *ent, f32 dt);

/* Query: find all entities within radius of a point. */
u32             physics_find_in_radius(const entity_pool_t *pool,
                                       vec3_t center, f32 radius,
                                       u32 *out_ids, u32 max_ids);
```

#### From Renderer (`include/renderer/renderer.h`)

```c
/* UI drawing primitives (rendered to offscreen UI texture) */
void            ui_draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color_rgba);
void            ui_draw_rect_outline(f32 x, f32 y, f32 w, f32 h,
                                      u32 color_rgba, f32 thickness);
void            ui_draw_text(f32 x, f32 y, const char *text, f32 size,
                              u32 color_rgba);
void            ui_draw_number(f32 x, f32 y, i32 value, f32 size,
                                u32 color_rgba);
void            ui_draw_icon(f32 x, f32 y, f32 size, u32 icon_id);
f32             ui_text_width(const char *text, f32 size);

/* Visual events (renderer draws these in world space) */
void            renderer_spawn_rail_trail(vec3_t start, vec3_t end, u32 color);
void            renderer_spawn_explosion(vec3_t origin, f32 radius);
void            renderer_spawn_beam(vec3_t start, vec3_t end);
```

#### From Core/Netcode

```c
/* Get current server time estimate (client-side). */
u32             cl_get_server_time(void);

/* Send usercmd to server. */
void            cl_send_usercmd(const usercmd_t *cmd);
```

### Shared Math Types (from `include/quicken.h` or a shared math header)

```c
typedef struct { f32 x, y, z; }     vec3_t;
typedef struct { vec3_t min, max; } bbox_t;

typedef enum {
    QK_OK = 0,
    QK_ERROR,
    QK_ERROR_FULL,
    QK_ERROR_NOT_FOUND,
} qk_result_t;
```

### Game Config

```c
typedef struct {
    u8      max_players;
    u8      rounds_to_win;
    u32     round_time_limit_ms;
    u32     countdown_time_ms;
    /* future: map name, server name, etc. */
} qk_game_config_t;
```

### Event Queue (Gameplay -> Client)

The server produces game events that the client needs for UI (killfeed, hit
confirmation). These are included in the server snapshot.

```c
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
        struct { u8 attacker; u8 victim; weapon_id_t weapon; } kill;
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
```

```c
void g_event_push(game_event_queue_t *queue, const game_event_t *event);
void g_event_clear(game_event_queue_t *queue);
```

The event queue is a member of `qk_game_state_t`. After the server builds
each snapshot, the events are serialized and sent to clients. After sending,
the queue is cleared.

---

## 10. Build Integration

Gameplay and UI code compiles as part of the main `quicken` executable (not a
separate static library), since it uses precise floating-point like the rest
of the game logic.

Add to `premake5.lua` in the `quicken` project:

```lua
files {
    "src/*.c",
    "src/core/**.c",
    "src/gameplay/**.c",    -- ADD
    "src/ui/**.c",          -- ADD
    "include/**.h"
}
```

No additional compiler flags are needed since the main executable already
compiles with `/fp:precise`.

---

## 11. File Manifest

### Source Files

| File                        | Purpose                                          |
|-----------------------------|--------------------------------------------------|
| `src/gameplay/g_main.c`     | `qk_game_init`, `qk_game_tick`, `qk_game_shutdown`, game state storage |
| `src/gameplay/g_player.c`   | Player spawn, armor absorption, stat tracking     |
| `src/gameplay/g_weapons.c`  | Weapon defs table, weapon tick, fire, switch       |
| `src/gameplay/g_combat.c`   | Damage pipeline, hitscan trace, splash damage, kill processing |
| `src/gameplay/g_projectile.c` | Projectile spawn, tick, collision/explosion       |
| `src/gameplay/g_ca.c`       | Clan Arena round state machine, all CA-specific logic |
| `src/gameplay/g_entity.c`   | Entity pool: alloc, free, find, iterate            |
| `src/gameplay/g_event.c`    | Game event queue: push, clear                      |
| `src/ui/ui_hud.c`          | `qk_ui_draw_hud`, health/armor/ammo/timer/scores   |
| `src/ui/ui_crosshair.c`    | Crosshair drawing                                  |
| `src/ui/ui_hitmarker.c`    | Hit marker state and drawing                       |
| `src/ui/ui_killfeed.c`     | Kill feed state and drawing                        |
| `src/ui/ui_scoreboard.c`   | Scoreboard overlay                                 |
| `src/ui/ui_draw.c`         | Thin wrappers / helpers over renderer primitives   |

### Header Files

| File                            | Purpose                                          |
|---------------------------------|--------------------------------------------------|
| `include/gameplay/gameplay.h`   | Public gameplay API (`qk_game_*`)                 |
| `include/gameplay/g_local.h`    | Internal gameplay types (entity_t, player_state_t, weapon defs, CA state) |
| `include/ui/ui.h`              | Public UI API (`qk_ui_*`)                         |
| `include/ui/ui_local.h`        | Internal UI types (killfeed, hitmarker state)      |

### Header Dependency Graph

```
quicken.h
    |
    +---> gameplay/gameplay.h   (public: qk_game_*, usercmd_t, player_state_t)
    |         |
    |         +---> gameplay/g_local.h  (internal: entity_t, weapon defs, ca_state_t)
    |
    +---> ui/ui.h               (public: qk_ui_*)
    |         |
    |         +---> ui/ui_local.h       (internal: killfeed_t, hitmarker_state_t)
    |
    +---> physics/physics.h     (trace_result_t, physics_player_move, etc.)
    +---> renderer/renderer.h   (ui_draw_*, renderer_spawn_*)
```

---

## 12. Implementation Order

This is the order in which files should be implemented and tested. Each step
builds on the previous and can be tested independently.

### Phase 1: Foundation (no rendering needed, test with prints)

1. **`g_entity.c` / `g_local.h`**: Entity pool, alloc/free/iterate. Unit test:
   alloc MAX_ENTITIES, verify IDs are unique, free and realloc.

2. **`g_player.c`**: Player spawn function (CA variant), armor absorption.
   Unit test: spawn a player, verify health/armor/ammo values. Apply damage
   through armor, verify split.

3. **`g_event.c`**: Event queue push/clear. Trivial, but needed by everything.

### Phase 2: Weapons and Combat

4. **`g_weapons.c`**: Weapon defs table, weapon tick (cooldown decrement,
   switch logic), fire dispatch. Test: spawn player, simulate ticks, verify
   fire timing.

5. **`g_combat.c`**: Damage pipeline (`g_combat_apply_damage`), hitscan trace
   (stub physics_trace_line initially), splash damage, kill processing.
   Test: apply damage, verify health/armor changes, verify kill triggers
   at health <= 0.

6. **`g_projectile.c`**: Projectile spawn, tick, collision. Test: spawn rocket,
   tick it forward, verify it moves, verify it expires after lifetime.

### Phase 3: Game Mode

7. **`g_ca.c`**: Full Clan Arena state machine. Test: walk through all states
   manually: warmup -> countdown -> playing -> kill all of one team ->
   round_end -> next round -> ... -> match_end.

8. **`g_main.c`**: Wire everything together. `qk_game_init` creates the game
   state, `qk_game_tick` calls CA tick, processes commands, ticks projectiles,
   resolves combat. `qk_game_shutdown` cleans up.

### Phase 4: HUD and UI

9. **`ui_draw.c`**: Wrapper/helper layer over renderer primitives. May be
   trivially thin at first.

10. **`ui_hud.c`**: Main HUD draw function. Requires renderer primitives to
    exist (even as stubs).

11. **`ui_crosshair.c`**: Crosshair drawing.

12. **`ui_hitmarker.c`**: Hit marker trigger and fade.

13. **`ui_killfeed.c`**: Kill feed push and draw.

14. **`ui_scoreboard.c`**: Scoreboard overlay (shows all players, teams,
    scores, stats).

### Phase 5: Integration

15. Wire client input (`cl_build_usercmd`) into the main client loop.
16. Wire `qk_game_player_command` into the server's command reception path.
17. Wire `qk_ui_draw_hud` into the client render path.
18. End-to-end test: local server, single client, play a CA round.

---

## Appendix A: Constants Reference

```c
/* Tick rate */
#define SV_TICK_RATE            125
#define SV_TICK_DT              (1.0f / SV_TICK_RATE)
#define SV_TICK_DT_MS           8

/* Entity limits */
#define MAX_ENTITIES            256
#define MAX_PLAYERS             16

/* Clan Arena */
#define CA_ROUNDS_TO_WIN        10
#define CA_COUNTDOWN_MS         5000
#define CA_ROUND_TIME_MS        120000
#define CA_ROUND_END_DELAY_MS   3000

/* CA spawn values */
#define CA_SPAWN_HEALTH         200
#define CA_SPAWN_ARMOR          200
#define CA_SPAWN_AMMO_ROCKET    25
#define CA_SPAWN_AMMO_RAIL      10
#define CA_SPAWN_AMMO_LG        150

/* Armor */
#define ARMOR_ABSORB_FACTOR     0.66f

/* HUD */
#define HUD_HITMARKER_MS        200
#define HUD_KILLFEED_ENTRIES    5
#define HUD_KILLFEED_MS         5000

/* Events */
#define MAX_EVENTS_PER_TICK     32
```

## Appendix B: Weapon Quick-Reference

```
Rocket Launcher:
  - 100 direct dmg, 100 max splash dmg in 120u radius
  - 800ms between shots, 1000 u/s projectile speed
  - 50% self-damage, 120% self-knockback (rocket jumps)
  - 25 ammo

Railgun:
  - 80 damage, instant hitscan, 8192u range
  - 1500ms between shots (long cooldown)
  - No splash, no self-damage
  - 10 ammo

Lightning Gun:
  - 7 damage per tick (50ms interval = 140 dps)
  - Continuous beam hitscan, 768u range
  - No splash, no self-damage
  - 150 ammo (7.5 seconds of sustained fire)
```
