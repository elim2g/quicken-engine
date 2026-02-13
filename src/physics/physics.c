/*
 * QUICKEN Physics Module - Stub Implementation
 *
 * All functions return success/zero/no-op.
 * The physics agent replaces this with real code on feat/physics.
 */

#include "physics/qk_physics.h"

struct qk_phys_world {
    qk_collision_model_t *cm;
};

qk_phys_world_t *qk_physics_world_create(qk_collision_model_t *cm) {
    QK_UNUSED(cm);
    return NULL;
}

void qk_physics_world_destroy(qk_phys_world_t *world) {
    QK_UNUSED(world);
}

void qk_physics_player_init(qk_player_state_t *ps, vec3_t spawn_origin) {
    QK_UNUSED(ps);
    QK_UNUSED(spawn_origin);
}

void qk_physics_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                      const qk_phys_world_t *world) {
    QK_UNUSED(ps);
    QK_UNUSED(cmd);
    QK_UNUSED(world);
}

void qk_physics_update(qk_phys_time_t *ts, f32 frame_dt,
                        qk_player_state_t *ps, const qk_usercmd_t *cmd,
                        const qk_phys_world_t *world) {
    QK_UNUSED(ts);
    QK_UNUSED(frame_dt);
    QK_UNUSED(ps);
    QK_UNUSED(cmd);
    QK_UNUSED(world);
}

qk_trace_result_t qk_physics_trace(const qk_phys_world_t *world,
                                     vec3_t start, vec3_t end,
                                     vec3_t mins, vec3_t maxs) {
    QK_UNUSED(world);
    QK_UNUSED(start);
    QK_UNUSED(end);
    QK_UNUSED(mins);
    QK_UNUSED(maxs);
    qk_trace_result_t result = {0};
    result.fraction = 1.0f;
    result.brush_index = -1;
    result.entity_id = -1;
    return result;
}

f32 qk_physics_get_alpha(const qk_phys_time_t *ts) {
    QK_UNUSED(ts);
    return 0.0f;
}
