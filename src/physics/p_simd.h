/*
 * QUICKEN Engine - Physics SIMD Helpers
 *
 * SSE2 helpers for the physics hot paths. SSE2 is guaranteed on all
 * x86_64 targets, so no compile-time feature detection is needed.
 *
 * These produce bit-identical results to scalar code because only
 * IEEE-754 compliant SSE operations are used (no rsqrt, rcp, or FMA).
 *
 * Usage rules:
 *   - SAFE:   _mm_add_ps, _mm_sub_ps, _mm_mul_ps, _mm_div_ps, _mm_sqrt_ps
 *   - SAFE:   _mm_min_ps, _mm_max_ps, _mm_cmp*_ps, _mm_and_ps, _mm_or_ps
 *   - UNSAFE: _mm_rsqrt_ps, _mm_rcp_ps (approximate -- NOT deterministic)
 *   - UNSAFE: _mm_fmadd_ps (FMA changes results from a*b+c)
 */

#ifndef P_SIMD_H
#define P_SIMD_H

#include "quicken.h"
#include "qk_math.h"
#include <emmintrin.h>  /* SSE2 -- guaranteed on x64 */

// --- Load/Store: vec3_t <-> __m128 ---

// Load a vec3_t into an __m128 with the 4th element set to 0.
// vec3_t is 12 bytes, not aligned to 16. We load x,y,z individually.
static inline __m128 p_simd_load_vec3(vec3_t v) {
    return _mm_set_ps(0.0f, v.z, v.y, v.x);
}

// Store the lower 3 floats of an __m128 back to a vec3_t.
static inline vec3_t p_simd_store_vec3(__m128 v) {
    // _MM_SHUFFLE: we only need the first 3 elements
    vec3_t result;
    _mm_store_ss(&result.x, v);
    _mm_store_ss(&result.y, _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)));
    _mm_store_ss(&result.z, _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2)));
    return result;
}

// --- Dot product (vec3 only, ignores w) ---

// SSE2: mul + horizontal add
static inline f32 p_simd_dot3(__m128 a, __m128 b) {
    __m128 mul = _mm_mul_ps(a, b);
    // mul = [x*x, y*y, z*z, 0]
    __m128 shuf1 = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(0, 0, 0, 1)); // [y*y, ...]
    __m128 shuf2 = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(0, 0, 0, 2)); // [z*z, ...]
    __m128 sum = _mm_add_ss(mul, shuf1);
    sum = _mm_add_ss(sum, shuf2);
    return _mm_cvtss_f32(sum);
}

// --- Dot product returning __m128 broadcast (for further SIMD ops) ---

static inline __m128 p_simd_dot3_broadcast(__m128 a, __m128 b) {
    __m128 mul = _mm_mul_ps(a, b);
    __m128 shuf1 = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(0, 0, 0, 1));
    __m128 shuf2 = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(0, 0, 0, 2));
    __m128 sum = _mm_add_ss(mul, shuf1);
    sum = _mm_add_ss(sum, shuf2);
    return _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(0, 0, 0, 0));
}

// --- AABB overlap test (all 6 separating axis comparisons) ---

static inline bool p_simd_aabb_overlap(__m128 a_mins, __m128 a_maxs,
                                       __m128 b_mins, __m128 b_maxs) {
    // Test: a_maxs < b_mins || a_mins > b_maxs (any component = separated)
    __m128 sep1 = _mm_cmplt_ps(a_maxs, b_mins);
    __m128 sep2 = _mm_cmplt_ps(b_maxs, a_mins);
    __m128 sep = _mm_or_ps(sep1, sep2);
    // Check only xyz lanes (mask 0x7 = bits 0,1,2)
    int mask = _mm_movemask_ps(sep) & 0x7;
    return mask == 0;
}

// --- Swept AABB (min/max of start+offset and end+offset) ---

static inline void p_simd_swept_aabb(__m128 start, __m128 end,
                                     __m128 mins, __m128 maxs,
                                     __m128 *out_mins, __m128 *out_maxs) {
    __m128 s0 = _mm_add_ps(start, mins);
    __m128 s1 = _mm_add_ps(start, maxs);
    __m128 e0 = _mm_add_ps(end, mins);
    __m128 e1 = _mm_add_ps(end, maxs);
    *out_mins = _mm_min_ps(s0, e0);
    *out_maxs = _mm_max_ps(s1, e1);
}

// --- Clip velocity against a plane normal (overbounce) ---

static inline __m128 p_simd_clip_velocity(__m128 velocity, __m128 normal,
                                          f32 overbounce) {
    // backoff = dot(velocity, normal) * overbounce
    f32 backoff = p_simd_dot3(velocity, normal) * overbounce;
    __m128 vbackoff = _mm_set1_ps(backoff);
    // result = velocity - normal * backoff
    __m128 result = _mm_sub_ps(velocity, _mm_mul_ps(normal, vbackoff));

    // Cleanup: if dot(result, normal) < 0, remove residual
    f32 adjust = p_simd_dot3(result, normal);
    if (adjust < 0.0f) {
        __m128 vadj = _mm_set1_ps(adjust);
        result = _mm_sub_ps(result, _mm_mul_ps(normal, vadj));
    }
    return result;
}

// --- Plane distance: dot(point, normal) - dist ---

static inline f32 p_simd_plane_dist(__m128 point, __m128 normal, f32 dist) {
    return p_simd_dot3(point, normal) - dist;
}

// --- Select support point for Minkowski expansion ---

// For each component of normal, select mins if >= 0, else maxs.
// Returns the support point as __m128.
static inline __m128 p_simd_support_point(__m128 normal, __m128 mins, __m128 maxs) {
    __m128 zero = _mm_setzero_ps();
    // mask = (normal >= 0) ? 0xFFFFFFFF : 0
    __m128 mask = _mm_cmpge_ps(normal, zero);
    // result = mask ? mins : maxs  (SSE2 and/andnot/or blend)
    return _mm_or_ps(_mm_and_ps(mask, mins), _mm_andnot_ps(mask, maxs));
}

#endif // P_SIMD_H
