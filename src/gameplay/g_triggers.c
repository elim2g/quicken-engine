/*
 * QUICKEN Engine - Trigger System (Teleporters + Jump Pads)
 *
 * Checks player overlap with trigger volumes each tick.
 * Teleporters: snap player origin to destination, set facing angle.
 * Jump pads: calculate launch velocity toward target position.
 */

#include "g_internal.h"
#include "physics/qk_physics.h"
#include <math.h>
#include <string.h>

/* ---- Trigger storage (set during map load via g_triggers_load) ---- */

#define MAX_TELEPORTERS 64
#define MAX_JUMP_PADS   64

static qk_teleporter_t s_teleporters[MAX_TELEPORTERS];
static u32              s_teleporter_count;

static qk_jump_pad_t   s_jump_pads[MAX_JUMP_PADS];
static u32              s_jump_pad_count;

/* Cooldown to prevent re-triggering on the same tick or immediately after */
#define TELEPORT_COOLDOWN_TICKS  16  /* ~125ms at 128Hz */
#define JUMP_PAD_COOLDOWN_TICKS  16

static u32 s_teleport_cooldown[QK_MAX_PLAYERS];
static u32 s_jump_pad_cooldown[QK_MAX_PLAYERS];

/* ---- Load triggers from map data ---- */

void g_triggers_load(const qk_teleporter_t *teleporters, u32 teleporter_count,
                     const qk_jump_pad_t *jump_pads, u32 jump_pad_count) {
    s_teleporter_count = 0;
    s_jump_pad_count = 0;

    if (teleporters && teleporter_count > 0) {
        u32 count = teleporter_count < MAX_TELEPORTERS ? teleporter_count : MAX_TELEPORTERS;
        memcpy(s_teleporters, teleporters, count * sizeof(qk_teleporter_t));
        s_teleporter_count = count;
    }

    if (jump_pads && jump_pad_count > 0) {
        u32 count = jump_pad_count < MAX_JUMP_PADS ? jump_pad_count : MAX_JUMP_PADS;
        memcpy(s_jump_pads, jump_pads, count * sizeof(qk_jump_pad_t));
        s_jump_pad_count = count;
    }

    memset(s_teleport_cooldown, 0, sizeof(s_teleport_cooldown));
    memset(s_jump_pad_cooldown, 0, sizeof(s_jump_pad_cooldown));
}

void g_triggers_clear(void) {
    s_teleporter_count = 0;
    s_jump_pad_count = 0;
    memset(s_teleport_cooldown, 0, sizeof(s_teleport_cooldown));
    memset(s_jump_pad_cooldown, 0, sizeof(s_jump_pad_cooldown));
}

/* ---- AABB-AABB overlap test ---- */

static bool aabb_overlap(vec3_t a_min, vec3_t a_max, vec3_t b_min, vec3_t b_max) {
    if (a_max.x < b_min.x || a_min.x > b_max.x) return false;
    if (a_max.y < b_min.y || a_min.y > b_max.y) return false;
    if (a_max.z < b_min.z || a_min.z > b_max.z) return false;
    return true;
}

/* ---- Player AABB from origin ---- */

static void player_aabb(const qk_player_state_t *ps, vec3_t *out_min, vec3_t *out_max) {
    out_min->x = ps->origin.x + ps->mins.x;
    out_min->y = ps->origin.y + ps->mins.y;
    out_min->z = ps->origin.z + ps->mins.z;
    out_max->x = ps->origin.x + ps->maxs.x;
    out_max->y = ps->origin.y + ps->maxs.y;
    out_max->z = ps->origin.z + ps->maxs.z;
}

/* ---- Teleporter check ---- */

static void g_check_teleporters(qk_game_state_t *gs) {
    if (s_teleporter_count == 0) return;

    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        i32 ent_idx = gs->player_entity[i];
        if (ent_idx < 0) continue;

        entity_t *ent = &gs->entities.entities[ent_idx];
        qk_player_state_t *ps = &ent->data.player;

        if (ps->alive_state != QK_PSTATE_ALIVE) continue;
        if (s_teleport_cooldown[i] > 0) {
            s_teleport_cooldown[i]--;
            continue;
        }

        vec3_t pmin, pmax;
        player_aabb(ps, &pmin, &pmax);

        for (u32 t = 0; t < s_teleporter_count; t++) {
            const qk_teleporter_t *tp = &s_teleporters[t];

            if (!aabb_overlap(pmin, pmax, tp->mins, tp->maxs)) continue;

            /* Teleport: set origin to destination */
            ps->origin = tp->destination;

            /* Set facing angle */
            ps->yaw = tp->dest_yaw;

            /* Preserve horizontal speed magnitude but redirect to new facing */
            f32 horiz_speed = sqrtf(ps->velocity.x * ps->velocity.x +
                                    ps->velocity.y * ps->velocity.y);
            f32 yaw_rad = tp->dest_yaw * (3.14159265f / 180.0f);
            ps->velocity.x = horiz_speed * cosf(yaw_rad);
            ps->velocity.y = horiz_speed * sinf(yaw_rad);
            /* Keep vertical velocity as-is */

            /* Toggle teleport bit — netcode detects via XOR between snapshots */
            ps->teleport_bit ^= 1;

            /* Cooldown to prevent immediate re-trigger */
            s_teleport_cooldown[i] = TELEPORT_COOLDOWN_TICKS;

            break; /* only one teleport per tick */
        }
    }
}

/* ---- Jump pad check ---- */

static void g_check_jump_pads(qk_game_state_t *gs) {
    if (s_jump_pad_count == 0) return;

    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        i32 ent_idx = gs->player_entity[i];
        if (ent_idx < 0) continue;

        entity_t *ent = &gs->entities.entities[ent_idx];
        qk_player_state_t *ps = &ent->data.player;

        if (ps->alive_state != QK_PSTATE_ALIVE) continue;
        if (s_jump_pad_cooldown[i] > 0) {
            s_jump_pad_cooldown[i]--;
            continue;
        }

        vec3_t pmin, pmax;
        player_aabb(ps, &pmin, &pmax);

        for (u32 j = 0; j < s_jump_pad_count; j++) {
            const qk_jump_pad_t *jp = &s_jump_pads[j];

            if (!aabb_overlap(pmin, pmax, jp->mins, jp->maxs)) continue;

            /* Override player velocity with physics-calculated launch velocity */
            ps->velocity = qk_physics_jumppad_velocity(ps->origin, jp->target);

            /* Take player off ground so air physics apply */
            ps->on_ground = false;
            ps->jump_held = true; /* prevent immediate jump override */
            ps->splash_slick_ticks = 3; /* prevent ground friction from eating launch velocity */

            /* Cooldown */
            s_jump_pad_cooldown[i] = JUMP_PAD_COOLDOWN_TICKS;

            break; /* only one pad per tick */
        }
    }
}

/* ---- Public tick function ---- */

void g_triggers_tick(qk_game_state_t *gs) {
    g_check_teleporters(gs);
    g_check_jump_pads(gs);
}
