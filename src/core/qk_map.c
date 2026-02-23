/*
 * QUICKEN Engine - Quake 3 .map Loader
 *
 * Parses the Quake 3 .map text format (entities containing brushes,
 * brushes containing face definitions). Produces collision brushes
 * for physics, triangulated render geometry, and spawn points.
 *
 * Supports the standard Quake 3 .map format as written by TrenchBroom.
 *
 * Texture colors: Each unique texture name is hashed to a deterministic
 * RGB color. No real texture loading for the vertical slice.
 */

#include "core/qk_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

// --- Internal limits ---

#define MAX_FACE_PLANES     32
#define MAX_FACE_VERTS      64
#define MAX_FACES_PER_BRUSH 32
#define MAX_BRUSHES         QK_MAP_MAX_BRUSHES
#define MAX_ENTITIES        1024

static const f32 EPSILON       = 0.001f;
static const f32 PLANE_EPSILON = 0.01f;

// --- Internal types ---

typedef struct {
    vec3_t normal;
    f32    dist;
} plane_t;

typedef struct {
    vec3_t  points[3];  // three points defining the plane
    plane_t plane;
    char    texture[64];
    f32     offset_x, offset_y;
    f32     rotation;
    f32     scale_x, scale_y;
} map_face_t;

typedef struct {
    map_face_t  faces[MAX_FACES_PER_BRUSH];
    u32         face_count;
} map_brush_t;

typedef struct {
    char key[64];
    char value[256];
} map_kv_t;

typedef struct {
    char        classname[64];
    map_kv_t    kvs[32];
    u32         kv_count;
    map_brush_t brushes[64];
    u32         brush_count;
} map_entity_t;

// --- Parsed map ---

typedef struct {
    map_entity_t entities[MAX_ENTITIES];
    u32          entity_count;
} parsed_map_t;

// --- Parse helpers ---

static const char *skip_whitespace(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *skip_to_eol(const char *p) {
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    return p;
}

static const char *read_token(const char *p, char *buf, u32 buf_size) {
    p = skip_whitespace(p);

    // Handle quoted strings
    if (*p == '"') {
        p++;
        u32 i = 0;
        while (*p && *p != '"' && i < buf_size - 1) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        if (*p == '"') p++;
        return p;
    }

    // Regular token
    u32 i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && i < buf_size - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return p;
}

static f32 parse_f32(const char **pp) {
    const char *p = *pp;
    p = skip_whitespace(p);
    char buf[64];
    u32 i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != ')' && *p != '\n' && *p != '\r' && i < 63) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    *pp = p;
    return (f32)atof(buf);
}

// --- Plane from three points ---

static plane_t plane_from_points(vec3_t p0, vec3_t p1, vec3_t p2) {
    vec3_t e1 = vec3_sub(p1, p0);
    vec3_t e2 = vec3_sub(p2, p0);
    vec3_t n = vec3_cross(e1, e2);
    f32 len = vec3_length(n);
    plane_t pl;
    if (len > EPSILON) {
        pl.normal = vec3_scale(n, 1.0f / len);
    } else {
        pl.normal = (vec3_t){0.0f, 0.0f, 1.0f};
    }
    pl.dist = vec3_dot(pl.normal, p0);
    return pl;
}

/* ---- Parse a face line ----
 * Format: ( x y z ) ( x y z ) ( x y z ) texture offsetX offsetY rotation scaleX scaleY
 */
static const char *parse_face(const char *p, map_face_t *face) {
    for (int i = 0; i < 3; i++) {
        p = skip_whitespace(p);
        if (*p != '(') return NULL;
        p++;  // skip (
        face->points[i].x = parse_f32(&p);
        face->points[i].y = parse_f32(&p);
        face->points[i].z = parse_f32(&p);
        p = skip_whitespace(p);
        if (*p != ')') return NULL;
        p++;  // skip )
    }

    // Texture name
    p = read_token(p, face->texture, sizeof(face->texture));

    /* Texture params -- Q3 format may have brackets or bare numbers.
       Handle both standard and Valve 220 format. */
    p = skip_whitespace(p);

    if (*p == '[') {
        // Valve 220 format: [ ux uy uz offset ] [ vx vy vz offset ] rotation scaleX scaleY
        // Skip the U axis bracket
        while (*p && *p != ']') p++;
        if (*p == ']') p++;
        p = skip_whitespace(p);
        // Skip the V axis bracket
        if (*p == '[') {
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        }
        p = skip_whitespace(p);
        face->rotation = parse_f32(&p);
        face->scale_x = parse_f32(&p);
        face->scale_y = parse_f32(&p);
        face->offset_x = 0.0f;
        face->offset_y = 0.0f;
    } else {
        // Standard format: offsetX offsetY rotation scaleX scaleY
        face->offset_x = parse_f32(&p);
        face->offset_y = parse_f32(&p);
        face->rotation = parse_f32(&p);
        face->scale_x = parse_f32(&p);
        face->scale_y = parse_f32(&p);
    }

    // Skip remaining tokens on line (content/surface flags in Q3 format)
    p = skip_to_eol(p);

    // Compute plane
    face->plane = plane_from_points(face->points[0], face->points[1], face->points[2]);

    return p;
}

// --- Parse the .map text ---

static qk_result_t parse_map_text(const char *data, parsed_map_t *map) {
    memset(map, 0, sizeof(*map));
    const char *p = data;

    while (*p) {
        p = skip_whitespace(p);
        if (*p == '\0') break;

        // Skip comments
        if (*p == '/' && *(p + 1) == '/') {
            p = skip_to_eol(p);
            continue;
        }

        // Entity open
        if (*p == '{') {
            p++;
            if (map->entity_count >= MAX_ENTITIES) {
                p = skip_to_eol(p);
                continue;
            }

            map_entity_t *ent = &map->entities[map->entity_count];
            memset(ent, 0, sizeof(*ent));

            while (*p) {
                p = skip_whitespace(p);
                if (*p == '}') { p++; break; }

                // Skip comments
                if (*p == '/' && *(p + 1) == '/') {
                    p = skip_to_eol(p);
                    continue;
                }

                // Brush
                if (*p == '{') {
                    p++;
                    if (ent->brush_count >= 64) {
                        // skip brush
                        int depth = 1;
                        while (*p && depth > 0) {
                            if (*p == '{') depth++;
                            if (*p == '}') depth--;
                            p++;
                        }
                        continue;
                    }

                    map_brush_t *brush = &ent->brushes[ent->brush_count];
                    memset(brush, 0, sizeof(*brush));

                    while (*p) {
                        p = skip_whitespace(p);
                        if (*p == '}') { p++; break; }

                        // Skip comments
                        if (*p == '/' && *(p + 1) == '/') {
                            p = skip_to_eol(p);
                            continue;
                        }

                        // Face line starts with (
                        if (*p == '(') {
                            if (brush->face_count < MAX_FACES_PER_BRUSH) {
                                const char *next = parse_face(p, &brush->faces[brush->face_count]);
                                if (next) {
                                    brush->face_count++;
                                    p = next;
                                } else {
                                    p = skip_to_eol(p);
                                }
                            } else {
                                p = skip_to_eol(p);
                            }
                        } else {
                            p = skip_to_eol(p);
                        }
                    }

                    if (brush->face_count >= 4) {
                        ent->brush_count++;
                    }
                }
                // Key-value pair
                else if (*p == '"') {
                    char key[64], value[256];
                    p = read_token(p, key, sizeof(key));
                    p = read_token(p, value, sizeof(value));

                    if (ent->kv_count < 32) {
                        snprintf(ent->kvs[ent->kv_count].key, sizeof(ent->kvs[ent->kv_count].key), "%s", key);
                        snprintf(ent->kvs[ent->kv_count].value, sizeof(ent->kvs[ent->kv_count].value), "%s", value);
                        ent->kv_count++;
                    }

                    if (strcmp(key, "classname") == 0) {
                        snprintf(ent->classname, sizeof(ent->classname), "%.63s", value);
                    }
                }
                else {
                    p = skip_to_eol(p);
                }
            }

            map->entity_count++;
        }
        else {
            p = skip_to_eol(p);
        }
    }

    return QK_SUCCESS;
}

// --- Brush to polygon generation ---

// Intersect three planes, returning the intersection point if valid
static bool intersect_3_planes(plane_t a, plane_t b, plane_t c, vec3_t *out) {
    vec3_t bc = vec3_cross(b.normal, c.normal);
    f32 denom = vec3_dot(a.normal, bc);
    if (fabsf(denom) < EPSILON) return false;

    vec3_t ca = vec3_cross(c.normal, a.normal);
    vec3_t ab = vec3_cross(a.normal, b.normal);

    *out = vec3_scale(
        vec3_add(vec3_add(
            vec3_scale(bc, a.dist),
            vec3_scale(ca, b.dist)),
            vec3_scale(ab, c.dist)),
        1.0f / denom);
    return true;
}

// Check if a point is on the inside of all planes of a brush
static bool point_inside_brush(vec3_t point, const map_face_t *faces, u32 face_count) {
    for (u32 i = 0; i < face_count; i++) {
        f32 d = vec3_dot(faces[i].plane.normal, point) - faces[i].plane.dist;
        if (d > PLANE_EPSILON) return false;
    }
    return true;
}

// Sort face vertices in winding order around the face normal
static void sort_face_verts(vec3_t *verts, u32 count, vec3_t normal, vec3_t center) {
    if (count < 3) return;

    // Compute tangent basis for the face plane
    vec3_t ref = (fabsf(normal.z) < 0.9f) ?
        (vec3_t){0, 0, 1} : (vec3_t){1, 0, 0};
    vec3_t u = vec3_normalize(vec3_cross(normal, ref));
    vec3_t v = vec3_cross(normal, u);

    // Compute angle of each vertex relative to center in the tangent plane
    f32 angles[MAX_FACE_VERTS];
    for (u32 i = 0; i < count; i++) {
        vec3_t d = vec3_sub(verts[i], center);
        angles[i] = atan2f(vec3_dot(d, v), vec3_dot(d, u));
    }

    // Simple insertion sort by angle
    for (u32 i = 1; i < count; i++) {
        vec3_t key_v = verts[i];
        f32 key_a = angles[i];
        u32 j = i;
        while (j > 0 && angles[j - 1] > key_a) {
            verts[j] = verts[j - 1];
            angles[j] = angles[j - 1];
            j--;
        }
        verts[j] = key_v;
        angles[j] = key_a;
    }
}

// --- Texture color from name hash ---

static u32 hash_texture_color(const char *name) {
    // Skip common path prefixes
    const char *base = name;
    const char *slash = strrchr(name, '/');
    if (slash) base = slash + 1;

    // FNV-1a hash
    u32 hash = 2166136261u;
    for (const char *c = base; *c; c++) {
        hash ^= (u32)(u8)*c;
        hash *= 16777619u;
    }

    // Convert to a visible color (avoid too dark)
    u8 r = (u8)(((hash >> 0) & 0xFF) / 2 + 64);
    u8 g = (u8)(((hash >> 8) & 0xFF) / 2 + 64);
    u8 b = (u8)(((hash >> 16) & 0xFF) / 2 + 64);

    // Skip known tool textures
    if (strstr(name, "skip") || strstr(name, "clip") || strstr(name, "trigger") ||
        strstr(name, "hint") || strstr(name, "nodraw")) {
        return 0;  // invisible
    }

    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
}

// --- Build collision model ---

static qk_result_t build_collision_model(const parsed_map_t *parsed, qk_collision_model_t *cm) {
    // Count total brushes across worldspawn entity (entity 0)
    u32 total_brushes = 0;
    for (u32 e = 0; e < parsed->entity_count; e++) {
        const map_entity_t *ent = &parsed->entities[e];
        // Only worldspawn contributes to the collision model
        if (e == 0 || strcmp(ent->classname, "worldspawn") == 0 ||
            strcmp(ent->classname, "func_wall") == 0) {
            total_brushes += ent->brush_count;
        }
    }

    if (total_brushes == 0) return QK_ERROR_NOT_FOUND;

    cm->brushes = (qk_brush_t *)calloc(total_brushes, sizeof(qk_brush_t));
    if (!cm->brushes) return QK_ERROR_OUT_OF_MEMORY;
    cm->brush_count = 0;

    for (u32 e = 0; e < parsed->entity_count; e++) {
        const map_entity_t *ent = &parsed->entities[e];
        if (e != 0 && strcmp(ent->classname, "worldspawn") != 0 &&
            strcmp(ent->classname, "func_wall") != 0) {
            continue;
        }

        for (u32 b = 0; b < ent->brush_count; b++) {
            const map_brush_t *mb = &ent->brushes[b];
            qk_brush_t *brush = &cm->brushes[cm->brush_count];

            brush->planes = (qk_plane_t *)calloc(mb->face_count, sizeof(qk_plane_t));
            if (!brush->planes) return QK_ERROR_OUT_OF_MEMORY;
            brush->plane_count = mb->face_count;

            /* Compute brush centroid from face definition points.
               Used to fix inconsistent normal directions. */
            vec3_t centroid = {0, 0, 0};
            u32 point_count = 0;
            for (u32 f = 0; f < mb->face_count; f++) {
                for (int p = 0; p < 3; p++) {
                    centroid = vec3_add(centroid, mb->faces[f].points[p]);
                    point_count++;
                }
            }
            if (point_count > 0) {
                centroid = vec3_scale(centroid, 1.0f / (f32)point_count);
            }

            /* Copy planes, ensuring all normals face OUTWARD from the brush.
               The physics trace code expects the brush interior on the negative
               side of every plane (dot(normal, point) - dist < 0 = inside).
               TrenchBroom .map files can have inconsistent face winding, so
               we use the centroid to fix the orientation: the centroid must be
               on the negative side (inside) of every plane. */
            for (u32 f = 0; f < mb->face_count; f++) {
                brush->planes[f].normal = mb->faces[f].plane.normal;
                brush->planes[f].dist = mb->faces[f].plane.dist;

                f32 d = vec3_dot(brush->planes[f].normal, centroid) - brush->planes[f].dist;
                if (d > 0.0f) {
                    /* Centroid is on the positive side -- normal faces inward.
                       Flip to make it outward-facing. */
                    brush->planes[f].normal.x = -brush->planes[f].normal.x;
                    brush->planes[f].normal.y = -brush->planes[f].normal.y;
                    brush->planes[f].normal.z = -brush->planes[f].normal.z;
                    brush->planes[f].dist = -brush->planes[f].dist;
                }
            }

            // Initialize AABB to extreme values
            brush->mins = (vec3_t){ 1e18f,  1e18f,  1e18f};
            brush->maxs = (vec3_t){-1e18f, -1e18f, -1e18f};

            /* Compute AABB from face-face-face vertex intersections.
               Now that normals are consistently outward, point_inside_brush
               (d <= PLANE_EPSILON) correctly identifies brush vertices. */
            bool has_vertex = false;
            for (u32 i = 0; i < mb->face_count; i++) {
                for (u32 j = i + 1; j < mb->face_count; j++) {
                    for (u32 k = j + 1; k < mb->face_count; k++) {
                        vec3_t v;
                        plane_t pi = {brush->planes[i].normal, brush->planes[i].dist};
                        plane_t pj = {brush->planes[j].normal, brush->planes[j].dist};
                        plane_t pk = {brush->planes[k].normal, brush->planes[k].dist};
                        if (!intersect_3_planes(pi, pj, pk, &v)) {
                            continue;
                        }
                        // Check vertex is inside brush (on negative side of all planes)
                        bool valid = true;
                        for (u32 q = 0; q < brush->plane_count; q++) {
                            f32 d = vec3_dot(brush->planes[q].normal, v)
                                  - brush->planes[q].dist;
                            if (d > PLANE_EPSILON) {
                                valid = false;
                                break;
                            }
                        }
                        if (!valid) continue;

                        // Expand AABB
                        if (v.x < brush->mins.x) brush->mins.x = v.x;
                        if (v.y < brush->mins.y) brush->mins.y = v.y;
                        if (v.z < brush->mins.z) brush->mins.z = v.z;
                        if (v.x > brush->maxs.x) brush->maxs.x = v.x;
                        if (v.y > brush->maxs.y) brush->maxs.y = v.y;
                        if (v.z > brush->maxs.z) brush->maxs.z = v.z;
                        has_vertex = true;
                    }
                }
            }

            if (!has_vertex) {
                free(brush->planes);
                brush->planes = NULL;
                continue;
            }

            cm->brush_count++;
        }
    }

    return QK_SUCCESS;
}

// --- Build render geometry ---

// Temporary vertex/index arrays for geometry building
typedef struct {
    qk_world_vertex_t *vertices;
    u32                *indices;
    qk_draw_surface_t *surfaces;
    u32 vert_count;
    u32 idx_count;
    u32 surf_count;
    u32 vert_cap;
    u32 idx_cap;
    u32 surf_cap;
} geom_builder_t;

static qk_result_t build_render_geometry(const parsed_map_t *parsed,
                                          qk_world_vertex_t **out_verts, u32 *out_vert_count,
                                          u32 **out_indices, u32 *out_idx_count,
                                          qk_draw_surface_t **out_surfaces, u32 *out_surf_count) {
    geom_builder_t gb;
    gb.vert_cap = QK_MAP_MAX_VERTICES;
    gb.idx_cap = QK_MAP_MAX_INDICES;
    gb.surf_cap = QK_MAP_MAX_SURFACES;
    gb.vert_count = 0;
    gb.idx_count = 0;
    gb.surf_count = 0;

    gb.vertices = (qk_world_vertex_t *)calloc(gb.vert_cap, sizeof(qk_world_vertex_t));
    gb.indices = (u32 *)calloc(gb.idx_cap, sizeof(u32));
    gb.surfaces = (qk_draw_surface_t *)calloc(gb.surf_cap, sizeof(qk_draw_surface_t));

    if (!gb.vertices || !gb.indices || !gb.surfaces) {
        free(gb.vertices); free(gb.indices); free(gb.surfaces);
        return QK_ERROR_OUT_OF_MEMORY;
    }

    // Process worldspawn and func_wall brushes
    for (u32 e = 0; e < parsed->entity_count; e++) {
        const map_entity_t *ent = &parsed->entities[e];
        if (e != 0 && strcmp(ent->classname, "worldspawn") != 0 &&
            strcmp(ent->classname, "func_wall") != 0) {
            continue;
        }

        for (u32 b = 0; b < ent->brush_count; b++) {
            const map_brush_t *mb = &ent->brushes[b];

            // For each face, compute the polygon by intersecting with all other planes
            for (u32 f = 0; f < mb->face_count; f++) {
                u32 tex_color = hash_texture_color(mb->faces[f].texture);
                if (tex_color == 0) continue;  // skip tool textures

                // Collect vertices on this face
                vec3_t face_verts[MAX_FACE_VERTS];
                u32 fv_count = 0;

                for (u32 i = 0; i < mb->face_count && fv_count < MAX_FACE_VERTS; i++) {
                    if (i == f) continue;
                    for (u32 j = i + 1; j < mb->face_count && fv_count < MAX_FACE_VERTS; j++) {
                        if (j == f) continue;
                        vec3_t v;
                        if (!intersect_3_planes(mb->faces[f].plane,
                                                mb->faces[i].plane,
                                                mb->faces[j].plane, &v)) {
                            continue;
                        }
                        if (!point_inside_brush(v, mb->faces, mb->face_count)) {
                            continue;
                        }
                        // De-duplicate
                        bool dup = false;
                        for (u32 d = 0; d < fv_count; d++) {
                            vec3_t diff = vec3_sub(face_verts[d], v);
                            if (vec3_dot(diff, diff) < EPSILON * EPSILON) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            face_verts[fv_count++] = v;
                        }
                    }
                }

                if (fv_count < 3) continue;

                // Compute face center
                vec3_t center = {0, 0, 0};
                for (u32 i = 0; i < fv_count; i++) {
                    center = vec3_add(center, face_verts[i]);
                }
                center = vec3_scale(center, 1.0f / (f32)fv_count);

                // Sort vertices in winding order
                sort_face_verts(face_verts, fv_count, mb->faces[f].plane.normal, center);

                // Check capacity
                if (gb.vert_count + fv_count > gb.vert_cap) continue;
                if (gb.idx_count + (fv_count - 2) * 3 > gb.idx_cap) continue;
                if (gb.surf_count >= gb.surf_cap) continue;

                // Emit vertices
                u32 base_vert = gb.vert_count;
                f32 nr = (f32)((tex_color >> 24) & 0xFF) / 255.0f;
                f32 ng = (f32)((tex_color >> 16) & 0xFF) / 255.0f;
                f32 nb = (f32)((tex_color >>  8) & 0xFF) / 255.0f;

                for (u32 i = 0; i < fv_count; i++) {
                    qk_world_vertex_t *wv = &gb.vertices[gb.vert_count++];
                    wv->position[0] = face_verts[i].x;
                    wv->position[1] = face_verts[i].y;
                    wv->position[2] = face_verts[i].z;
                    // Use face normal
                    wv->normal[0] = mb->faces[f].plane.normal.x;
                    wv->normal[1] = mb->faces[f].plane.normal.y;
                    wv->normal[2] = mb->faces[f].plane.normal.z;
                    // Simple planar UV projection
                    vec3_t n = mb->faces[f].plane.normal;
                    f32 ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
                    if (az >= ax && az >= ay) {
                        wv->uv[0] = face_verts[i].x / 64.0f;
                        wv->uv[1] = face_verts[i].y / 64.0f;
                    } else if (ax >= ay) {
                        wv->uv[0] = face_verts[i].y / 64.0f;
                        wv->uv[1] = face_verts[i].z / 64.0f;
                    } else {
                        wv->uv[0] = face_verts[i].x / 64.0f;
                        wv->uv[1] = face_verts[i].z / 64.0f;
                    }
                    /* Encode color as texture_id placeholder
                       (actual texture system would use real IDs) */
                    wv->texture_id = tex_color;
                }

                // Emit triangle fan indices
                u32 base_idx = gb.idx_count;
                for (u32 i = 1; i + 1 < fv_count; i++) {
                    gb.indices[gb.idx_count++] = base_vert;
                    gb.indices[gb.idx_count++] = base_vert + i;
                    gb.indices[gb.idx_count++] = base_vert + i + 1;
                }

                // Emit surface
                qk_draw_surface_t *surf = &gb.surfaces[gb.surf_count++];
                surf->index_offset = base_idx;
                surf->index_count = gb.idx_count - base_idx;
                surf->vertex_offset = base_vert;
                surf->texture_index = tex_color;

                // Store color in normal channels for flat shading
                for (u32 i = base_vert; i < gb.vert_count; i++) {
                    gb.vertices[i].normal[0] = nr;
                    gb.vertices[i].normal[1] = ng;
                    gb.vertices[i].normal[2] = nb;
                }
            }
        }
    }

    if (gb.vert_count == 0) {
        free(gb.vertices); free(gb.indices); free(gb.surfaces);
        return QK_ERROR_NOT_FOUND;
    }

    *out_verts = gb.vertices;
    *out_vert_count = gb.vert_count;
    *out_indices = gb.indices;
    *out_idx_count = gb.idx_count;
    *out_surfaces = gb.surfaces;
    *out_surf_count = gb.surf_count;

    return QK_SUCCESS;
}

// --- Extract entity key-value helpers ---

static const char *ent_get_value(const map_entity_t *ent, const char *key) {
    for (u32 k = 0; k < ent->kv_count; k++) {
        if (strcmp(ent->kvs[k].key, key) == 0) return ent->kvs[k].value;
    }
    return NULL;
}

static vec3_t ent_get_origin(const map_entity_t *ent) {
    const char *val = ent_get_value(ent, "origin");
    vec3_t v = {0, 0, 0};
    if (val) sscanf(val, "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

static f32 ent_get_angle(const map_entity_t *ent) {
    const char *val = ent_get_value(ent, "angle");
    return val ? (f32)atof(val) : 0.0f;
}

static const map_entity_t *find_by_targetname(const parsed_map_t *parsed, const char *targetname) {
    if (!targetname || targetname[0] == '\0') return NULL;
    for (u32 e = 0; e < parsed->entity_count; e++) {
        const char *tn = ent_get_value(&parsed->entities[e], "targetname");
        if (tn && strcmp(tn, targetname) == 0) return &parsed->entities[e];
    }
    return NULL;
}

// --- Extract spawn points, teleporters, and jump pads ---

static void extract_map_entities(const parsed_map_t *parsed, qk_map_data_t *out) {
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

    for (u32 e = 0; e < parsed->entity_count; e++) {
        const map_entity_t *ent = &parsed->entities[e];

        // Spawn points
        if (out->spawn_count < spawn_cap &&
            (strcmp(ent->classname, "info_player_deathmatch") == 0 ||
             strcmp(ent->classname, "info_player_start") == 0)) {
            out->spawn_points[out->spawn_count].origin = ent_get_origin(ent);
            out->spawn_points[out->spawn_count].yaw = ent_get_angle(ent);
            out->spawn_count++;
        }

        // Teleporters
        if (out->teleporter_count < tele_cap &&
            strcmp(ent->classname, "trigger_teleport") == 0) {
            const char *target = ent_get_value(ent, "target");
            const map_entity_t *dest = find_by_targetname(parsed, target);
            if (dest) {
                qk_teleporter_t *tp = &out->teleporters[out->teleporter_count];
                tp->origin = ent_get_origin(ent);
                tp->mins = (vec3_t){tp->origin.x - 16.0f, tp->origin.y - 16.0f, tp->origin.z - 16.0f};
                tp->maxs = (vec3_t){tp->origin.x + 16.0f, tp->origin.y + 16.0f, tp->origin.z + 16.0f};
                tp->destination = ent_get_origin(dest);
                tp->dest_yaw = ent_get_angle(dest);
                out->teleporter_count++;
            }
        }

        // Jump pads
        if (out->jump_pad_count < pad_cap &&
            strcmp(ent->classname, "trigger_push") == 0) {
            const char *target = ent_get_value(ent, "target");
            const map_entity_t *dest = find_by_targetname(parsed, target);
            if (dest) {
                qk_jump_pad_t *jp = &out->jump_pads[out->jump_pad_count];
                jp->origin = ent_get_origin(ent);
                jp->mins = (vec3_t){jp->origin.x - 16.0f, jp->origin.y - 16.0f, jp->origin.z - 16.0f};
                jp->maxs = (vec3_t){jp->origin.x + 16.0f, jp->origin.y + 16.0f, jp->origin.z + 16.0f};
                jp->target = ent_get_origin(dest);
                out->jump_pad_count++;
            }
        }
    }

    // Free empty arrays
    if (out->spawn_count == 0) { free(out->spawn_points); out->spawn_points = NULL; }
    if (out->teleporter_count == 0) { free(out->teleporters); out->teleporters = NULL; }
    if (out->jump_pad_count == 0) { free(out->jump_pads); out->jump_pads = NULL; }
}

// --- Public API ---

qk_result_t qk_map_load_from_memory(const char *data, u64 data_len, qk_map_data_t *out) {
    if (!data || !out || data_len == 0) return QK_ERROR_INVALID_PARAM;
    memset(out, 0, sizeof(*out));

    // Parse the text
    parsed_map_t *parsed = (parsed_map_t *)calloc(1, sizeof(parsed_map_t));
    if (!parsed) return QK_ERROR_OUT_OF_MEMORY;

    qk_result_t res = parse_map_text(data, parsed);
    if (res != QK_SUCCESS) { free(parsed); return res; }

    if (parsed->entity_count == 0) {
        free(parsed);
        return QK_ERROR_NOT_FOUND;
    }

    fprintf(stderr, "[MapLoader] Parsed %u entities\n", parsed->entity_count);

    // Count total brushes
    u32 total_brushes = 0;
    for (u32 e = 0; e < parsed->entity_count; e++) {
        total_brushes += parsed->entities[e].brush_count;
    }
    fprintf(stderr, "[MapLoader] Total brushes: %u\n", total_brushes);

    // Build collision model
    res = build_collision_model(parsed, &out->collision);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "[MapLoader] Warning: collision model build failed (%d)\n", res);
        // Continue -- we can still have render geometry
    } else {
        fprintf(stderr, "[MapLoader] Collision model: %u brushes\n", out->collision.brush_count);
    }

    // Build render geometry
    res = build_render_geometry(parsed,
        &out->vertices, &out->vertex_count,
        &out->indices, &out->index_count,
        &out->surfaces, &out->surface_count);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "[MapLoader] Warning: render geometry build failed (%d)\n", res);
    } else {
        fprintf(stderr, "[MapLoader] Render geometry: %u verts, %u indices, %u surfaces\n",
                out->vertex_count, out->index_count, out->surface_count);
    }

    // Extract spawn points, teleporters, jump pads
    extract_map_entities(parsed, out);
    fprintf(stderr, "[MapLoader] Spawn points: %u, Teleporters: %u, Jump pads: %u\n",
            out->spawn_count, out->teleporter_count, out->jump_pad_count);

    free(parsed);
    return QK_SUCCESS;
}

qk_result_t qk_map_load(const char *filepath, qk_map_data_t *out) {
    if (!filepath || !out) return QK_ERROR_INVALID_PARAM;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "[MapLoader] Failed to open: %s\n", filepath);
        return QK_ERROR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return QK_ERROR_NOT_FOUND;
    }

    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return QK_ERROR_OUT_OF_MEMORY;
    }

    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[read] = '\0';

    fprintf(stderr, "[MapLoader] Loading: %s (%ld bytes)\n", filepath, size);

    // Detect BSP format by magic bytes
    if (read >= 4 && memcmp(data, "IBSP", 4) == 0) {
        qk_result_t res = qk_bsp_load((const u8 *)data, (u64)read, out);
        free(data);
        return res;
    }

    qk_result_t res = qk_map_load_from_memory(data, (u64)read, out);
    free(data);
    return res;
}

void qk_map_free(qk_map_data_t *map) {
    if (!map) return;

    // Free collision model
    if (map->collision.brushes) {
        for (u32 i = 0; i < map->collision.brush_count; i++) {
            free(map->collision.brushes[i].planes);
        }
        free(map->collision.brushes);
    }

    // Free render data
    free(map->vertices);
    free(map->indices);
    free(map->surfaces);
    free(map->lightmap_atlas);

    // Free spawn points
    free(map->spawn_points);

    // Free teleporters and jump pads
    free(map->teleporters);
    free(map->jump_pads);

    memset(map, 0, sizeof(*map));
}
