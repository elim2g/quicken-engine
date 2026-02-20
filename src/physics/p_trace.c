/*
 * QUICKEN Engine - Trace Implementation
 *
 * Sweep an AABB through the world. Single-brush trace (Quake CM_TraceThroughBrush),
 * world trace (brute force with AABB broadphase), and ground check.
 *
 * SIMD (SSE2/SSE4.1) is used for the inner loops where profitable:
 *   - Plane dot products and Minkowski expansion in p_trace_brush
 *   - Swept AABB computation and broadphase overlap in p_trace_world
 * All SIMD operations are IEEE-754 compliant (no rsqrt, rcp, or FMA)
 * to maintain cross-platform determinism under /fp:precise.
 */

#include "p_internal.h"
#include "p_simd.h"

// --- Trace against a single brush (Minkowski expansion) ---

qk_trace_result_t p_trace_brush(const qk_brush_t *brush,
                                vec3_t start, vec3_t end,
                                vec3_t mins, vec3_t maxs) {
    qk_trace_result_t result = {
        .fraction = 1.0f,
        .brush_index = -1,
        .entity_id = -1,
    };

    f32 enter_frac = -1.0f;
    f32 leave_frac = 1.0f;
    const qk_plane_t *clip_plane = NULL;

    bool starts_out = false;
    bool gets_out = false;

#if P_USE_SSE2
    // Pre-load vectors once outside the plane loop
    __m128 v_start = p_simd_load_vec3(start);
    __m128 v_end   = p_simd_load_vec3(end);
    __m128 v_mins  = p_simd_load_vec3(mins);
    __m128 v_maxs  = p_simd_load_vec3(maxs);

    for (u32 i = 0; i < brush->plane_count; i++) {
        const qk_plane_t *plane = &brush->planes[i];
        __m128 v_normal = p_simd_load_vec3(plane->normal);

        // Minkowski expansion: select support point per component
        // and dot with normal
        __m128 v_support = p_simd_support_point(v_normal, v_mins, v_maxs);
        f32 expand = p_simd_dot3(v_support, v_normal);
        f32 dist = plane->dist - expand;

        // Plane distances for start and end
        f32 d_start = p_simd_dot3(v_start, v_normal) - dist;
        f32 d_end   = p_simd_dot3(v_end,   v_normal) - dist;

        if (d_start > 0.0f) starts_out = true;
        if (d_end > 0.0f)   gets_out = true;

        // Both in front: completely outside this plane
        if (d_start > 0.0f && d_end >= d_start) {
            return result; // fraction = 1.0, no hit
        }

        // Both behind: inside this half-space, continue
        if (d_start <= 0.0f && d_end <= 0.0f) {
            continue;
        }

        // Crosses the plane
        f32 f;
        if (d_start > d_end) {
            // Entering the brush
            f = (d_start - QK_TRACE_EPSILON) / (d_start - d_end);
            if (f < 0.0f) f = 0.0f;
            if (f > enter_frac) {
                enter_frac = f;
                clip_plane = plane;
            }
        } else {
            // Leaving the brush
            f = (d_start + QK_TRACE_EPSILON) / (d_start - d_end);
            if (f > 1.0f) f = 1.0f;
            if (f < leave_frac) {
                leave_frac = f;
            }
        }
    }
#else
    // Scalar fallback
    for (u32 i = 0; i < brush->plane_count; i++) {
        const qk_plane_t *plane = &brush->planes[i];

        f32 sx = (plane->normal.x >= 0.0f) ? mins.x : maxs.x;
        f32 sy = (plane->normal.y >= 0.0f) ? mins.y : maxs.y;
        f32 sz = (plane->normal.z >= 0.0f) ? mins.z : maxs.z;
        f32 expand = sx * plane->normal.x
                   + sy * plane->normal.y
                   + sz * plane->normal.z;
        f32 dist = plane->dist - expand;

        f32 d_start = vec3_dot(start, plane->normal) - dist;
        f32 d_end   = vec3_dot(end,   plane->normal) - dist;

        if (d_start > 0.0f) starts_out = true;
        if (d_end > 0.0f)   gets_out = true;

        if (d_start > 0.0f && d_end >= d_start) {
            return result;
        }

        if (d_start <= 0.0f && d_end <= 0.0f) {
            continue;
        }

        f32 f;
        if (d_start > d_end) {
            f = (d_start - QK_TRACE_EPSILON) / (d_start - d_end);
            if (f < 0.0f) f = 0.0f;
            if (f > enter_frac) {
                enter_frac = f;
                clip_plane = plane;
            }
        } else {
            f = (d_start + QK_TRACE_EPSILON) / (d_start - d_end);
            if (f > 1.0f) f = 1.0f;
            if (f < leave_frac) {
                leave_frac = f;
            }
        }
    }
#endif

    if (!starts_out) {
        result.start_solid = true;
        if (!gets_out) {
            result.all_solid = true;
            result.fraction = 0.0f;
        }
        return result;
    }

    if (enter_frac < leave_frac) {
        if (enter_frac > -1.0f && enter_frac < result.fraction) {
            if (enter_frac < 0.0f) enter_frac = 0.0f;
            result.fraction = enter_frac;
            result.hit_normal = clip_plane->normal;
            result.hit_dist = clip_plane->dist;
            vec3_t delta = vec3_sub(end, start);
            result.end_pos = vec3_add(start, vec3_scale(delta, enter_frac));
        }
    }

    return result;
}

// --- World trace (all brushes, brute force + AABB broadphase) ---

qk_trace_result_t p_trace_world(const qk_phys_world_t *world,
                                vec3_t start, vec3_t end,
                                vec3_t mins, vec3_t maxs) {
    qk_trace_result_t best = {
        .fraction = 1.0f,
        .end_pos = end,
        .brush_index = -1,
        .entity_id = -1,
    };

    if (!world || !world->cm) {
        return best;
    }

#if P_USE_SSE2
    // SIMD swept AABB computation
    __m128 v_start = p_simd_load_vec3(start);
    __m128 v_end   = p_simd_load_vec3(end);
    __m128 v_mins  = p_simd_load_vec3(mins);
    __m128 v_maxs  = p_simd_load_vec3(maxs);

    __m128 swept_mins_v, swept_maxs_v;
    p_simd_swept_aabb(v_start, v_end, v_mins, v_maxs,
                      &swept_mins_v, &swept_maxs_v);

    for (u32 i = 0; i < world->cm->brush_count; i++) {
        const qk_brush_t *brush = &world->cm->brushes[i];

        // SIMD broadphase: AABB overlap test
        __m128 brush_mins = p_simd_load_vec3(brush->mins);
        __m128 brush_maxs = p_simd_load_vec3(brush->maxs);
        if (!p_simd_aabb_overlap(swept_mins_v, swept_maxs_v,
                                 brush_mins, brush_maxs)) {
            continue;
        }

        qk_trace_result_t result = p_trace_brush(brush, start, end, mins, maxs);

        if (result.all_solid) {
            result.brush_index = (i32)i;
            result.entity_id = -1;
            return result;
        }

        if (result.fraction < best.fraction) {
            best = result;
            best.brush_index = (i32)i;
            best.entity_id = -1;
        }
    }
#else
    // Scalar fallback
    vec3_t swept_mins, swept_maxs;
    p_compute_swept_aabb(start, end, mins, maxs, &swept_mins, &swept_maxs);

    for (u32 i = 0; i < world->cm->brush_count; i++) {
        const qk_brush_t *brush = &world->cm->brushes[i];

        if (!p_aabb_overlap(swept_mins, swept_maxs, brush->mins, brush->maxs)) {
            continue;
        }

        qk_trace_result_t result = p_trace_brush(brush, start, end, mins, maxs);

        if (result.all_solid) {
            result.brush_index = (i32)i;
            result.entity_id = -1;
            return result;
        }

        if (result.fraction < best.fraction) {
            best = result;
            best.brush_index = (i32)i;
            best.entity_id = -1;
        }
    }
#endif

    // Compute final end position from fraction
    best.end_pos.x = start.x + best.fraction * (end.x - start.x);
    best.end_pos.y = start.y + best.fraction * (end.y - start.y);
    best.end_pos.z = start.z + best.fraction * (end.z - start.z);

    return best;
}
