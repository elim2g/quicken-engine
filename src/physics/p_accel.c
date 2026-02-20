/*
 * QUICKEN Engine - Acceleration and Friction
 *
 * PM_Accelerate (Q3-style: project-then-cap) and ground friction.
 * Air movement uses the same accelerate with a lower wish_speed
 * (QK_PM_AIR_SPEED) instead of a separate capped function.
 */

#include "p_internal.h"

// --- Acceleration (Q3-style) ---

void p_accelerate(qk_player_state_t *ps, vec3_t wish_dir,
                  f32 wish_speed, f32 accel, f32 dt) {
    // Current speed projected onto wish direction
    f32 current_speed = vec3_dot(ps->velocity, wish_dir);

    // How much we can accelerate before hitting wish_speed
    f32 add_speed = wish_speed - current_speed;
    if (add_speed <= 0.0f) return;

    // Acceleration this frame
    f32 accel_speed = accel * wish_speed * dt;
    if (accel_speed > add_speed) {
        accel_speed = add_speed;
    }

    ps->velocity.x += accel_speed * wish_dir.x;
    ps->velocity.y += accel_speed * wish_dir.y;
    ps->velocity.z += accel_speed * wish_dir.z;
}

// --- Ground friction ---

void p_apply_friction(qk_player_state_t *ps, f32 dt) {
    f32 speed = vec3_length(ps->velocity);
    if (speed < 0.1f) {
        ps->velocity.x = 0.0f;
        ps->velocity.y = 0.0f;
        // preserve vertical velocity
        return;
    }

    // Q3 friction: use max(speed, PM_STOP_SPEED) for the friction term.
    // Slow movement gets MORE friction relative to speed,
    // bringing it to a stop faster.
    f32 control = (speed < QK_PM_STOP_SPEED) ? QK_PM_STOP_SPEED : speed;
    f32 drop = control * QK_PM_GROUND_FRICTION * dt;

    f32 new_speed = speed - drop;
    if (new_speed < 0.0f) new_speed = 0.0f;

    f32 scale = new_speed / speed;
    ps->velocity.x *= scale;
    ps->velocity.y *= scale;
    // z velocity preserved -- important for ramp interactions
}
