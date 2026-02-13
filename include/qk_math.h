/*
 * QUICKEN Engine - Shared Math Types
 *
 * vec3_t, bbox_t, and inline math operations.
 * Safe under both fast and precise float (add, sub, mul, dot, cross, length, normalize).
 */

#ifndef QK_MATH_H
#define QK_MATH_H

#include "quicken.h"
#include <math.h>

typedef struct { f32 x, y, z; } vec3_t;
typedef struct { vec3_t min, max; } bbox_t;

static inline vec3_t vec3_add(vec3_t a, vec3_t b) {
    return (vec3_t){ a.x + b.x, a.y + b.y, a.z + b.z };
}

static inline vec3_t vec3_sub(vec3_t a, vec3_t b) {
    return (vec3_t){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static inline vec3_t vec3_scale(vec3_t v, f32 s) {
    return (vec3_t){ v.x * s, v.y * s, v.z * s };
}

static inline f32 vec3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3_t vec3_cross(vec3_t a, vec3_t b) {
    return (vec3_t){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static inline f32 vec3_length(vec3_t v) {
    return sqrtf(vec3_dot(v, v));
}

static inline vec3_t vec3_normalize(vec3_t v) {
    f32 len = vec3_length(v);
    if (len > 0.0f) {
        f32 inv = 1.0f / len;
        return vec3_scale(v, inv);
    }
    return (vec3_t){ 0.0f, 0.0f, 0.0f };
}

#endif /* QK_MATH_H */
