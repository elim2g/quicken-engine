/*
 * QUICKEN Engine - Brush Utilities
 *
 * AABB computation, overlap tests, swept AABB for broadphase.
 */

#include "p_internal.h"
#include "p_simd.h"
#include <stdlib.h>

// --- Compute tight AABB from brush plane intersections ---

void p_brush_compute_aabb(qk_brush_t *brush) {
    /*
     * Compute the tight AABB by finding all vertices of the convex brush.
     * A vertex exists at each triple of planes whose normals are linearly
     * independent, AND the resulting point lies on the inside (or boundary)
     * of every other plane.
     *
     * This is exact for any convex brush, including those with only angled
     * planes (ramps, wedges, bevels) common in real Quake maps.
     */
    f32 big = 1e18f;
    brush->mins = (vec3_t){  big,  big,  big };
    brush->maxs = (vec3_t){ -big, -big, -big };

    bool found_vertex = false;
    u32 n = brush->plane_count;

    for (u32 i = 0; i < n; i++) {
        for (u32 j = i + 1; j < n; j++) {
            for (u32 k = j + 1; k < n; k++) {
                // Solve the 3x3 linear system:
                //   n_i . p = d_i
                //   n_j . p = d_j
                //   n_k . p = d_k
                // Using Cramer's rule.
                const vec3_t *ni = &brush->planes[i].normal;
                const vec3_t *nj = &brush->planes[j].normal;
                const vec3_t *nk = &brush->planes[k].normal;

                // Determinant = ni . (nj x nk)
                vec3_t jxk = vec3_cross(*nj, *nk);
                f32 det = vec3_dot(*ni, jxk);

                // If det is near zero, planes are coplanar or parallel
                if (det > -1e-6f && det < 1e-6f) continue;

                f32 inv_det = 1.0f / det;
                f32 di = brush->planes[i].dist;
                f32 dj = brush->planes[j].dist;
                f32 dk = brush->planes[k].dist;

                // p = (di * (nj x nk) + dj * (nk x ni) + dk * (ni x nj)) / det
                vec3_t kxi = vec3_cross(*nk, *ni);
                vec3_t ixj = vec3_cross(*ni, *nj);

                vec3_t pt;
                pt.x = (di * jxk.x + dj * kxi.x + dk * ixj.x) * inv_det;
                pt.y = (di * jxk.y + dj * kxi.y + dk * ixj.y) * inv_det;
                pt.z = (di * jxk.z + dj * kxi.z + dk * ixj.z) * inv_det;

                // Verify this point is inside (or on boundary of) all planes.
                // A point is inside plane m if: nm . pt <= dm + epsilon
                bool inside = true;
                __m128 v_pt = p_simd_load_vec3(pt);
                for (u32 m = 0; m < n; m++) {
                    __m128 v_nm = p_simd_load_vec3(brush->planes[m].normal);
                    f32 d = p_simd_dot3(v_pt, v_nm) - brush->planes[m].dist;
                    if (d > 0.1f) {
                        inside = false;
                        break;
                    }
                }

                if (!inside) continue;

                // Expand AABB to include this vertex
                if (pt.x < brush->mins.x) brush->mins.x = pt.x;
                if (pt.y < brush->mins.y) brush->mins.y = pt.y;
                if (pt.z < brush->mins.z) brush->mins.z = pt.z;
                if (pt.x > brush->maxs.x) brush->maxs.x = pt.x;
                if (pt.y > brush->maxs.y) brush->maxs.y = pt.y;
                if (pt.z > brush->maxs.z) brush->maxs.z = pt.z;
                found_vertex = true;
            }
        }
    }

    if (!found_vertex) {
        // Degenerate brush (e.g. fewer than 4 planes). Zero-size AABB at origin.
        brush->mins = (vec3_t){ 0.0f, 0.0f, 0.0f };
        brush->maxs = (vec3_t){ 0.0f, 0.0f, 0.0f };
    }
}

// --- Add axial bevel planes to a brush ---

void p_brush_add_bevels(qk_brush_t *brush) {
    /*
     * Raw .map brushes may lack axis-aligned planes. The Minkowski-expanded
     * box trace can miss edges where two angled planes meet without an
     * axial plane between them. Add bevel planes at the brush AABB extents
     * for any missing axis directions.
     *
     * This must be called AFTER p_brush_compute_aabb (needs valid mins/maxs).
     */
    bool has_pos_x = false, has_neg_x = false;
    bool has_pos_y = false, has_neg_y = false;
    bool has_pos_z = false, has_neg_z = false;

    for (u32 i = 0; i < brush->plane_count; i++) {
        const vec3_t *n = &brush->planes[i].normal;
        if (n->x >  0.999f) has_pos_x = true;
        if (n->x < -0.999f) has_neg_x = true;
        if (n->y >  0.999f) has_pos_y = true;
        if (n->y < -0.999f) has_neg_y = true;
        if (n->z >  0.999f) has_pos_z = true;
        if (n->z < -0.999f) has_neg_z = true;
    }

    // Count how many bevels we need to add
    u32 bevels_needed = 0;
    if (!has_pos_x) bevels_needed++;
    if (!has_neg_x) bevels_needed++;
    if (!has_pos_y) bevels_needed++;
    if (!has_neg_y) bevels_needed++;
    if (!has_pos_z) bevels_needed++;
    if (!has_neg_z) bevels_needed++;

    if (bevels_needed == 0) return;

    // Reallocate planes array to accommodate bevels
    u32 new_count = brush->plane_count + bevels_needed;
    qk_plane_t *new_planes = (qk_plane_t *)realloc(
        brush->planes, new_count * sizeof(qk_plane_t));
    if (!new_planes) return; // allocation failed, skip bevels

    brush->planes = new_planes;
    u32 idx = brush->plane_count;

    if (!has_pos_x) {
        brush->planes[idx].normal = (vec3_t){ 1.0f, 0.0f, 0.0f };
        brush->planes[idx].dist = brush->maxs.x;
        idx++;
    }
    if (!has_neg_x) {
        brush->planes[idx].normal = (vec3_t){-1.0f, 0.0f, 0.0f };
        brush->planes[idx].dist = -brush->mins.x;
        idx++;
    }
    if (!has_pos_y) {
        brush->planes[idx].normal = (vec3_t){ 0.0f, 1.0f, 0.0f };
        brush->planes[idx].dist = brush->maxs.y;
        idx++;
    }
    if (!has_neg_y) {
        brush->planes[idx].normal = (vec3_t){ 0.0f,-1.0f, 0.0f };
        brush->planes[idx].dist = -brush->mins.y;
        idx++;
    }
    if (!has_pos_z) {
        brush->planes[idx].normal = (vec3_t){ 0.0f, 0.0f, 1.0f };
        brush->planes[idx].dist = brush->maxs.z;
        idx++;
    }
    if (!has_neg_z) {
        brush->planes[idx].normal = (vec3_t){ 0.0f, 0.0f,-1.0f };
        brush->planes[idx].dist = -brush->mins.z;
        idx++;
    }

    brush->plane_count = idx;
}

// --- AABB overlap test ---

bool p_aabb_overlap(vec3_t a_mins, vec3_t a_maxs,
                    vec3_t b_mins, vec3_t b_maxs) {
    return p_simd_aabb_overlap(p_simd_load_vec3(a_mins), p_simd_load_vec3(a_maxs),
                               p_simd_load_vec3(b_mins), p_simd_load_vec3(b_maxs));
}

// --- Compute swept AABB for a moving box ---

void p_compute_swept_aabb(vec3_t start, vec3_t end,
                          vec3_t mins, vec3_t maxs,
                          vec3_t *out_mins, vec3_t *out_maxs) {
    __m128 v_start = p_simd_load_vec3(start);
    __m128 v_end   = p_simd_load_vec3(end);
    __m128 v_mins  = p_simd_load_vec3(mins);
    __m128 v_maxs  = p_simd_load_vec3(maxs);
    __m128 r_mins, r_maxs;
    p_simd_swept_aabb(v_start, v_end, v_mins, v_maxs, &r_mins, &r_maxs);
    *out_mins = p_simd_store_vec3(r_mins);
    *out_maxs = p_simd_store_vec3(r_maxs);
}
