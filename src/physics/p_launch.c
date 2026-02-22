/*
 * QUICKEN Engine - Jump Pad Launch Velocity
 *
 * Calculates the initial velocity to arc a player from a jump pad
 * origin to a target_position entity.
 *
 * Matches Q3's AimAtTarget: the target_position marks the APEX of the
 * arc, not a pass-through point.  The player arrives at the target
 * height with zero vertical velocity, then falls to the landing spot.
 */

#include "p_internal.h"
#include <math.h>

vec3_t p_calc_launch_velocity(vec3_t start, vec3_t target, f32 gravity) {
    vec3_t delta = vec3_sub(target, start);

    // Height to the apex (target).  Clamp to a small positive value so
    // degenerate pads (target at or below the pad) don't produce NaN.
    f32 height = delta.z;
    if (height < 1.0f) height = 1.0f;

    // Time to reach apex under gravity: h = 0.5*g*t^2  =>  t = sqrt(2h/g)
    // (Q3 writes this as sqrt(height / (0.5 * gravity)), same thing.)
    f32 time = sqrtf(2.0f * height / gravity);

    // Vertical impulse: player reaches zero vz exactly at apex height
    f32 vz = time * gravity;

    // Horizontal velocity: constant speed to cover XY distance by apex time
    f32 horiz_dist = sqrtf(delta.x * delta.x + delta.y * delta.y);

    vec3_t result;
    if (horiz_dist > 1.0f) {
        f32 horiz_speed = horiz_dist / time;
        f32 inv_dist = 1.0f / horiz_dist;
        result.x = delta.x * inv_dist * horiz_speed;
        result.y = delta.y * inv_dist * horiz_speed;
    } else {
        result.x = 0.0f;
        result.y = 0.0f;
    }
    result.z = vz;

    return result;
}
