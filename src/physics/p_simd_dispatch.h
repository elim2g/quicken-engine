/*
 * QUICKEN Engine - Physics SIMD Dispatch Table
 *
 * Runtime dispatch for physics functions based on detected SIMD tier.
 * Currently all x64 CPUs have SSE2, so the SSE2 path is always taken.
 * The dispatch structure is in place for future AVX2 fast-paths.
 */

#ifndef P_SIMD_DISPATCH_H
#define P_SIMD_DISPATCH_H

#include "quicken.h"
#include "qk_math.h"
#include "qk_types.h"
#include "physics/qk_physics.h"

typedef struct {
    qk_trace_result_t (*trace_brush)(const qk_brush_t *brush,
                                      vec3_t start, vec3_t end,
                                      vec3_t mins, vec3_t maxs);
    qk_trace_result_t (*trace_world)(const qk_phys_world_t *world,
                                      vec3_t start, vec3_t end,
                                      vec3_t mins, vec3_t maxs);
    vec3_t (*clip_velocity)(vec3_t velocity, vec3_t normal, f32 overbounce);
    bool (*aabb_overlap)(vec3_t a_mins, vec3_t a_maxs,
                         vec3_t b_mins, vec3_t b_maxs);
} p_simd_dispatch_t;

extern p_simd_dispatch_t g_p_dispatch;

/* Initialize the dispatch table based on runtime SIMD tier.
 * Must be called after qk_cpuid_detect(). */
void p_simd_dispatch_init(void);

#endif /* P_SIMD_DISPATCH_H */
