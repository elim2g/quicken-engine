/*
 * QUICKEN Engine - Physics Public API
 *
 * Delegates to internal implementations in p_*.c files.
 * This is the only file that provides symbols matching qk_physics.h.
 */

#include "physics/p_internal.h"

/* ---- World lifecycle ---- */

qk_phys_world_t *qk_physics_world_create(qk_collision_model_t *cm) {
    return p_world_create(cm);
}

void qk_physics_world_destroy(qk_phys_world_t *world) {
    p_world_destroy(world);
}

/* ---- Player init ---- */

void qk_physics_player_init(qk_player_state_t *ps, vec3_t spawn_origin) {
    if (!ps) return;

    ps->origin = spawn_origin;
    ps->velocity = (vec3_t){0.0f, 0.0f, 0.0f};
    ps->mins = QK_PLAYER_MINS;
    ps->maxs = QK_PLAYER_MAXS;
    ps->on_ground = false;
    ps->ground_normal = (vec3_t){0.0f, 0.0f, 0.0f};
    ps->jump_held = false;
    ps->max_speed = QK_PM_MAX_SPEED;
    ps->gravity = QK_PM_GRAVITY;
    ps->command_time = 0;
}

/* ---- Run one physics tick ---- */

void qk_physics_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                      const qk_phys_world_t *world) {
    p_move(ps, cmd, world);
}

/* ---- Fixed-timestep wrapper ---- */

void qk_physics_update(qk_phys_time_t *ts, f32 frame_dt,
                        qk_player_state_t *ps, const qk_usercmd_t *cmd,
                        const qk_phys_world_t *world) {
    p_time_update(ts, frame_dt, ps, cmd, world);
}

/* ---- Trace a box through the world ---- */

qk_trace_result_t qk_physics_trace(const qk_phys_world_t *world,
                                     vec3_t start, vec3_t end,
                                     vec3_t mins, vec3_t maxs) {
    return p_trace_world(world, start, end, mins, maxs);
}

/* ---- Get interpolation alpha for rendering ---- */

f32 qk_physics_get_alpha(const qk_phys_time_t *ts) {
    return p_time_get_alpha(ts);
}
