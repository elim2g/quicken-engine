/*
 * QUICKEN Renderer - Entity Rendering (Capsules + Spheres)
 *
 * Procedurally generates unit capsule and unit sphere meshes at init,
 * uploads them to the GPU. Per-frame draw calls specify position, scale,
 * rotation, and color via push constants.
 */

#include "r_types.h"
#include "renderer/qk_renderer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Mesh generation parameters ---- */

#define SPHERE_SLICES   16
#define SPHERE_STACKS   12
#define CAPSULE_SLICES  16
#define CAPSULE_STACKS  6   /* stacks per hemisphere */

/* ---- Helpers ---- */

static void build_model_matrix(f32 *out, f32 px, f32 py, f32 pz,
                                f32 sx, f32 sy, f32 sz, f32 yaw_deg)
{
    /* Column-major 4x4: scale * rotate_y * translate */
    f32 rad = yaw_deg * (3.14159265358979f / 180.0f);
    f32 cy = cosf(rad);
    f32 sy_val = sinf(rad);

    /* col 0 */ out[ 0] = sx * cy;  out[ 1] = 0.0f;    out[ 2] = sx * sy_val; out[ 3] = 0.0f;
    /* col 1 */ out[ 4] = 0.0f;     out[ 5] = sy;       out[ 6] = 0.0f;        out[ 7] = 0.0f;
    /* col 2 */ out[ 8] = -sz * sy_val; out[ 9] = 0.0f; out[10] = sz * cy;     out[11] = 0.0f;
    /* col 3 */ out[12] = px;        out[13] = py;       out[14] = pz;          out[15] = 1.0f;
}

static void color_u32_to_f32(u32 rgba, f32 *out)
{
    out[0] = (f32)((rgba >> 24) & 0xFF) / 255.0f;
    out[1] = (f32)((rgba >> 16) & 0xFF) / 255.0f;
    out[2] = (f32)((rgba >>  8) & 0xFF) / 255.0f;
    out[3] = (f32)((rgba      ) & 0xFF) / 255.0f;
}

/* ---- Sphere mesh generation ---- */

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
        f32 phi = 3.14159265358979f * (f32)st / (f32)stacks;
        f32 sp = sinf(phi);
        f32 cp = cosf(phi);

        for (u32 sl = 0; sl <= slices; sl++) {
            f32 theta = 2.0f * 3.14159265358979f * (f32)sl / (f32)slices;
            f32 st_val = sinf(theta);
            f32 ct = cosf(theta);

            f32 nx = sp * ct;
            f32 ny = cp;
            f32 nz = sp * st_val;

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

/* ---- Capsule mesh generation ---- */
/* Unit capsule: radius 1.0, total height 2.0 (half_height 1.0 from center to
 * end of cylinder, plus 1.0 hemisphere radius at each end = total 4.0 height).
 * Actually: half_height = distance from center to hemisphere center.
 * The cylinder spans from y=-1 to y=+1, hemispheres cap each end. */

static void generate_capsule(r_entity_vertex_t **out_verts, u32 *out_vert_count,
                               u32 **out_indices, u32 *out_index_count)
{
    u32 slices = CAPSULE_SLICES;
    u32 hemi_stacks = CAPSULE_STACKS;

    /* Top hemisphere + cylinder ring + bottom hemisphere */
    /* Top hemisphere: (hemi_stacks+1) rings, bottom: (hemi_stacks+1) rings,
     * cylinder: 2 rings (top and bottom of cylinder section).
     * Total unique rings = hemi_stacks + 1 (top) + 1 (middle shared) + hemi_stacks (bottom) + 1
     * Simplified: top hemi has (hemi_stacks+1) rows, bottom has (hemi_stacks+1) rows,
     * they share the equator row. */
    u32 total_rows = 2 * hemi_stacks + 1; /* +1 for equator shared once */
    u32 vert_count = (total_rows + 1) * (slices + 1);
    u32 face_count = (total_rows) * slices * 2;
    u32 index_count = face_count * 3;

    r_entity_vertex_t *verts = malloc(vert_count * sizeof(r_entity_vertex_t));
    u32 *indices = malloc(index_count * sizeof(u32));

    u32 vi = 0;

    /* Top hemisphere: from north pole (z = +2) down to equator (z = +1)
     * phi goes from 0 to PI/2 */
    for (u32 st = 0; st <= hemi_stacks; st++) {
        f32 phi = (3.14159265358979f * 0.5f) * (f32)st / (f32)hemi_stacks;
        f32 sp = sinf(phi);
        f32 cp = cosf(phi);

        for (u32 sl = 0; sl <= slices; sl++) {
            f32 theta = 2.0f * 3.14159265358979f * (f32)sl / (f32)slices;
            f32 st_val = sinf(theta);
            f32 ct = cosf(theta);

            f32 nx = sp * ct;
            f32 nz = cp;
            f32 ny = sp * st_val;

            verts[vi].position[0] = nx;         /* radius = 1 */
            verts[vi].position[1] = ny;
            verts[vi].position[2] = 1.0f + cp;  /* hemisphere center at z=+1 */
            verts[vi].normal[0] = nx;
            verts[vi].normal[1] = ny;
            verts[vi].normal[2] = nz;
            vi++;
        }
    }

    /* Bottom hemisphere: from equator (z = -1) down to south pole (z = -2)
     * phi goes from PI/2 to PI */
    for (u32 st = 1; st <= hemi_stacks; st++) {
        f32 phi = (3.14159265358979f * 0.5f) + (3.14159265358979f * 0.5f) * (f32)st / (f32)hemi_stacks;
        f32 sp = sinf(phi);
        f32 cp = cosf(phi);

        for (u32 sl = 0; sl <= slices; sl++) {
            f32 theta = 2.0f * 3.14159265358979f * (f32)sl / (f32)slices;
            f32 st_val = sinf(theta);
            f32 ct = cosf(theta);

            f32 nx = sp * ct;
            f32 nz = cp;
            f32 ny = sp * st_val;

            verts[vi].position[0] = nx;
            verts[vi].position[1] = ny;
            verts[vi].position[2] = -1.0f + cp;  /* hemisphere center at z=-1 */
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

/* ---- GPU upload ---- */

static qk_result_t upload_mesh(r_entity_vertex_t *verts, u32 vert_count,
                                u32 *indices, u32 index_count,
                                r_entity_mesh_t *mesh)
{
    r_staging_reset();

    /* Vertices */
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

    /* Indices */
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

/* ---- Public API ---- */

qk_result_t r_entity_init(void)
{
    memset(&g_r.entities, 0, sizeof(g_r.entities));

    /* Generate and upload capsule mesh */
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

    /* Generate and upload sphere mesh */
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

    g_r.entities.initialized = true;
    fprintf(stderr, "[Renderer] Entity meshes initialized (capsule + sphere)\n");

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

    /* Bind view UBO (set 0, same as world pipeline) */
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.entity_pipeline.layout, 0, 1,
                            &frame->view_descriptor_set, 0, NULL);

    r_entity_mesh_type_t current_mesh = (r_entity_mesh_type_t)-1;

    for (u32 i = 0; i < g_r.entities.draw_count; i++) {
        r_entity_draw_t *draw = &g_r.entities.draws[i];

        /* Bind mesh if changed */
        if (draw->mesh_type != current_mesh) {
            r_entity_mesh_t *m = &g_r.entities.meshes[draw->mesh_type];
            if (!m->vertex_buffer) continue;

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m->vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, m->index_buffer, 0, VK_INDEX_TYPE_UINT32);
            current_mesh = draw->mesh_type;
        }

        /* Push model matrix + color */
        vkCmdPushConstants(cmd, g_r.entity_pipeline.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(r_entity_push_constants_t), &draw->push);

        r_entity_mesh_t *m = &g_r.entities.meshes[draw->mesh_type];
        vkCmdDrawIndexed(cmd, m->index_count, 1, 0, 0, 0);

        g_r.stats_draw_calls++;
        g_r.stats_triangles += m->index_count / 3;
    }
}

/* ---- Public draw functions (called between begin_frame and end_frame) ---- */

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
