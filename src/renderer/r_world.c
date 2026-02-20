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

    // Bind view UBO (set 0), bindless textures (set 1), light SSBOs (set 2)
    VkDescriptorSet sets[3] = {
        frame->view_descriptor_set,
        g_r.bindless_descriptor_set,
        g_r.lights.light_descriptor_set
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.world_pipeline.layout, 0, 3,
                            sets, 0, NULL);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_r.world.vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, g_r.world.index_buffer, 0, VK_INDEX_TYPE_UINT32);

    // Draw surfaces with push constants for texture index
    for (u32 i = 0; i < g_r.world.surface_count; i++) {
        r_draw_surface_t *surf = &g_r.world.surfaces[i];

        r_world_push_constants_t pc = {
            .texture_index  = surf->texture_index,
            .lightmap_index = g_r.has_lightmaps ? g_r.lightmap_texture_id : 0,
            .overbright     = g_r.has_lightmaps ? 2.0f : 1.0f,
            .ambient        = g_r.ambient
        };
        vkCmdPushConstants(cmd, g_r.world_pipeline.layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        vkCmdDrawIndexed(cmd, surf->index_count, 1,
                         surf->index_offset, (i32)surf->vertex_offset, 0);

        g_r.stats_draw_calls++;
        g_r.stats_triangles += surf->index_count / 3;
    }
}
