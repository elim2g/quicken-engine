/*
 * QUICKEN Renderer - Entity Rendering (Capsules, Spheres, Viewmodels)
 *
 * Procedurally generates unit capsule, sphere, and viewmodel weapon meshes
 * at init, uploads them to the GPU. Per-frame draw calls specify position,
 * scale, rotation, and color via push constants.
 */

#include "r_types.h"
#include "renderer/qk_renderer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Mesh generation parameters ---

static const f32 PI_F = 3.14159265358979f;
#define SPHERE_SLICES   16
#define SPHERE_STACKS   12
#define CAPSULE_SLICES  16
#define CAPSULE_STACKS  6   // stacks per hemisphere
#define VIEWMODEL_CYL_SLICES  12
#define VIEWMODEL_HEX_SIDES    6

// --- Helpers ---

static void build_model_matrix(f32 *out, f32 px, f32 py, f32 pz,
                                f32 sx, f32 sy, f32 sz, f32 yaw_deg)
{
    // Column-major 4x4: scale * rotate_y * translate
    f32 rad = yaw_deg * (PI_F / 180.0f);
    f32 cos_yaw = cosf(rad);
    f32 sin_yaw = sinf(rad);

    // col 0
    out[ 0] = sx * cos_yaw;      out[ 1] = 0.0f;  out[ 2] = sx * sin_yaw;  out[ 3] = 0.0f;
    // col 1
    out[ 4] = 0.0f;              out[ 5] = sy;     out[ 6] = 0.0f;          out[ 7] = 0.0f;
    // col 2
    out[ 8] = -sz * sin_yaw;     out[ 9] = 0.0f;  out[10] = sz * cos_yaw;  out[11] = 0.0f;
    // col 3
    out[12] = px;                 out[13] = py;     out[14] = pz;            out[15] = 1.0f;
}

static void color_u32_to_f32(u32 rgba, f32 *out)
{
    out[0] = (f32)((rgba >> 24) & 0xFF) / 255.0f;
    out[1] = (f32)((rgba >> 16) & 0xFF) / 255.0f;
    out[2] = (f32)((rgba >>  8) & 0xFF) / 255.0f;
    out[3] = (f32)((rgba      ) & 0xFF) / 255.0f;
}

// --- Sphere mesh generation ---

static void generate_sphere(r_entity_vertex_t **out_verts, u32 *out_vert_count,
                             u32 **out_indices, u32 *out_index_count)
{
    u32 slices = SPHERE_SLICES;
    u32 stacks = SPHERE_STACKS;

    u32 vert_count = (stacks + 1) * (slices + 1);
    u32 index_count = stacks * slices * 6;

    r_entity_vertex_t *verts = malloc(vert_count * sizeof(r_entity_vertex_t));
    u32 *indices = malloc(index_count * sizeof(u32));

    u32 vi = 0;
    for (u32 st = 0; st <= stacks; st++) {
        f32 phi = PI_F * (f32)st / (f32)stacks;
        f32 sin_phi = sinf(phi);
        f32 cos_phi = cosf(phi);

        for (u32 sl = 0; sl <= slices; sl++) {
            f32 theta = 2.0f * PI_F * (f32)sl / (f32)slices;
            f32 sin_theta = sinf(theta);
            f32 cos_theta = cosf(theta);

            f32 nx = sin_phi * cos_theta;
            f32 ny = cos_phi;
            f32 nz = sin_phi * sin_theta;

            verts[vi].position[0] = nx;
            verts[vi].position[1] = ny;
            verts[vi].position[2] = nz;
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = ny;
            verts[vi].normal[2] = nz;
            vi++;
        }
    }

    u32 ii = 0;
    for (u32 st = 0; st < stacks; st++) {
        for (u32 sl = 0; sl < slices; sl++) {
            u32 a = st * (slices + 1) + sl;
            u32 b = a + slices + 1;

            indices[ii++] = a;
            indices[ii++] = b;
            indices[ii++] = a + 1;

            indices[ii++] = a + 1;
            indices[ii++] = b;
            indices[ii++] = b + 1;
        }
    }

    *out_verts = verts;
    *out_vert_count = vert_count;
    *out_indices = indices;
    *out_index_count = index_count;
}

// --- Capsule mesh generation ---
/* Unit capsule: radius 1.0, total height 2.0 (half_height 1.0 from center to
 * end of cylinder, plus 1.0 hemisphere radius at each end = total 4.0 height).
 * Actually: half_height = distance from center to hemisphere center.
 * The cylinder spans from y=-1 to y=+1, hemispheres cap each end. */

static void generate_capsule(r_entity_vertex_t **out_verts, u32 *out_vert_count,
                               u32 **out_indices, u32 *out_index_count)
{
    u32 slices = CAPSULE_SLICES;
    u32 hemi_stacks = CAPSULE_STACKS;

    /* Top hemisphere + cylinder ring + bottom hemisphere.
     * Top hemisphere: (hemi_stacks+1) rings, bottom: (hemi_stacks+1) rings,
     * cylinder: 2 rings (top and bottom of cylinder section).
     * Total unique rings = hemi_stacks + 1 (top) + 1 (middle shared) + hemi_stacks (bottom) + 1
     * Simplified: top hemi has (hemi_stacks+1) rows, bottom has (hemi_stacks+1) rows,
     * they share the equator row. */
    u32 total_rows = 2 * hemi_stacks + 1; // +1 for equator shared once
    u32 vert_count = (total_rows + 1) * (slices + 1);
    u32 face_count = (total_rows) * slices * 2;
    u32 index_count = face_count * 3;

    r_entity_vertex_t *verts = malloc(vert_count * sizeof(r_entity_vertex_t));
    u32 *indices = malloc(index_count * sizeof(u32));

    u32 vi = 0;

    /* Top hemisphere: from north pole (z = +2) down to equator (z = +1)
     * phi goes from 0 to PI/2 */
    for (u32 st = 0; st <= hemi_stacks; st++) {
        f32 phi = (PI_F * 0.5f) * (f32)st / (f32)hemi_stacks;
        f32 sin_phi = sinf(phi);
        f32 cos_phi = cosf(phi);

        for (u32 sl = 0; sl <= slices; sl++) {
            f32 theta = 2.0f * PI_F * (f32)sl / (f32)slices;
            f32 sin_theta = sinf(theta);
            f32 cos_theta = cosf(theta);

            f32 nx = sin_phi * cos_theta;
            f32 nz = cos_phi;
            f32 ny = sin_phi * sin_theta;

            verts[vi].position[0] = nx;              // radius = 1
            verts[vi].position[1] = ny;
            verts[vi].position[2] = 1.0f + cos_phi;  // hemisphere center at z=+1
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = ny;
            verts[vi].normal[2] = nz;
            vi++;
        }
    }

    /* Bottom hemisphere: from equator (z = -1) down to south pole (z = -2)
     * phi goes from PI/2 to PI */
    for (u32 st = 1; st <= hemi_stacks; st++) {
        f32 phi = (PI_F * 0.5f) + (PI_F * 0.5f) * (f32)st / (f32)hemi_stacks;
        f32 sin_phi = sinf(phi);
        f32 cos_phi = cosf(phi);

        for (u32 sl = 0; sl <= slices; sl++) {
            f32 theta = 2.0f * PI_F * (f32)sl / (f32)slices;
            f32 sin_theta = sinf(theta);
            f32 cos_theta = cosf(theta);

            f32 nx = sin_phi * cos_theta;
            f32 nz = cos_phi;
            f32 ny = sin_phi * sin_theta;

            verts[vi].position[0] = nx;
            verts[vi].position[1] = ny;
            verts[vi].position[2] = -1.0f + cos_phi;  // hemisphere center at z=-1
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = ny;
            verts[vi].normal[2] = nz;
            vi++;
        }
    }

    u32 actual_vert_count = vi;

    /* Total rows of quads = 2*hemi_stacks (top hemi has hemi_stacks quad rows,
     * bottom hemi has hemi_stacks quad rows) */
    u32 ii = 0;
    u32 num_rows = 2 * hemi_stacks;
    for (u32 row = 0; row < num_rows; row++) {
        for (u32 sl = 0; sl < slices; sl++) {
            u32 a = row * (slices + 1) + sl;
            u32 b = a + slices + 1;

            indices[ii++] = a;
            indices[ii++] = b;
            indices[ii++] = a + 1;

            indices[ii++] = a + 1;
            indices[ii++] = b;
            indices[ii++] = b + 1;
        }
    }

    *out_verts = verts;
    *out_vert_count = actual_vert_count;
    *out_indices = indices;
    *out_index_count = ii;
}

// --- Viewmodel mesh helper: emit a cylinder along +X ---

/* Appends a cylinder from x0 to x1 with given radius, using `slices` segments.
 * Returns the number of vertices written (all triangles, no index buffer needed
 * for generation -- we build indexed meshes). Actually we build indexed meshes,
 * so we append verts and indices. */

static void emit_cylinder(r_entity_vertex_t **verts, u32 *vi,
                           u32 **indices, u32 *ii,
                           f32 x0, f32 x1, f32 radius, u32 slices)
{
    u32 base = *vi;
    r_entity_vertex_t *v = *verts;
    u32 *idx = *indices;

    // Two rings of vertices: ring0 at x0, ring1 at x1
    for (u32 ring = 0; ring < 2; ring++) {
        f32 x = (ring == 0) ? x0 : x1;
        for (u32 s = 0; s <= slices; s++) {
            f32 theta = 2.0f * PI_F * (f32)s / (f32)slices;
            f32 cy = cosf(theta) * radius;
            f32 cz = sinf(theta) * radius;
            f32 ny = cosf(theta);
            f32 nz = sinf(theta);

            v[*vi].position[0] = x;
            v[*vi].position[1] = cy;
            v[*vi].position[2] = cz;
            v[*vi].normal[0] = 0.0f;
            v[*vi].normal[1] = ny;
            v[*vi].normal[2] = nz;
            (*vi)++;
        }
    }

    // Indices: quads between ring0 and ring1
    u32 ring_verts = slices + 1;
    for (u32 s = 0; s < slices; s++) {
        u32 a = base + s;
        u32 b = base + s + 1;
        u32 c = base + ring_verts + s;
        u32 d = base + ring_verts + s + 1;

        idx[(*ii)++] = a; idx[(*ii)++] = c; idx[(*ii)++] = b;
        idx[(*ii)++] = b; idx[(*ii)++] = c; idx[(*ii)++] = d;
    }

    // End caps
    // Front cap (at x1): fan from center vertex
    u32 front_center = *vi;
    v[*vi].position[0] = x1; v[*vi].position[1] = 0.0f; v[*vi].position[2] = 0.0f;
    v[*vi].normal[0] = 1.0f; v[*vi].normal[1] = 0.0f; v[*vi].normal[2] = 0.0f;
    (*vi)++;
    for (u32 s = 0; s < slices; s++) {
        idx[(*ii)++] = front_center;
        idx[(*ii)++] = base + ring_verts + s;
        idx[(*ii)++] = base + ring_verts + s + 1;
    }

    // Back cap (at x0): fan from center vertex
    u32 back_center = *vi;
    v[*vi].position[0] = x0; v[*vi].position[1] = 0.0f; v[*vi].position[2] = 0.0f;
    v[*vi].normal[0] = -1.0f; v[*vi].normal[1] = 0.0f; v[*vi].normal[2] = 0.0f;
    (*vi)++;
    for (u32 s = 0; s < slices; s++) {
        idx[(*ii)++] = back_center;
        idx[(*ii)++] = base + s + 1;
        idx[(*ii)++] = base + s;
    }
}

// Emit an axis-aligned box with given bounds
static void emit_box(r_entity_vertex_t **verts, u32 *vi,
                      u32 **indices, u32 *ii,
                      f32 x0, f32 y0, f32 z0,
                      f32 x1, f32 y1, f32 z1)
{
    u32 base = *vi;
    r_entity_vertex_t *v = *verts;
    u32 *idx = *indices;

    // 8 unique positions, but we need 24 verts (4 per face) for correct normals
    f32 corners[8][3] = {
        {x0,y0,z0}, {x1,y0,z0}, {x1,y1,z0}, {x0,y1,z0},
        {x0,y0,z1}, {x1,y0,z1}, {x1,y1,z1}, {x0,y1,z1}
    };

    // 6 faces: {4 corner indices, normal}
    struct { u32 c[4]; f32 n[3]; } faces[6] = {
        {{ 1,5,6,2 }, { 1, 0, 0}},  // +X
        {{ 0,3,7,4 }, {-1, 0, 0}},  // -X
        {{ 3,2,6,7 }, { 0, 1, 0}},  // +Y
        {{ 0,4,5,1 }, { 0,-1, 0}},  // -Y
        {{ 4,7,6,5 }, { 0, 0, 1}},  // +Z
        {{ 0,1,2,3 }, { 0, 0,-1}}   // -Z
    };

    for (u32 f = 0; f < 6; f++) {
        u32 fv = *vi;
        for (u32 c = 0; c < 4; c++) {
            v[*vi].position[0] = corners[faces[f].c[c]][0];
            v[*vi].position[1] = corners[faces[f].c[c]][1];
            v[*vi].position[2] = corners[faces[f].c[c]][2];
            v[*vi].normal[0] = faces[f].n[0];
            v[*vi].normal[1] = faces[f].n[1];
            v[*vi].normal[2] = faces[f].n[2];
            (*vi)++;
        }
        idx[(*ii)++] = fv+0; idx[(*ii)++] = fv+1; idx[(*ii)++] = fv+2;
        idx[(*ii)++] = fv+0; idx[(*ii)++] = fv+2; idx[(*ii)++] = fv+3;
    }

    (void)base;
}

// --- Viewmodel: Rocket Launcher ---
/* Thick barrel cylinder + box body. ~28 units total along +X.
 * Origin near grip area (around x=0). */

static void generate_viewmodel_rl(r_entity_vertex_t **out_verts, u32 *out_vert_count,
                                    u32 **out_indices, u32 *out_index_count)
{
    /* Over-allocate: cylinder ~ 2*(slices+1)+2 verts, 2*slices*2*3 + 2*slices*3 indices
     * box = 24 verts, 36 indices. Two shapes total. */
    u32 max_verts = 512;
    u32 max_indices = 2048;
    r_entity_vertex_t *verts = malloc(max_verts * sizeof(r_entity_vertex_t));
    u32 *indices = malloc(max_indices * sizeof(u32));
    u32 vi = 0, ii = 0;

    // Body box: x from -12 to 0, y from -2.5 to 2.5, z from -3 to 3
    emit_box(&verts, &vi, &indices, &ii,
             -12.0f, -2.5f, -3.0f,
               0.0f,  2.5f,  3.0f);

    // Barrel cylinder: x from -2 to 16, radius 2.0
    emit_cylinder(&verts, &vi, &indices, &ii,
                  -2.0f, 16.0f, 2.0f, VIEWMODEL_CYL_SLICES);

    // Magazine box underneath: x from -10 to -4, y from -5 to -2.5, z from -1.5 to 1.5
    emit_box(&verts, &vi, &indices, &ii,
             -10.0f, -5.0f, -1.5f,
              -4.0f, -2.5f,  1.5f);

    *out_verts = verts;
    *out_vert_count = vi;
    *out_indices = indices;
    *out_index_count = ii;
}

// --- Viewmodel: Railgun ---
/* Long thin barrel + scope on top + wider rear body. ~30 units along +X. */

static void generate_viewmodel_rg(r_entity_vertex_t **out_verts, u32 *out_vert_count,
                                    u32 **out_indices, u32 *out_index_count)
{
    u32 max_verts = 512;
    u32 max_indices = 2048;
    r_entity_vertex_t *verts = malloc(max_verts * sizeof(r_entity_vertex_t));
    u32 *indices = malloc(max_indices * sizeof(u32));
    u32 vi = 0, ii = 0;

    // Main barrel: long thin cylinder x from -5 to 25, radius 1.0
    emit_cylinder(&verts, &vi, &indices, &ii,
                  -5.0f, 25.0f, 1.0f, VIEWMODEL_CYL_SLICES);

    // Rear body: wider box x from -12 to -2, y from -2.5 to 2.5, z from -2.5 to 2.5
    emit_box(&verts, &vi, &indices, &ii,
             -12.0f, -2.5f, -2.5f,
              -2.0f,  2.5f,  2.5f);

    // Scope on top: small box x from -2 to 8, y from 1.0 to 3.5, z from -1.0 to 1.0
    emit_box(&verts, &vi, &indices, &ii,
             -2.0f, 1.0f, -1.0f,
              8.0f, 3.5f,  1.0f);

    // Grip/handle: x from -8 to -4, y from -5 to -2.5, z from -1 to 1
    emit_box(&verts, &vi, &indices, &ii,
             -8.0f, -5.0f, -1.0f,
             -4.0f, -2.5f,  1.0f);

    *out_verts = verts;
    *out_vert_count = vi;
    *out_indices = indices;
    *out_index_count = ii;
}

// --- Viewmodel: Lightning Gun ---
/* Hexagonal barrel + two prongs at muzzle + chunky body. ~24 units along +X. */

static void emit_hex_prism(r_entity_vertex_t **verts, u32 *vi,
                            u32 **indices, u32 *ii,
                            f32 x0, f32 x1, f32 radius)
{
    // Hexagonal prism = cylinder with 6 sides
    emit_cylinder(verts, vi, indices, ii, x0, x1, radius, VIEWMODEL_HEX_SIDES);
}

static void generate_viewmodel_lg(r_entity_vertex_t **out_verts, u32 *out_vert_count,
                                    u32 **out_indices, u32 *out_index_count)
{
    u32 max_verts = 768;
    u32 max_indices = 3072;
    r_entity_vertex_t *verts = malloc(max_verts * sizeof(r_entity_vertex_t));
    u32 *indices = malloc(max_indices * sizeof(u32));
    u32 vi = 0, ii = 0;

    // Main hexagonal barrel: x from -4 to 16, radius 1.8
    emit_hex_prism(&verts, &vi, &indices, &ii,
                   -4.0f, 16.0f, 1.8f);

    // Chunky body box: x from -12 to -2, y from -3 to 3, z from -3.5 to 3.5
    emit_box(&verts, &vi, &indices, &ii,
             -12.0f, -3.0f, -3.5f,
              -2.0f,  3.0f,  3.5f);

    // Top prong: thin box extending from muzzle
    emit_box(&verts, &vi, &indices, &ii,
             14.0f, 1.5f, -0.5f,
             22.0f, 2.5f,  0.5f);

    // Bottom prong: thin box extending from muzzle
    emit_box(&verts, &vi, &indices, &ii,
             14.0f, -2.5f, -0.5f,
             22.0f, -1.5f,  0.5f);

    // Power cell on side: small box
    emit_box(&verts, &vi, &indices, &ii,
             -8.0f, -3.0f, 3.5f,
             -3.0f, -0.5f, 5.0f);

    *out_verts = verts;
    *out_vert_count = vi;
    *out_indices = indices;
    *out_index_count = ii;
}

// --- GPU upload ---

static qk_result_t upload_mesh(r_entity_vertex_t *verts, u32 vert_count,
                                u32 *indices, u32 index_count,
                                r_entity_mesh_t *mesh)
{
    r_staging_reset();

    // Vertices
    VkDeviceSize vb_size = vert_count * sizeof(r_entity_vertex_t);
    VkDeviceSize staging_offset;
    void *staging_ptr = r_staging_alloc(vb_size, &staging_offset);
    if (!staging_ptr) return QK_ERROR_OUT_OF_MEMORY;
    memcpy(staging_ptr, verts, (size_t)vb_size);

    qk_result_t res = r_memory_create_buffer(
        vb_size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &mesh->vertex_buffer, &mesh->vertex_memory);
    if (res != QK_SUCCESS) return res;

    VkCommandBuffer cmd = r_commands_begin_single();
    VkBufferCopy vb_copy = { .srcOffset = staging_offset, .size = vb_size };
    vkCmdCopyBuffer(cmd, g_r.staging.buffer, mesh->vertex_buffer, 1, &vb_copy);
    r_commands_end_single(cmd);

    // Indices
    VkDeviceSize ib_size = index_count * sizeof(u32);
    staging_ptr = r_staging_alloc(ib_size, &staging_offset);
    if (!staging_ptr) return QK_ERROR_OUT_OF_MEMORY;
    memcpy(staging_ptr, indices, (size_t)ib_size);

    res = r_memory_create_buffer(
        ib_size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &mesh->index_buffer, &mesh->index_memory);
    if (res != QK_SUCCESS) return res;

    cmd = r_commands_begin_single();
    VkBufferCopy ib_copy = { .srcOffset = staging_offset, .size = ib_size };
    vkCmdCopyBuffer(cmd, g_r.staging.buffer, mesh->index_buffer, 1, &ib_copy);
    r_commands_end_single(cmd);

    mesh->index_count = index_count;

    return QK_SUCCESS;
}

// --- Public API ---

qk_result_t r_entity_init(void)
{
    memset(&g_r.entities, 0, sizeof(g_r.entities));

    // Generate and upload capsule mesh
    {
        r_entity_vertex_t *verts;
        u32 *indices;
        u32 vert_count, index_count;
        generate_capsule(&verts, &vert_count, &indices, &index_count);

        qk_result_t res = upload_mesh(verts, vert_count, indices, index_count,
                                       &g_r.entities.meshes[R_ENTITY_MESH_CAPSULE]);
        free(verts);
        free(indices);
        if (res != QK_SUCCESS) return res;
    }

    // Generate and upload sphere mesh
    {
        r_entity_vertex_t *verts;
        u32 *indices;
        u32 vert_count, index_count;
        generate_sphere(&verts, &vert_count, &indices, &index_count);

        qk_result_t res = upload_mesh(verts, vert_count, indices, index_count,
                                       &g_r.entities.meshes[R_ENTITY_MESH_SPHERE]);
        free(verts);
        free(indices);
        if (res != QK_SUCCESS) return res;
    }

    // Generate and upload viewmodel meshes
    {
        typedef void (*gen_fn)(r_entity_vertex_t**, u32*, u32**, u32*);
        gen_fn generators[3] = { generate_viewmodel_rl, generate_viewmodel_rg, generate_viewmodel_lg };
        r_entity_mesh_type_t types[3] = {
            R_ENTITY_MESH_VIEWMODEL_RL,
            R_ENTITY_MESH_VIEWMODEL_RG,
            R_ENTITY_MESH_VIEWMODEL_LG
        };

        for (u32 w = 0; w < 3; w++) {
            r_entity_vertex_t *verts;
            u32 *indices;
            u32 vert_count, index_count;
            generators[w](&verts, &vert_count, &indices, &index_count);

            qk_result_t res = upload_mesh(verts, vert_count, indices, index_count,
                                           &g_r.entities.meshes[types[w]]);
            free(verts);
            free(indices);
            if (res != QK_SUCCESS) return res;
        }
    }

    g_r.entities.initialized = true;
    fprintf(stderr, "[Renderer] Entity meshes initialized (capsule + sphere + viewmodels)\n");

    return QK_SUCCESS;
}

void r_entity_shutdown(void)
{
    VkDevice dev = g_r.device.handle;

    for (u32 i = 0; i < R_ENTITY_MESH_COUNT; i++) {
        r_entity_mesh_t *m = &g_r.entities.meshes[i];
        if (m->vertex_buffer) vkDestroyBuffer(dev, m->vertex_buffer, NULL);
        if (m->vertex_memory) vkFreeMemory(dev, m->vertex_memory, NULL);
        if (m->index_buffer) vkDestroyBuffer(dev, m->index_buffer, NULL);
        if (m->index_memory) vkFreeMemory(dev, m->index_memory, NULL);
    }

    memset(&g_r.entities, 0, sizeof(g_r.entities));
}

void r_entity_record_commands(VkCommandBuffer cmd, u32 frame_index)
{
    if (!g_r.entity_pipeline.handle || g_r.entities.draw_count == 0) return;

    r_frame_data_t *frame = &g_r.frames[frame_index];

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_r.entity_pipeline.handle);

    VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (f32)g_r.config.render_width,
        .height   = (f32)g_r.config.render_height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = { g_r.config.render_width, g_r.config.render_height }
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind view UBO (set 0) and light SSBOs (set 1)
    VkDescriptorSet entity_sets[2] = {
        frame->view_descriptor_set,
        g_r.lights.light_descriptor_set
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.entity_pipeline.layout, 0, 2,
                            entity_sets, 0, NULL);

    r_entity_mesh_type_t current_mesh = (r_entity_mesh_type_t)-1;

    // Pass 1: draw non-viewmodel entities (normal depth range)
    for (u32 i = 0; i < g_r.entities.draw_count; i++) {
        r_entity_draw_t *draw = &g_r.entities.draws[i];
        if (draw->mesh_type >= R_ENTITY_MESH_VIEWMODEL_RL) continue;

        if (draw->mesh_type != current_mesh) {
            r_entity_mesh_t *m = &g_r.entities.meshes[draw->mesh_type];
            if (!m->vertex_buffer) continue;

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);
            current_mesh = draw->mesh_type;
        }

        vkCmdPushConstants(cmd, g_r.entity_pipeline.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(r_entity_push_constants_t), &draw->push);

        r_entity_mesh_t *m = &g_r.entities.meshes[draw->mesh_type];
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);

        g_r.stats_draw_calls++;
        g_r.stats_triangles += m->index_count / 3;
    }

    /* Pass 2: draw viewmodels with compressed depth range [0, 0.01].
       This makes them always render in front of world geometry,
       preventing clipping when walking into walls. */
    bool has_viewmodels = false;
    for (u32 i = 0; i < g_r.entities.draw_count; i++) {
        if (g_r.entities.draws[i].mesh_type >= R_ENTITY_MESH_VIEWMODEL_RL) {
            has_viewmodels = true;
            break;
        }
    }

    if (has_viewmodels) {
        VkViewport vm_viewport = viewport;
        vm_viewport.minDepth = 0.0f;
        vm_viewport.maxDepth = 0.01f;
        vkCmdSetViewport(cmd, 0, 1, &vm_viewport);

        current_mesh = (r_entity_mesh_type_t)-1;

        for (u32 i = 0; i < g_r.entities.draw_count; i++) {
            r_entity_draw_t *draw = &g_r.entities.draws[i];
            if (draw->mesh_type < R_ENTITY_MESH_VIEWMODEL_RL) continue;

            if (draw->mesh_type != current_mesh) {
                r_entity_mesh_t *m = &g_r.entities.meshes[draw->mesh_type];
                if (!m->vertex_buffer) continue;

                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
                vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);
                current_mesh = draw->mesh_type;
            }

            vkCmdPushConstants(cmd, g_r.entity_pipeline.layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(r_entity_push_constants_t), &draw->push);

            r_entity_mesh_t *m = &g_r.entities.meshes[draw->mesh_type];
            vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);

            g_r.stats_draw_calls++;
            g_r.stats_triangles += m->index_count / 3;
        }
    }
}

// --- Public draw functions (called between begin_frame and end_frame) ---

void qk_renderer_draw_capsule(f32 pos_x, f32 pos_y, f32 pos_z,
                               f32 radius, f32 half_height,
                               f32 yaw, u32 color_rgba)
{
    if (!g_r.entities.initialized) return;
    if (g_r.entities.draw_count >= R_ENTITY_MAX_DRAWS) return;

    r_entity_draw_t *draw = &g_r.entities.draws[g_r.entities.draw_count++];
    draw->mesh_type = R_ENTITY_MESH_CAPSULE;

    /* The unit capsule has hemisphere centers at y=+1 and y=-1 with radius 1.
     * Scale x/z by radius, y by half_height (stretches cylinder between hemispheres),
     * and add radius to y-scale for the hemisphere caps.
     * Actually the unit capsule total is from y=-2 to y=+2. We want total height
     * = 2*half_height + 2*radius. The unit capsule's height is 4 (from -2 to +2).
     * The cylinder half-span is 1.0 in unit space, hemisphere radius is 1.0.
     * Scale y so that unit half_height(1.0) -> desired half_height, and
     * unit radius(1.0) -> desired radius.
     * Use non-uniform scale: x,z = radius, y = half_height for the cylinder part.
     * But that distorts the hemispheres. For debug visuals this is acceptable. */

    /* Simpler approach: scale uniformly by radius for the sphere parts,
     * scale y by (half_height / 1.0) for cylinder stretch.
     * The unit capsule cylinder half-span is 1.0, total height is 4.0.
     * sy = half_height stretches cylinder to correct size; hemispheres
     * get distorted if radius != half_height, but it looks fine for debug. */
    build_model_matrix(draw->push.model,
                       pos_x, pos_y, pos_z,
                       radius, radius, half_height,
                       yaw);
    color_u32_to_f32(color_rgba, draw->push.color);
}

void qk_renderer_draw_sphere(f32 pos_x, f32 pos_y, f32 pos_z,
                              f32 radius, u32 color_rgba)
{
    if (!g_r.entities.initialized) return;
    if (g_r.entities.draw_count >= R_ENTITY_MAX_DRAWS) return;

    r_entity_draw_t *draw = &g_r.entities.draws[g_r.entities.draw_count++];
    draw->mesh_type = R_ENTITY_MESH_SPHERE;

    build_model_matrix(draw->push.model,
                       pos_x, pos_y, pos_z,
                       radius, radius, radius,
                       0.0f);
    color_u32_to_f32(color_rgba, draw->push.color);
}

// --- Viewmodel rendering ---

void qk_renderer_draw_viewmodel(u32 weapon_id, f32 pitch_deg, f32 yaw_deg,
                                 f32 time_seconds, bool firing)
{
    if (!g_r.entities.initialized) return;
    if (g_r.entities.draw_count >= R_ENTITY_MAX_DRAWS) return;
    if (weapon_id == 0 || weapon_id >= 4) return; // QK_WEAPON_NONE or invalid

    // Map weapon_id -> mesh type and color
    r_entity_mesh_type_t mesh;
    u32 base_color;
    switch (weapon_id) {
        case 1: mesh = R_ENTITY_MESH_VIEWMODEL_RL; base_color = 0x882200FF; break; // Rocket
        case 2: mesh = R_ENTITY_MESH_VIEWMODEL_RG; base_color = 0x008888FF; break; // Rail
        case 3: mesh = R_ENTITY_MESH_VIEWMODEL_LG; base_color = 0x6688FFFF; break; // LG
        default: return;
    }

    // Get camera position from view UBO
    u32 fi = g_r.frame_index % R_FRAMES_IN_FLIGHT;
    r_view_uniforms_t *view = (r_view_uniforms_t *)g_r.frames[fi].view_ubo_mapped;
    if (!view) return;

    f32 cam_pos[3] = { view->camera_pos[0], view->camera_pos[1], view->camera_pos[2] };

    // Compute camera basis vectors from pitch/yaw
    f32 pitch_rad = pitch_deg * (PI_F / 180.0f);
    f32 yaw_rad = yaw_deg * (PI_F / 180.0f);

    f32 cos_pitch = cosf(pitch_rad);
    f32 sin_pitch = sinf(pitch_rad);
    f32 cos_yaw = cosf(yaw_rad);
    f32 sin_yaw = sinf(yaw_rad);

    // Forward = direction camera is looking (QUAKE convention: yaw around Z, pitch tilts)
    f32 fwd[3] = { cos_pitch * cos_yaw, cos_pitch * sin_yaw, sin_pitch };
    f32 world_up[3] = { 0.0f, 0.0f, 1.0f };

    // Right = cross(forward, world_up) normalized
    f32 right[3] = {
        fwd[1] * world_up[2] - fwd[2] * world_up[1],
        fwd[2] * world_up[0] - fwd[0] * world_up[2],
        fwd[0] * world_up[1] - fwd[1] * world_up[0]
    };
    {
        f32 len = sqrtf(right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
        if (len > 1e-6f) {
            f32 inv = 1.0f / len;
            right[0] *= inv; right[1] *= inv; right[2] *= inv;
        }
    }

    // Up = cross(right, forward)
    f32 up[3] = {
        right[1] * fwd[2] - right[2] * fwd[1],
        right[2] * fwd[0] - right[0] * fwd[2],
        right[0] * fwd[1] - right[1] * fwd[0]
    };

    // Weapon bob
    f32 bob_x = sinf(time_seconds * 4.0f) * 0.3f;
    f32 bob_y = sinf(time_seconds * 8.0f) * 0.15f;

    /* Placement offset from camera: right, down, forward.
       Pushed further right+down so the smaller model sits in
       the lower-right corner like a typical arena FPS. */
    f32 offset_right = 10.0f + bob_x;
    f32 offset_down = -6.0f + bob_y;
    f32 offset_forward = 12.0f;

    f32 pos[3];
    for (int i = 0; i < 3; i++) {
        pos[i] = cam_pos[i]
               + fwd[i] * offset_forward
               + right[i] * offset_right
               + up[i] * offset_down;
    }

    /* Build model matrix: columns are right(=forward of mesh +X), up, forward
     * The mesh barrel points +X, so we want +X -> camera forward direction.
     * Column-major 4x4:
     *   col0 = fwd (mesh +X -> world forward)
     *   col1 = right (mesh +Y -> world right, but we want it to match camera orientation)
     *   col2 = up (mesh +Z -> world up)
     *
     * Actually mesh +Y should map to camera-right, +Z to camera-up for correct
     * orientation. But the mesh was built with +X=barrel forward, +Y=sideways, +Z=up.
     * So: col0 = fwd, col1 = right, col2 = up. */

    r_entity_draw_t *draw = &g_r.entities.draws[g_r.entities.draw_count++];
    draw->mesh_type = mesh;

    f32 vm_scale = 0.65f;
    f32 *m = draw->push.model;
    // col 0: forward (where mesh +X goes) -- scaled
    m[ 0] = fwd[0] * vm_scale;    m[ 1] = fwd[1] * vm_scale;    m[ 2] = fwd[2] * vm_scale;    m[ 3] = 0.0f;
    // col 1: right (where mesh +Y goes) -- scaled
    m[ 4] = right[0] * vm_scale;  m[ 5] = right[1] * vm_scale;  m[ 6] = right[2] * vm_scale;  m[ 7] = 0.0f;
    // col 2: up (where mesh +Z goes) -- scaled
    m[ 8] = up[0] * vm_scale;     m[ 9] = up[1] * vm_scale;     m[10] = up[2] * vm_scale;     m[11] = 0.0f;
    // col 3: translation
    m[12] = pos[0];    m[13] = pos[1];    m[14] = pos[2];    m[15] = 1.0f;

    // Color with optional firing brightness
    f32 color[4];
    color_u32_to_f32(base_color, color);
    if (firing) {
        color[0] = color[0] * 1.5f + 0.15f;
        color[1] = color[1] * 1.5f + 0.15f;
        color[2] = color[2] * 1.5f + 0.15f;
        if (color[0] > 1.0f) color[0] = 1.0f;
        if (color[1] > 1.0f) color[1] = 1.0f;
        if (color[2] > 1.0f) color[2] = 1.0f;
    }
    memcpy(draw->push.color, color, sizeof(f32) * 4);
}
