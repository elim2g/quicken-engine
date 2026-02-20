/*
 * QUICKEN Engine - Jump Pad Launch Velocity
 *
 * Calculates the initial velocity to arc a player from a jump pad
 * origin to a target_position entity, similar to Q3's jump pad behavior.
 */

#include "p_internal.h"
#include <math.h>

vec3_t p_calc_launch_velocity(vec3_t start, vec3_t target, f32 gravity) {
    vec3_t delta = vec3_sub(target, start);

    f32 dx = delta.x;
    f32 dy = delta.y;
    f32 dz = delta.z;
    f32 horiz_dist = sqrtf(dx * dx + dy * dy);

    // Choose flight time based on horizontal distance, clamped to
    // a reasonable range. Q3 uses a fixed time of ~1s for short pads
    // and scales up for long distances. We use:
    //   t = sqrt(2 * horiz_dist / gravity)
    // clamped to [0.5, 2.5] seconds. This gives a natural parabolic
    // arc that scales with distance.
    f32 t;
    if (horiz_dist < 1.0f) {
        // Vertical jump pad (straight up)
        t = 1.0f;
    } else {
        t = sqrtf(2.0f * horiz_dist / gravity);
        if (t < 0.5f) t = 0.5f;
        if (t > 2.5f) t = 2.5f;
    }

    // Kinematic equation: dz = vz*t - 0.5*g*t^2
    // Solve for vz: vz = dz/t + 0.5*g*t
    f32 vz = dz / t + 0.5f * gravity * t;

    // Horizontal velocity: constant speed to cover horiz_dist in time t
    vec3_t result;
    if (horiz_dist > 1.0f) {
        f32 horiz_speed = horiz_dist / t;
        f32 inv_dist = 1.0f / horiz_dist;
        result.x = dx * inv_dist * horiz_speed;
        result.y = dy * inv_dist * horiz_speed;
    } else {
        result.x = 0.0f;
        result.y = 0.0f;
    }
    result.z = vz;

    return result;
}
