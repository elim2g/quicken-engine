/*
 * QUICKEN Engine - Brush Utilities
 *
 * AABB computation, overlap tests, swept AABB for broadphase.
 */

#include "p_internal.h"

/* ---- Compute tight AABB from brush planes ---- */

void p_brush_compute_aabb(qk_brush_t *brush) {
    /*
     * For a convex brush defined by planes, the AABB is found by testing
     * the six axis directions against all planes. For each axis direction d,
     * the min extent is: max over all planes of (plane.dist / dot(plane.normal, d))
     * when dot > 0, and similarly for max. This is the support function.
     *
     * Simpler approach: for each axis, find the tightest bound from each plane.
     * Each plane normal.axis component with dist gives us a bound.
     */
    f32 big = 1e18f;
    brush->mins = (vec3_t){ -big, -big, -big };
    brush->maxs = (vec3_t){  big,  big,  big };

    /* For each plane, project the halfspace onto each axis.
       For axis-aligned planes (e.g. normal=(1,0,0)), d/nx == d,
       which gives the exact bound. For angled planes, d/nx is
       a conservative (loose) bound. */
    for (u32 i = 0; i < brush->plane_count; i++) {
        const qk_plane_t *p = &brush->planes[i];
        f32 nx = p->normal.x;
        f32 ny = p->normal.y;
        f32 nz = p->normal.z;
        f32 d  = p->dist;

        /* If normal has a significant positive X component, the brush's
           max X cannot exceed d / nx (approximately). */
        if (nx > 0.01f) {
            f32 bound = d / nx;
            if (bound < brush->maxs.x) brush->maxs.x = bound;
        } else if (nx < -0.01f) {
            f32 bound = d / nx;
            if (bound > brush->mins.x) brush->mins.x = bound;
        }

        if (ny > 0.01f) {
            f32 bound = d / ny;
            if (bound < brush->maxs.y) brush->maxs.y = bound;
        } else if (ny < -0.01f) {
            f32 bound = d / ny;
            if (bound > brush->mins.y) brush->mins.y = bound;
        }

        if (nz > 0.01f) {
            f32 bound = d / nz;
            if (bound < brush->maxs.z) brush->maxs.z = bound;
        } else if (nz < -0.01f) {
            f32 bound = d / nz;
            if (bound > brush->mins.z) brush->mins.z = bound;
        }
    }
}

/* ---- AABB overlap test ---- */

bool p_aabb_overlap(vec3_t a_mins, vec3_t a_maxs,
                    vec3_t b_mins, vec3_t b_maxs) {
    if (a_maxs.x < b_mins.x || a_mins.x > b_maxs.x) return false;
    if (a_maxs.y < b_mins.y || a_mins.y > b_maxs.y) return false;
    if (a_maxs.z < b_mins.z || a_mins.z > b_maxs.z) return false;
    return true;
}

/* ---- Compute swept AABB for a moving box ---- */

void p_compute_swept_aabb(vec3_t start, vec3_t end,
                          vec3_t mins, vec3_t maxs,
                          vec3_t *out_mins, vec3_t *out_maxs) {
    /* The swept AABB covers both the start and end positions,
       expanded by the box extents. */
    f32 sx0 = start.x + mins.x;
    f32 sx1 = start.x + maxs.x;
    f32 ex0 = end.x + mins.x;
    f32 ex1 = end.x + maxs.x;

    f32 sy0 = start.y + mins.y;
    f32 sy1 = start.y + maxs.y;
    f32 ey0 = end.y + mins.y;
    f32 ey1 = end.y + maxs.y;

    f32 sz0 = start.z + mins.z;
    f32 sz1 = start.z + maxs.z;
    f32 ez0 = end.z + mins.z;
    f32 ez1 = end.z + maxs.z;

    out_mins->x = (sx0 < ex0) ? sx0 : ex0;
    out_mins->y = (sy0 < ey0) ? sy0 : ey0;
    out_mins->z = (sz0 < ez0) ? sz0 : ez0;

    out_maxs->x = (sx1 > ex1) ? sx1 : ex1;
    out_maxs->y = (sy1 > ey1) ? sy1 : ey1;
    out_maxs->z = (sz1 > ez1) ? sz1 : ez1;
}
