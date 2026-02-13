/*
 * QUICKEN Renderer - UI Quad Rendering (Immediate Mode)
 */

#include "renderer/r_types.h"
#include <stdio.h>
#include <string.h>

qk_result_t r_ui_init(void)
{
    /* Build pre-computed index buffer for quads:
     * quad N: indices (N*4+0, N*4+1, N*4+2, N*4+2, N*4+3, N*4+0) */
    VkDeviceSize ib_size = R_UI_MAX_QUADS * 6 * sizeof(u16);

    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;
    qk_result_t res = r_memory_create_buffer(
        ib_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging_buf, &staging_mem);
    if (res != QK_SUCCESS) return res;

    void *mapped;
    vkMapMemory(g_r.device.handle, staging_mem, 0, ib_size, 0, &mapped);

    u16 *indices = (u16 *)mapped;
    for (u32 i = 0; i < R_UI_MAX_QUADS; i++) {
        u16 base = (u16)(i * 4);
        indices[i * 6 + 0] = base + 0;
        indices[i * 6 + 1] = base + 1;
        indices[i * 6 + 2] = base + 2;
        indices[i * 6 + 3] = base + 2;
        indices[i * 6 + 4] = base + 3;
        indices[i * 6 + 5] = base + 0;
    }

    vkUnmapMemory(g_r.device.handle, staging_mem);

    /* Create device-local index buffer */
    res = r_memory_create_buffer(
        ib_size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_r.ui_index_buffer, &g_r.ui_index_memory);
    if (res != QK_SUCCESS) {
        vkDestroyBuffer(g_r.device.handle, staging_buf, NULL);
        vkFreeMemory(g_r.device.handle, staging_mem, NULL);
        return res;
    }

    /* Copy */
    VkCommandBuffer cmd = r_commands_begin_single();
    VkBufferCopy copy = { .size = ib_size };
    vkCmdCopyBuffer(cmd, staging_buf, g_r.ui_index_buffer, 1, &copy);
    r_commands_end_single(cmd);

    vkDestroyBuffer(g_r.device.handle, staging_buf, NULL);
    vkFreeMemory(g_r.device.handle, staging_mem, NULL);

    return QK_SUCCESS;
}

void r_ui_shutdown(void)
{
    VkDevice dev = g_r.device.handle;
    if (g_r.ui_index_buffer) vkDestroyBuffer(dev, g_r.ui_index_buffer, NULL);
    if (g_r.ui_index_memory) vkFreeMemory(dev, g_r.ui_index_memory, NULL);
    g_r.ui_index_buffer = VK_NULL_HANDLE;
    g_r.ui_index_memory = VK_NULL_HANDLE;
}

void r_ui_record_commands(VkCommandBuffer cmd, u32 frame_index)
{
    if (!g_r.ui_pipeline.handle || g_r.ui_quad_count == 0) return;

    r_frame_data_t *frame = &g_r.frames[frame_index];

    /* Write vertices into the frame's mapped buffer */
    r_ui_vertex_t *verts = (r_ui_vertex_t *)frame->ui_vertex_mapped;
    u32 vertex_count = 0;

    for (u32 i = 0; i < g_r.ui_quad_count; i++) {
        r_ui_quad_t *q = &g_r.ui_quads[i];

        f32 x0 = q->x;
        f32 y0 = q->y;
        f32 x1 = q->x + q->w;
        f32 y1 = q->y + q->h;

        /* 4 vertices per quad: TL, TR, BR, BL */
        verts[vertex_count + 0] = (r_ui_vertex_t){
            .position = { x0, y0 }, .uv = { q->u0, q->v0 }, .color = q->color
        };
        verts[vertex_count + 1] = (r_ui_vertex_t){
            .position = { x1, y0 }, .uv = { q->u1, q->v0 }, .color = q->color
        };
        verts[vertex_count + 2] = (r_ui_vertex_t){
            .position = { x1, y1 }, .uv = { q->u1, q->v1 }, .color = q->color
        };
        verts[vertex_count + 3] = (r_ui_vertex_t){
            .position = { x0, y1 }, .uv = { q->u0, q->v1 }, .color = q->color
        };

        vertex_count += 4;
    }

    /* Bind pipeline */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_r.ui_pipeline.handle);

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

    /* Push screen size */
    f32 screen_size[2] = {
        (f32)g_r.config.render_width,
        (f32)g_r.config.render_height
    };
    vkCmdPushConstants(cmd, g_r.ui_pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(screen_size), screen_size);

    /* Bind vertex/index buffers */
    VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &frame->ui_vertex_buffer, &vb_offset);
    vkCmdBindIndexBuffer(cmd, g_r.ui_index_buffer, 0, VK_INDEX_TYPE_UINT16);

    /* Draw quads batched by texture */
    u32 batch_start = 0;
    u32 current_texture = g_r.ui_quads[0].texture_id;

    for (u32 i = 0; i <= g_r.ui_quad_count; i++) {
        bool flush = (i == g_r.ui_quad_count) ||
                     (g_r.ui_quads[i].texture_id != current_texture);

        if (flush && i > batch_start) {
            VkDescriptorSet tex_desc = r_texture_get_descriptor(current_texture);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    g_r.ui_pipeline.layout, 0, 1,
                                    &tex_desc, 0, NULL);

            u32 quad_count = i - batch_start;
            u32 first_index = batch_start * 6;
            u32 index_count = quad_count * 6;

            vkCmdDrawIndexed(cmd, index_count, 1, first_index, 0, 0);

            g_r.stats_draw_calls++;
            g_r.stats_triangles += quad_count * 2;

            if (i < g_r.ui_quad_count) {
                batch_start = i;
                current_texture = g_r.ui_quads[i].texture_id;
            }
        }
    }
}
