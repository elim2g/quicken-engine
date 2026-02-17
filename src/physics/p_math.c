/*
 * QUICKEN Engine - Physics Deterministic Math
 *
 * Portable sinf/cosf (minimax polynomial) and angle_vectors.
 * Never uses <math.h> sinf/cosf in gameplay paths.
 */

#include "p_internal.h"

/* ---- Deterministic sinf ---- */

f32 p_sinf(f32 x) {
    /* Reduce x to [-PI, PI] without integer cast (avoids i32 overflow) */
    while (x >  P_PI) x -= P_2PI;
    while (x < -P_PI) x += P_2PI;

    /* Reduce to [-PI/2, PI/2] using sin(x) = sin(PI - x) reflection.
       The 7th-order polynomial is only accurate near zero; without this
       step, values near +-PI produce catastrophic errors (wrong sign for
       cosine of angles near 90 deg, which breaks strafejumping). */
    if (x > P_PI * 0.5f) {
        x = P_PI - x;
    } else if (x < -P_PI * 0.5f) {
        x = -P_PI - x;
    }

    /* 7th-order minimax polynomial: sin(x) ~ x(1 - x^2/6 + x^4/120 - x^6/5040) */
    f32 x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f -
           x2 * (1.0f / 5040.0f))));
}

/* ---- Deterministic cosf ---- */

f32 p_cosf(f32 x) {
    return p_sinf(x + (P_PI * 0.5f));
}

/* ---- Angle to direction vectors ---- */

void p_angle_vectors(f32 pitch, f32 yaw,
                     vec3_t *forward, vec3_t *right, vec3_t *up) {
    f32 sp = p_sinf(pitch * P_DEG2RAD);
    f32 cp = p_cosf(pitch * P_DEG2RAD);
    f32 sy = p_sinf(yaw * P_DEG2RAD);
    f32 cy = p_cosf(yaw * P_DEG2RAD);

    if (forward) {
        forward->x = cp * cy;
        forward->y = cp * sy;
        forward->z = sp;
    }
    if (right) {
        right->x = sy;
        right->y = -cy;
        right->z = 0.0f;
    }
    if (up) {
        up->x = -sp * cy;
        up->y = -sp * sy;
        up->z = cp;
    }
}
