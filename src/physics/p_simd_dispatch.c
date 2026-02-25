/*
 * QUICKEN Engine - Physics SIMD Dispatch Initialization
 *
 * Populates the global dispatch table based on detected CPU features.
 * On x64, SSE2 is always available, so all function pointers resolve
 * to the SSE2 implementations. The dispatch structure is in place for
 * future AVX2 fast-paths that can be compiled in separate TUs with
 * /arch:AVX2.
 */

#include "p_simd_dispatch.h"
#include "p_internal.h"
#include "core/qk_simd_dispatch.h"

/* The dispatch table -- zero-initialized until p_simd_dispatch_init(). */
p_simd_dispatch_t g_p_dispatch;

/*
 * On x64, SSE2 is baseline. The "scalar" and "SSE2" paths point to the
 * same functions because p_trace.c / p_slide.c / p_brush.c are already
 * compiled with SSE2 intrinsics unconditionally.
 *
 * The extern prototypes reference the functions declared in p_internal.h:
 *   p_trace_brush, p_trace_world, p_clip_velocity, p_aabb_overlap
 */

void p_simd_dispatch_init(void) {
    qk_simd_tier_t tier = qk_simd_get_tier();
    (void)tier; /* Currently unused -- all tiers map to SSE2 */

    /* SSE2 is always available on x64. Populate with the SSE2
     * implementations (which are the only implementations now). */
    g_p_dispatch.trace_brush    = p_trace_brush;
    g_p_dispatch.trace_world    = p_trace_world;
    g_p_dispatch.clip_velocity  = p_clip_velocity;
    g_p_dispatch.aabb_overlap   = p_aabb_overlap;
}
