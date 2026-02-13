/*
 * QUICKEN Renderer - World Geometry Rendering
 */

#include "r_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void r_world_init(void)
{
    memset(&g_r.world, 0, sizeof(g_r.world));
}

void r_world_shutdown(void)
{
    VkDevice dev = g_r.device.handle;

    if (g_r.world.vertex_buffer) vkDestroyBuffer(dev, g_r.world.vertex_buffer, NULL);
    if (g_r.world.vertex_memory) vkFreeMemory(dev, g_r.world.vertex_memory, NULL);
    if (g_r.world.index_buffer) vkDestroyBuffer(dev, g_r.world.index_buffer, NULL);
    if (g_r.world.index_memory) vkFreeMemory(dev, g_r.world.index_memory, NULL);
    free(g_r.world.surfaces);

    memset(&g_r.world, 0, sizeof(g_r.world));
}

void r_world_record_commands(VkCommandBuffer cmd, u32 frame_index)
{
    if (!g_r.world_pipeline.handle || g_r.world.vertex_count == 0) return;

    r_frame_data_t *frame = &g_r.frames[frame_index];

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_r.world_pipeline.handle);

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

    /* Bind view UBO (set 0) */
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.world_pipeline.layout, 0, 1,
                            &frame->view_descriptor_set, 0, NULL);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_r.world.vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, g_r.world.index_buffer, 0, VK_INDEX_TYPE_UINT32);

    /* Draw surfaces sorted by texture */
    u32 current_texture = UINT32_MAX;
    for (u32 i = 0; i < g_r.world.surface_count; i++) {
        r_draw_surface_t *surf = &g_r.world.surfaces[i];

        if (surf->texture_index != current_texture) {
            VkDescriptorSet tex_desc = r_texture_get_descriptor(surf->texture_index);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    g_r.world_pipeline.layout, 1, 1,
                                    &tex_desc, 0, NULL);
            current_texture = surf->texture_index;
        }

        vkCmdDrawIndexed(cmd, surf->index_count, 1,
                         surf->index_offset, (i32)surf->vertex_offset, 0);

        g_r.stats_draw_calls++;
        g_r.stats_triangles += surf->index_count / 3;
    }
}
