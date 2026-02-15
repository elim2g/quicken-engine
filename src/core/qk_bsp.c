/*
 * QUICKEN Engine - Quake 3 BSP Loader
 *
 * Parses IBSP v46/v47 binary format and produces the same qk_map_data_t
 * output as the text .map loader: collision brushes, render geometry,
 * and spawn points.
 */

#include "core/qk_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Q3 BSP format constants ---- */

#define BSP_MAGIC           0x50534249  /* "IBSP" little-endian */
#define BSP_VERSION_46      46
#define BSP_VERSION_47      47
#define BSP_LUMP_COUNT      17

#define LUMP_ENTITIES       0
#define LUMP_TEXTURES       1
#define LUMP_PLANES         2
#define LUMP_BRUSHES        8
#define LUMP_BRUSHSIDES     9
#define LUMP_VERTICES       10
#define LUMP_MESHVERTS      11
#define LUMP_FACES          13

#define Q3_CONTENTS_SOLID       0x1
#define Q3_CONTENTS_PLAYERCLIP  0x10000

#define Q3_SURF_NODRAW          0x80
#define Q3_SURF_SKY             0x4

#define BSP_PLANE_EPSILON   0.01f

/* ---- On-disk structures (packed to match BSP binary layout) ---- */

#pragma pack(push, 1)

typedef struct { i32 offset; i32 length; } bsp_lump_t;

typedef struct {
    i32         magic;
    i32         version;
    bsp_lump_t  lumps[BSP_LUMP_COUNT];
} bsp_header_t;

typedef struct {
    char    name[64];
    i32     flags;
    i32     contents;
} bsp_texture_t;

typedef struct {
    f32     normal[3];
    f32     dist;
} bsp_plane_t;

typedef struct {
    i32     first_side;
    i32     num_sides;
    i32     texture;
} bsp_brush_t;

typedef struct {
    i32     plane;
    i32     texture;
} bsp_brushside_t;

typedef struct {
    f32     position[3];
    f32     st[2];
    f32     lm_st[2];
    f32     normal[3];
    u8      color[4];
} bsp_vertex_t;

typedef struct {
    i32     offset;
} bsp_meshvert_t;

typedef struct {
    i32     texture;
    i32     effect;
    i32     type;
    i32     vertex;
    i32     n_verts;
    i32     meshvert;
    i32     n_meshverts;
    i32     lm_index;
    i32     lm_start[2];
    i32     lm_size[2];
    f32     lm_origin[3];
    f32     lm_vecs[2][3];
    f32     normal[3];
    i32     size[2];
} bsp_face_t;

#pragma pack(pop)

_Static_assert(sizeof(bsp_texture_t)  == 72,  "bsp_texture_t packing");
_Static_assert(sizeof(bsp_plane_t)    == 16,  "bsp_plane_t packing");
_Static_assert(sizeof(bsp_brush_t)    == 12,  "bsp_brush_t packing");
_Static_assert(sizeof(bsp_brushside_t)== 8,   "bsp_brushside_t packing");
_Static_assert(sizeof(bsp_vertex_t)   == 44,  "bsp_vertex_t packing");
_Static_assert(sizeof(bsp_meshvert_t) == 4,   "bsp_meshvert_t packing");
_Static_assert(sizeof(bsp_face_t)     == 104, "bsp_face_t packing");
_Static_assert(sizeof(bsp_header_t)   == 144, "bsp_header_t packing");

/* ---- Lump access helper ---- */

static const void *get_lump(const u8 *data, u64 data_len,
                             const bsp_header_t *hdr, int lump_idx,
                             u32 elem_size, u32 *out_count) {
    const bsp_lump_t *l = &hdr->lumps[lump_idx];
    if (l->offset < 0 || l->length < 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if ((u64)l->offset + (u64)l->length > data_len) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) {
        *out_count = (elem_size > 0) ? (u32)((u32)l->length / elem_size) : 0;
    }
    if (l->length == 0) return NULL;
    return data + l->offset;
}

/* ---- Tool texture filter ---- */

static bool is_tool_texture(const char *name) {
    return strstr(name, "skip") || strstr(name, "clip") ||
           strstr(name, "trigger") || strstr(name, "hint") ||
           strstr(name, "nodraw") || strstr(name, "caulk");
}

/* ---- World-space planar UV (for grid texture tiling) ---- */

static void world_uv(const f32 *pos, const f32 *nrm, f32 *uv) {
    f32 ax = fabsf(nrm[0]), ay = fabsf(nrm[1]), az = fabsf(nrm[2]);
    if (az >= ax && az >= ay) {
        uv[0] = pos[0] / 64.0f;
        uv[1] = pos[1] / 64.0f;
    } else if (ax >= ay) {
        uv[0] = pos[1] / 64.0f;
        uv[1] = pos[2] / 64.0f;
    } else {
        uv[0] = pos[0] / 64.0f;
        uv[1] = pos[2] / 64.0f;
    }
}

/* ---- Bezier patch tessellation ---- */

#define BSP_TESS_LEVEL 4
#define BSP_TESS_VERTS ((BSP_TESS_LEVEL + 1) * (BSP_TESS_LEVEL + 1))
#define BSP_TESS_IDXS  (BSP_TESS_LEVEL * BSP_TESS_LEVEL * 6)

static void bezier_eval(const bsp_vertex_t *cp, i32 stride,
                         f32 u, f32 v, f32 *out_pos, f32 *out_nrm) {
    f32 au = (1-u)*(1-u), bu = 2*(1-u)*u, cu = u*u;
    f32 av = (1-v)*(1-v), bv = 2*(1-v)*v, cv = v*v;

    f32 tp[3][3], tn[3][3];
    for (int r = 0; r < 3; r++) {
        const bsp_vertex_t *p0 = &cp[r * stride + 0];
        const bsp_vertex_t *p1 = &cp[r * stride + 1];
        const bsp_vertex_t *p2 = &cp[r * stride + 2];
        for (int i = 0; i < 3; i++) {
            tp[r][i] = au * p0->position[i] + bu * p1->position[i] + cu * p2->position[i];
            tn[r][i] = au * p0->normal[i]   + bu * p1->normal[i]   + cu * p2->normal[i];
        }
    }

    f32 len = 0;
    for (int i = 0; i < 3; i++) {
        out_pos[i] = av * tp[0][i] + bv * tp[1][i] + cv * tp[2][i];
        out_nrm[i] = av * tn[0][i] + bv * tn[1][i] + cv * tn[2][i];
        len += out_nrm[i] * out_nrm[i];
    }
    len = sqrtf(len);
    if (len > 0.0001f) { out_nrm[0] /= len; out_nrm[1] /= len; out_nrm[2] /= len; }
}

/* ---- Build collision model from BSP brushes ---- */

static qk_result_t build_bsp_collision(
    const bsp_texture_t *textures, u32 tex_count,
    const bsp_plane_t *planes, u32 plane_count,
    const bsp_brush_t *brushes, u32 brush_count,
    const bsp_brushside_t *sides, u32 side_count,
    qk_collision_model_t *cm)
{
    /* Count solid brushes */
    u32 solid_count = 0;
    for (u32 i = 0; i < brush_count; i++) {
        i32 ti = brushes[i].texture;
        if (ti < 0 || (u32)ti >= tex_count) continue;
        if (textures[ti].contents & (Q3_CONTENTS_SOLID | Q3_CONTENTS_PLAYERCLIP))
            solid_count++;
    }

    if (solid_count == 0) return QK_ERROR_NOT_FOUND;

    cm->brushes = (qk_brush_t *)calloc(solid_count, sizeof(qk_brush_t));
    if (!cm->brushes) return QK_ERROR_OUT_OF_MEMORY;
    cm->brush_count = 0;

    for (u32 i = 0; i < brush_count; i++) {
        const bsp_brush_t *bb = &brushes[i];
        i32 ti = bb->texture;
        if (ti < 0 || (u32)ti >= tex_count) continue;
        if (!(textures[ti].contents & (Q3_CONTENTS_SOLID | Q3_CONTENTS_PLAYERCLIP))) continue;
        if (bb->num_sides <= 0) continue;
        if (bb->first_side < 0 || (u32)(bb->first_side + bb->num_sides) > side_count) continue;

        qk_brush_t *ob = &cm->brushes[cm->brush_count];
        ob->planes = (qk_plane_t *)calloc((u32)bb->num_sides, sizeof(qk_plane_t));
        if (!ob->planes) return QK_ERROR_OUT_OF_MEMORY;
        ob->plane_count = (u32)bb->num_sides;

        /* Copy planes from BSP (already outward-facing) */
        for (i32 s = 0; s < bb->num_sides; s++) {
            const bsp_brushside_t *bs = &sides[bb->first_side + s];
            if (bs->plane < 0 || (u32)bs->plane >= plane_count) {
                ob->planes[s].normal = (vec3_t){0, 0, 1};
                ob->planes[s].dist = 0;
                continue;
            }
            const bsp_plane_t *bp = &planes[bs->plane];
            ob->planes[s].normal = (vec3_t){bp->normal[0], bp->normal[1], bp->normal[2]};
            ob->planes[s].dist = bp->dist;
        }

        /* Compute AABB from plane intersections */
        ob->mins = (vec3_t){ 1e18f,  1e18f,  1e18f};
        ob->maxs = (vec3_t){-1e18f, -1e18f, -1e18f};
        bool has_vertex = false;

        for (u32 a = 0; a < ob->plane_count; a++) {
            for (u32 b = a + 1; b < ob->plane_count; b++) {
                for (u32 c = b + 1; c < ob->plane_count; c++) {
                    vec3_t n1 = ob->planes[a].normal;
                    vec3_t n2 = ob->planes[b].normal;
                    vec3_t n3 = ob->planes[c].normal;

                    vec3_t bc_cross = vec3_cross(n2, n3);
                    f32 denom = vec3_dot(n1, bc_cross);
                    if (fabsf(denom) < 0.001f) continue;

                    vec3_t ca_cross = vec3_cross(n3, n1);
                    vec3_t ab_cross = vec3_cross(n1, n2);
                    vec3_t v = vec3_scale(
                        vec3_add(vec3_add(
                            vec3_scale(bc_cross, ob->planes[a].dist),
                            vec3_scale(ca_cross, ob->planes[b].dist)),
                            vec3_scale(ab_cross, ob->planes[c].dist)),
                        1.0f / denom);

                    /* Check vertex is inside brush */
                    bool valid = true;
                    for (u32 q = 0; q < ob->plane_count; q++) {
                        f32 d = vec3_dot(ob->planes[q].normal, v) - ob->planes[q].dist;
                        if (d > BSP_PLANE_EPSILON) { valid = false; break; }
                    }
                    if (!valid) continue;

                    if (v.x < ob->mins.x) ob->mins.x = v.x;
                    if (v.y < ob->mins.y) ob->mins.y = v.y;
                    if (v.z < ob->mins.z) ob->mins.z = v.z;
                    if (v.x > ob->maxs.x) ob->maxs.x = v.x;
                    if (v.y > ob->maxs.y) ob->maxs.y = v.y;
                    if (v.z > ob->maxs.z) ob->maxs.z = v.z;
                    has_vertex = true;
                }
            }
        }

        if (!has_vertex) {
            free(ob->planes);
            ob->planes = NULL;
            continue;
        }

        cm->brush_count++;
    }

    return QK_SUCCESS;
}

/* ---- Build patch collision (thin slab brushes from tessellated quads) ---- */

#define PATCH_SLAB_THICKNESS 2.0f

static void build_patch_collision(
    const bsp_texture_t *textures, u32 tex_count,
    const bsp_vertex_t *verts, u32 vert_count,
    const bsp_face_t *faces, u32 face_count,
    qk_collision_model_t *cm)
{
    /* Count how many quads we'll generate */
    u32 total_quads = 0;
    for (u32 i = 0; i < face_count; i++) {
        const bsp_face_t *f = &faces[i];
        if (f->type != 2 || f->size[0] < 3 || f->size[1] < 3) continue;
        if (f->texture < 0 || (u32)f->texture >= tex_count) continue;
        if (is_tool_texture(textures[f->texture].name)) continue;
        u32 pu = ((u32)f->size[0] - 1) / 2;
        u32 pv = ((u32)f->size[1] - 1) / 2;
        total_quads += pu * pv * BSP_TESS_LEVEL * BSP_TESS_LEVEL;
    }
    if (total_quads == 0) return;

    /* Expand brush array to hold existing brushes + new patch brushes */
    u32 old_count = cm->brush_count;
    u32 new_cap = old_count + total_quads;
    qk_brush_t *new_brushes = (qk_brush_t *)realloc(cm->brushes, new_cap * sizeof(qk_brush_t));
    if (!new_brushes) return;
    cm->brushes = new_brushes;

    for (u32 fi = 0; fi < face_count; fi++) {
        const bsp_face_t *f = &faces[fi];
        if (f->type != 2 || f->size[0] < 3 || f->size[1] < 3) continue;
        if (f->texture < 0 || (u32)f->texture >= tex_count) continue;
        if (is_tool_texture(textures[f->texture].name)) continue;
        if (f->vertex < 0 || (u32)(f->vertex + f->size[0] * f->size[1]) > vert_count) continue;

        i32 cols = f->size[0];
        u32 pu_count = ((u32)cols - 1) / 2;
        u32 pv_count = ((u32)(f->size[1]) - 1) / 2;

        for (u32 pv = 0; pv < pv_count; pv++) {
            for (u32 pu = 0; pu < pu_count; pu++) {
                const bsp_vertex_t *cp = &verts[f->vertex + (pv * 2) * cols + (pu * 2)];

                /* Tessellate sub-patch into a grid of positions */
                f32 grid[BSP_TESS_VERTS][3];
                i32 w = BSP_TESS_LEVEL + 1;
                for (i32 tv = 0; tv <= BSP_TESS_LEVEL; tv++) {
                    for (i32 tu = 0; tu <= BSP_TESS_LEVEL; tu++) {
                        f32 u = (f32)tu / (f32)BSP_TESS_LEVEL;
                        f32 v = (f32)tv / (f32)BSP_TESS_LEVEL;
                        f32 nrm[3];
                        bezier_eval(cp, cols, u, v, grid[tv * w + tu], nrm);
                    }
                }

                /* For each tessellated quad, create a thin slab brush */
                for (i32 tv = 0; tv < BSP_TESS_LEVEL; tv++) {
                    for (i32 tu = 0; tu < BSP_TESS_LEVEL; tu++) {
                        f32 *p0 = grid[tv * w + tu];
                        f32 *p1 = grid[tv * w + tu + 1];
                        f32 *p2 = grid[(tv + 1) * w + tu];
                        f32 *p3 = grid[(tv + 1) * w + tu + 1];

                        /* Quad center and face normal */
                        f32 cx = (p0[0]+p1[0]+p2[0]+p3[0]) * 0.25f;
                        f32 cy = (p0[1]+p1[1]+p2[1]+p3[1]) * 0.25f;
                        f32 cz = (p0[2]+p1[2]+p2[2]+p3[2]) * 0.25f;

                        /* Cross product of diagonals for face normal */
                        f32 d1[3] = { p3[0]-p0[0], p3[1]-p0[1], p3[2]-p0[2] };
                        f32 d2[3] = { p2[0]-p1[0], p2[1]-p1[1], p2[2]-p1[2] };
                        f32 nx = d1[1]*d2[2] - d1[2]*d2[1];
                        f32 ny = d1[2]*d2[0] - d1[0]*d2[2];
                        f32 nz = d1[0]*d2[1] - d1[1]*d2[0];
                        f32 len = sqrtf(nx*nx + ny*ny + nz*nz);
                        if (len < 0.0001f) continue;
                        nx /= len; ny /= len; nz /= len;

                        /* 6-plane slab brush */
                        qk_plane_t *pl = (qk_plane_t *)calloc(6, sizeof(qk_plane_t));
                        if (!pl) continue;

                        /* Front plane (surface) */
                        pl[0].normal = (vec3_t){nx, ny, nz};
                        pl[0].dist = nx*cx + ny*cy + nz*cz;

                        /* Back plane (SLAB_THICKNESS behind surface) */
                        pl[1].normal = (vec3_t){-nx, -ny, -nz};
                        pl[1].dist = -(pl[0].dist - PATCH_SLAB_THICKNESS);

                        /* 4 edge bevel planes */
                        f32 *edges[4][2] = { {p0,p1}, {p1,p3}, {p3,p2}, {p2,p0} };
                        for (int e = 0; e < 4; e++) {
                            f32 *ea = edges[e][0], *eb = edges[e][1];
                            f32 ex = eb[0]-ea[0], ey = eb[1]-ea[1], ez = eb[2]-ea[2];
                            /* Edge normal = cross(edge_dir, face_normal) */
                            f32 enx = ey*nz - ez*ny;
                            f32 eny = ez*nx - ex*nz;
                            f32 enz = ex*ny - ey*nx;
                            f32 elen = sqrtf(enx*enx + eny*eny + enz*enz);
                            if (elen < 0.0001f) { enx = 0; eny = 0; enz = 1; elen = 1; }
                            enx /= elen; eny /= elen; enz /= elen;
                            pl[2+e].normal = (vec3_t){enx, eny, enz};
                            pl[2+e].dist = enx*ea[0] + eny*ea[1] + enz*ea[2];
                        }

                        qk_brush_t *ob = &cm->brushes[cm->brush_count];
                        ob->planes = pl;
                        ob->plane_count = 6;

                        /* AABB from the 4 quad corners + slab thickness */
                        ob->mins.x = fminf(fminf(p0[0],p1[0]), fminf(p2[0],p3[0])) - PATCH_SLAB_THICKNESS;
                        ob->mins.y = fminf(fminf(p0[1],p1[1]), fminf(p2[1],p3[1])) - PATCH_SLAB_THICKNESS;
                        ob->mins.z = fminf(fminf(p0[2],p1[2]), fminf(p2[2],p3[2])) - PATCH_SLAB_THICKNESS;
                        ob->maxs.x = fmaxf(fmaxf(p0[0],p1[0]), fmaxf(p2[0],p3[0])) + PATCH_SLAB_THICKNESS;
                        ob->maxs.y = fmaxf(fmaxf(p0[1],p1[1]), fmaxf(p2[1],p3[1])) + PATCH_SLAB_THICKNESS;
                        ob->maxs.z = fmaxf(fmaxf(p0[2],p1[2]), fmaxf(p2[2],p3[2])) + PATCH_SLAB_THICKNESS;

                        cm->brush_count++;
                    }
                }
            }
        }
    }
}

/* ---- Check if a face should be rendered ---- */

static bool face_renderable(const bsp_face_t *f, const bsp_texture_t *textures, u32 tex_count) {
    if (f->texture < 0 || (u32)f->texture >= tex_count) return false;
    if (textures[f->texture].flags & (Q3_SURF_NODRAW | Q3_SURF_SKY)) return false;
    if (is_tool_texture(textures[f->texture].name)) return false;
    return true;
}

/* ---- Build render geometry from BSP faces ---- */

static qk_result_t build_bsp_render(
    const bsp_texture_t *textures, u32 tex_count,
    const bsp_vertex_t *verts, u32 vert_count,
    const bsp_meshvert_t *meshverts, u32 mv_count,
    const bsp_face_t *faces, u32 face_count,
    qk_world_vertex_t **out_verts, u32 *out_vert_count,
    u32 **out_indices, u32 *out_idx_count,
    qk_draw_surface_t **out_surfaces, u32 *out_surf_count)
{
    /* Pre-count vertices, indices, and surfaces needed */
    u32 total_indices = 0;
    u32 total_surfs = 0;
    u32 patch_verts = 0;
    u32 patch_indices = 0;
    u32 patch_surfs = 0;

    for (u32 i = 0; i < face_count; i++) {
        const bsp_face_t *f = &faces[i];
        if (!face_renderable(f, textures, tex_count)) continue;

        if ((f->type == 1 || f->type == 3) &&
            f->n_meshverts > 0 && f->n_meshverts % 3 == 0) {
            total_indices += (u32)f->n_meshverts;
            total_surfs++;
        } else if (f->type == 2 && f->size[0] >= 3 && f->size[1] >= 3) {
            u32 pu = ((u32)f->size[0] - 1) / 2;
            u32 pv = ((u32)f->size[1] - 1) / 2;
            u32 np = pu * pv;
            patch_verts += np * BSP_TESS_VERTS;
            patch_indices += np * BSP_TESS_IDXS;
            patch_surfs += np;
        }
    }

    total_indices += patch_indices;
    total_surfs += patch_surfs;

    if (total_surfs == 0) return QK_ERROR_NOT_FOUND;

    u32 total_verts = vert_count + patch_verts;
    qk_world_vertex_t *rv = (qk_world_vertex_t *)calloc(total_verts, sizeof(qk_world_vertex_t));
    u32 *ri = (u32 *)calloc(total_indices, sizeof(u32));
    qk_draw_surface_t *rs = (qk_draw_surface_t *)calloc(total_surfs, sizeof(qk_draw_surface_t));
    if (!rv || !ri || !rs) {
        free(rv); free(ri); free(rs);
        return QK_ERROR_OUT_OF_MEMORY;
    }

    /* Convert all BSP vertices: real normals + world-space planar UVs */
    for (u32 i = 0; i < vert_count; i++) {
        rv[i].position[0] = verts[i].position[0];
        rv[i].position[1] = verts[i].position[1];
        rv[i].position[2] = verts[i].position[2];
        rv[i].normal[0] = verts[i].normal[0];
        rv[i].normal[1] = verts[i].normal[1];
        rv[i].normal[2] = verts[i].normal[2];
        world_uv(rv[i].position, rv[i].normal, rv[i].uv);
        rv[i].texture_id = 0;
    }

    u32 idx_cursor = 0;
    u32 surf_cursor = 0;
    u32 vert_cursor = vert_count; /* extra verts for patches go here */

    for (u32 i = 0; i < face_count; i++) {
        const bsp_face_t *f = &faces[i];
        if (!face_renderable(f, textures, tex_count)) continue;

        /* ---- Type 1 (polygon) / Type 3 (mesh) ---- */
        if ((f->type == 1 || f->type == 3) &&
            f->n_meshverts > 0 && f->n_meshverts % 3 == 0) {

            if (f->vertex < 0 || f->meshvert < 0) continue;
            if ((u32)(f->vertex + f->n_verts) > vert_count) continue;
            if ((u32)(f->meshvert + f->n_meshverts) > mv_count) continue;

            u32 base_idx = idx_cursor;
            for (i32 m = 0; m < f->n_meshverts; m += 3) {
                i32 v0 = f->vertex + meshverts[f->meshvert + m + 0].offset;
                i32 v1 = f->vertex + meshverts[f->meshvert + m + 1].offset;
                i32 v2 = f->vertex + meshverts[f->meshvert + m + 2].offset;
                /* Q3 BSP uses CW winding; Vulkan expects CCW — swap v1/v2 */
                ri[idx_cursor++] = (v0 >= 0 && (u32)v0 < vert_count) ? (u32)v0 : 0;
                ri[idx_cursor++] = (v2 >= 0 && (u32)v2 < vert_count) ? (u32)v2 : 0;
                ri[idx_cursor++] = (v1 >= 0 && (u32)v1 < vert_count) ? (u32)v1 : 0;
            }

            rs[surf_cursor].index_offset  = base_idx;
            rs[surf_cursor].index_count   = idx_cursor - base_idx;
            rs[surf_cursor].vertex_offset = 0;
            rs[surf_cursor].texture_index = 0;
            surf_cursor++;
        }

        /* ---- Type 2 (bezier patch) ---- */
        else if (f->type == 2 && f->size[0] >= 3 && f->size[1] >= 3) {
            i32 cols = f->size[0];
            i32 rows = f->size[1];
            u32 pu_count = ((u32)cols - 1) / 2;
            u32 pv_count = ((u32)rows - 1) / 2;

            if (f->vertex < 0 || (u32)(f->vertex + rows * cols) > vert_count) continue;

            for (u32 pv = 0; pv < pv_count; pv++) {
                for (u32 pu = 0; pu < pu_count; pu++) {
                    const bsp_vertex_t *cp = &verts[f->vertex + (pv * 2) * cols + (pu * 2)];

                    /* Tessellate: generate (N+1)x(N+1) grid of vertices */
                    u32 vbase = vert_cursor;
                    for (i32 tv = 0; tv <= BSP_TESS_LEVEL; tv++) {
                        for (i32 tu = 0; tu <= BSP_TESS_LEVEL; tu++) {
                            f32 u = (f32)tu / (f32)BSP_TESS_LEVEL;
                            f32 v = (f32)tv / (f32)BSP_TESS_LEVEL;
                            qk_world_vertex_t *wv = &rv[vert_cursor++];
                            bezier_eval(cp, cols, u, v, wv->position, wv->normal);
                            world_uv(wv->position, wv->normal, wv->uv);
                            wv->texture_id = 0;
                        }
                    }

                    /* Generate triangle indices for the tessellated grid */
                    u32 base_idx = idx_cursor;
                    i32 w = BSP_TESS_LEVEL + 1;
                    for (i32 tv = 0; tv < BSP_TESS_LEVEL; tv++) {
                        for (i32 tu = 0; tu < BSP_TESS_LEVEL; tu++) {
                            u32 i0 = vbase + (u32)(tv * w + tu);
                            u32 i1 = i0 + 1;
                            u32 i2 = i0 + (u32)w;
                            u32 i3 = i2 + 1;
                            /* CCW winding for Vulkan */
                            ri[idx_cursor++] = i0;
                            ri[idx_cursor++] = i1;
                            ri[idx_cursor++] = i2;
                            ri[idx_cursor++] = i1;
                            ri[idx_cursor++] = i3;
                            ri[idx_cursor++] = i2;
                        }
                    }

                    rs[surf_cursor].index_offset  = base_idx;
                    rs[surf_cursor].index_count   = idx_cursor - base_idx;
                    rs[surf_cursor].vertex_offset = 0;
                    rs[surf_cursor].texture_index = 0;
                    surf_cursor++;
                }
            }
        }
    }

    *out_verts = rv;
    *out_vert_count = vert_cursor;
    *out_indices = ri;
    *out_idx_count = idx_cursor;
    *out_surfaces = rs;
    *out_surf_count = surf_cursor;

    return QK_SUCCESS;
}

/* ---- Parsed entity for BSP entity lump ---- */

#define BSP_MAX_ENTITIES 1024

typedef struct {
    char    classname[64];
    char    targetname[64];
    char    target[64];
    vec3_t  origin;
    f32     angle;
    vec3_t  mins;
    vec3_t  maxs;
    bool    has_model;       /* has a brush model (trigger volume) */
} bsp_entity_parsed_t;

/* ---- Parse all entities from BSP entity lump ---- */

static u32 parse_bsp_entity_lump(const char *text, u32 text_len,
                                   bsp_entity_parsed_t *ents, u32 max_ents) {
    u32 count = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end && count < max_ents) {
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        p++;

        bsp_entity_parsed_t *ent = &ents[count];
        memset(ent, 0, sizeof(*ent));

        char model_str[64] = {0};

        while (p < end && *p != '}') {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p >= end || *p == '}') break;

            if (*p != '"') { while (p < end && *p != '\n') p++; continue; }
            p++;
            char key[64] = {0};
            u32 ki = 0;
            while (p < end && *p != '"' && ki < 63) key[ki++] = *p++;
            if (p < end && *p == '"') p++;

            while (p < end && (*p == ' ' || *p == '\t')) p++;

            if (p >= end || *p != '"') { while (p < end && *p != '\n') p++; continue; }
            p++;
            char val[256] = {0};
            u32 vi = 0;
            while (p < end && *p != '"' && vi < 255) val[vi++] = *p++;
            if (p < end && *p == '"') p++;

            if (strcmp(key, "classname") == 0) strncpy(ent->classname, val, 63);
            else if (strcmp(key, "targetname") == 0) strncpy(ent->targetname, val, 63);
            else if (strcmp(key, "target") == 0) strncpy(ent->target, val, 63);
            else if (strcmp(key, "origin") == 0) sscanf(val, "%f %f %f", &ent->origin.x, &ent->origin.y, &ent->origin.z);
            else if (strcmp(key, "angle") == 0) ent->angle = (f32)atof(val);
            else if (strcmp(key, "model") == 0) strncpy(model_str, val, 63);
        }

        if (p < end && *p == '}') p++;

        ent->has_model = (model_str[0] == '*');
        count++;
    }

    return count;
}

/* ---- Find entity by targetname ---- */

static const bsp_entity_parsed_t *find_entity_by_targetname(
    const bsp_entity_parsed_t *ents, u32 count, const char *targetname) {
    if (!targetname || targetname[0] == '\0') return NULL;
    for (u32 i = 0; i < count; i++) {
        if (ents[i].targetname[0] != '\0' && strcmp(ents[i].targetname, targetname) == 0)
            return &ents[i];
    }
    return NULL;
}

/* ---- Extract spawn points, teleporters, and jump pads from parsed entities ---- */

static void extract_bsp_entities(const bsp_entity_parsed_t *ents, u32 ent_count,
                                  qk_map_data_t *out) {
    /* Count each type */
    u32 spawn_cap = QK_MAP_MAX_SPAWN_POINTS;
    u32 tele_cap = QK_MAP_MAX_TELEPORTERS;
    u32 pad_cap = QK_MAP_MAX_JUMP_PADS;

    out->spawn_points = (qk_spawn_point_t *)calloc(spawn_cap, sizeof(qk_spawn_point_t));
    out->teleporters = (qk_teleporter_t *)calloc(tele_cap, sizeof(qk_teleporter_t));
    out->jump_pads = (qk_jump_pad_t *)calloc(pad_cap, sizeof(qk_jump_pad_t));
    out->spawn_count = 0;
    out->teleporter_count = 0;
    out->jump_pad_count = 0;

    if (!out->spawn_points || !out->teleporters || !out->jump_pads) return;

    for (u32 i = 0; i < ent_count; i++) {
        const bsp_entity_parsed_t *e = &ents[i];

        /* Spawn points */
        if (out->spawn_count < spawn_cap &&
            (strcmp(e->classname, "info_player_deathmatch") == 0 ||
             strcmp(e->classname, "info_player_start") == 0)) {
            out->spawn_points[out->spawn_count].origin = e->origin;
            out->spawn_points[out->spawn_count].yaw = e->angle;
            out->spawn_count++;
        }

        /* Teleporters: trigger_teleport -> target -> misc_teleporter_dest/target_position */
        if (out->teleporter_count < tele_cap &&
            strcmp(e->classname, "trigger_teleport") == 0 &&
            e->target[0] != '\0') {
            const bsp_entity_parsed_t *dest = find_entity_by_targetname(ents, ent_count, e->target);
            if (dest) {
                qk_teleporter_t *tp = &out->teleporters[out->teleporter_count];
                tp->origin = e->origin;
                /* Default trigger volume (32x32x32) if no brush model.
                   Gameplay can refine using brush model bounds later. */
                tp->mins = (vec3_t){e->origin.x - 16.0f, e->origin.y - 16.0f, e->origin.z - 16.0f};
                tp->maxs = (vec3_t){e->origin.x + 16.0f, e->origin.y + 16.0f, e->origin.z + 16.0f};
                tp->destination = dest->origin;
                tp->dest_yaw = dest->angle;
                out->teleporter_count++;
            }
        }

        /* Jump pads: trigger_push -> target -> target_position */
        if (out->jump_pad_count < pad_cap &&
            strcmp(e->classname, "trigger_push") == 0 &&
            e->target[0] != '\0') {
            const bsp_entity_parsed_t *dest = find_entity_by_targetname(ents, ent_count, e->target);
            if (dest) {
                qk_jump_pad_t *jp = &out->jump_pads[out->jump_pad_count];
                jp->origin = e->origin;
                jp->mins = (vec3_t){e->origin.x - 16.0f, e->origin.y - 16.0f, e->origin.z - 16.0f};
                jp->maxs = (vec3_t){e->origin.x + 16.0f, e->origin.y + 16.0f, e->origin.z + 16.0f};
                jp->target = dest->origin;
                out->jump_pad_count++;
            }
        }
    }

    /* Free empty arrays */
    if (out->spawn_count == 0) { free(out->spawn_points); out->spawn_points = NULL; }
    if (out->teleporter_count == 0) { free(out->teleporters); out->teleporters = NULL; }
    if (out->jump_pad_count == 0) { free(out->jump_pads); out->jump_pads = NULL; }
}

/* ---- Public API ---- */

qk_result_t qk_bsp_load(const u8 *data, u64 data_len, qk_map_data_t *out) {
    if (!data || !out || data_len < sizeof(bsp_header_t))
        return QK_ERROR_INVALID_PARAM;

    memset(out, 0, sizeof(*out));

    const bsp_header_t *hdr = (const bsp_header_t *)data;
    if (hdr->magic != BSP_MAGIC) return QK_ERROR_INVALID_PARAM;
    if (hdr->version != BSP_VERSION_46 && hdr->version != BSP_VERSION_47) {
        fprintf(stderr, "[BSP] Unsupported version: %d\n", hdr->version);
        return QK_ERROR_INVALID_PARAM;
    }

    fprintf(stderr, "[BSP] Loading IBSP v%d (%lu bytes)\n",
            hdr->version, (unsigned long)data_len);

    /* Get lump pointers */
    u32 tex_count = 0, plane_count = 0, brush_count = 0, side_count = 0;
    u32 vert_count = 0, mv_count = 0, face_count = 0;

    const bsp_texture_t   *textures  = (const bsp_texture_t *)  get_lump(data, data_len, hdr, LUMP_TEXTURES,   sizeof(bsp_texture_t),   &tex_count);
    const bsp_plane_t     *planes    = (const bsp_plane_t *)    get_lump(data, data_len, hdr, LUMP_PLANES,     sizeof(bsp_plane_t),     &plane_count);
    const bsp_brush_t     *brushes   = (const bsp_brush_t *)    get_lump(data, data_len, hdr, LUMP_BRUSHES,    sizeof(bsp_brush_t),     &brush_count);
    const bsp_brushside_t *sides     = (const bsp_brushside_t *)get_lump(data, data_len, hdr, LUMP_BRUSHSIDES, sizeof(bsp_brushside_t), &side_count);
    const bsp_vertex_t    *verts     = (const bsp_vertex_t *)   get_lump(data, data_len, hdr, LUMP_VERTICES,   sizeof(bsp_vertex_t),    &vert_count);
    const bsp_meshvert_t  *meshverts = (const bsp_meshvert_t *) get_lump(data, data_len, hdr, LUMP_MESHVERTS,  sizeof(bsp_meshvert_t),  &mv_count);
    const bsp_face_t      *faces     = (const bsp_face_t *)     get_lump(data, data_len, hdr, LUMP_FACES,      sizeof(bsp_face_t),      &face_count);

    fprintf(stderr, "[BSP] %u textures, %u planes, %u brushes, %u sides\n",
            tex_count, plane_count, brush_count, side_count);
    fprintf(stderr, "[BSP] %u vertices, %u meshverts, %u faces\n",
            vert_count, mv_count, face_count);

    /* Build collision model */
    if (brushes && sides && planes && textures) {
        qk_result_t res = build_bsp_collision(textures, tex_count,
                                               planes, plane_count,
                                               brushes, brush_count,
                                               sides, side_count,
                                               &out->collision);
        if (res == QK_SUCCESS) {
            fprintf(stderr, "[BSP] Collision: %u solid brushes\n", out->collision.brush_count);
        } else {
            fprintf(stderr, "[BSP] Warning: collision build failed (%d)\n", res);
        }
    }

    /* Add patch collision (thin slab brushes from bezier patches) */
    if (verts && faces && textures) {
        u32 before = out->collision.brush_count;
        build_patch_collision(textures, tex_count,
                              verts, vert_count,
                              faces, face_count,
                              &out->collision);
        u32 patch_brushes = out->collision.brush_count - before;
        if (patch_brushes > 0)
            fprintf(stderr, "[BSP] Patch collision: %u slab brushes\n", patch_brushes);
    }

    /* Build render geometry */
    if (verts && meshverts && faces && textures) {
        qk_result_t res = build_bsp_render(textures, tex_count,
                                            verts, vert_count,
                                            meshverts, mv_count,
                                            faces, face_count,
                                            &out->vertices, &out->vertex_count,
                                            &out->indices, &out->index_count,
                                            &out->surfaces, &out->surface_count);
        if (res == QK_SUCCESS) {
            fprintf(stderr, "[BSP] Render: %u verts, %u indices, %u surfaces\n",
                    out->vertex_count, out->index_count, out->surface_count);
        } else {
            fprintf(stderr, "[BSP] Warning: render build failed (%d)\n", res);
        }
    }

    /* Parse entities for spawn points, teleporters, jump pads */
    u32 ent_len = 0;
    const char *ent_text = (const char *)get_lump(data, data_len, hdr,
                                                   LUMP_ENTITIES, 1, &ent_len);
    if (ent_text && ent_len > 0) {
        bsp_entity_parsed_t *ents = (bsp_entity_parsed_t *)calloc(BSP_MAX_ENTITIES, sizeof(bsp_entity_parsed_t));
        if (ents) {
            u32 ent_count = parse_bsp_entity_lump(ent_text, ent_len, ents, BSP_MAX_ENTITIES);
            extract_bsp_entities(ents, ent_count, out);
            free(ents);
        }
        fprintf(stderr, "[BSP] Spawn points: %u, Teleporters: %u, Jump pads: %u\n",
                out->spawn_count, out->teleporter_count, out->jump_pad_count);
    }

    return QK_SUCCESS;
}
