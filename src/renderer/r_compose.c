/*
 * QUICKEN Renderer - Composition Pass
 *
 * Samples world and UI offscreen render targets, blends UI over world,
 * outputs to swapchain via a fullscreen triangle.
 */

#include "r_types.h"
#include <string.h>

static r_compose_push_constants_t compute_viewport(
    u32 render_w, u32 render_h,
    u32 window_w, u32 window_h,
    bool aspect_fit)
{
    r_compose_push_constants_t pc;
    memset(&pc, 0, sizeof(pc));
    pc.mode = aspect_fit ? 1 : 0;

    if (!aspect_fit) {
        pc.viewport[0] = 0.0f;
        pc.viewport[1] = 0.0f;
        pc.viewport[2] = 1.0f;
        pc.viewport[3] = 1.0f;
    } else {
        f32 render_aspect = (f32)render_w / (f32)render_h;
        f32 window_aspect = (f32)window_w / (f32)window_h;

        if (window_aspect > render_aspect) {
            f32 scale = render_aspect / window_aspect;
            pc.viewport[0] = (1.0f - scale) * 0.5f;
            pc.viewport[1] = 0.0f;
            pc.viewport[2] = scale;
            pc.viewport[3] = 1.0f;
        } else {
            f32 scale = window_aspect / render_aspect;
            pc.viewport[0] = 0.0f;
            pc.viewport[1] = (1.0f - scale) * 0.5f;
            pc.viewport[2] = 1.0f;
            pc.viewport[3] = scale;
        }
    }
    return pc;
}

qk_result_t r_compose_init(void)
{
    r_compose_update_descriptors();
    return QK_SUCCESS;
}

void r_compose_shutdown(void)
{
    /* Descriptor set freed with pool; sampler freed in r_descriptors_shutdown */
}

void r_compose_update_descriptors(void)
{
    VkDescriptorImageInfo images[2] = {
        {
            .sampler     = g_r.compose_sampler,
            .imageView   = g_r.world_target.color_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        },
        {
            .sampler     = g_r.compose_sampler,
            .imageView   = g_r.ui_target.color_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        }
    };

    VkWriteDescriptorSet writes[2] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.compose_descriptor_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &images[0]
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.compose_descriptor_set,
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &images[1]
        }
    };

    vkUpdateDescriptorSets(g_r.device.handle, 2, writes, 0, NULL);
}

void r_compose_record_commands(VkCommandBuffer cmd, u32 image_index)
{
    if (!g_r.compose_pipeline.handle) return;

    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };

    VkRenderPassBeginInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = g_r.compose_render_pass,
        .framebuffer     = g_r.compose_framebuffers[image_index],
        .renderArea      = { .offset = { 0, 0 }, .extent = g_r.swapchain.extent },
        .clearValueCount = 1,
        .pClearValues    = &clear
    };

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_r.compose_pipeline.handle);

    VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (f32)g_r.swapchain.extent.width,
        .height   = (f32)g_r.swapchain.extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = g_r.swapchain.extent
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.compose_pipeline.layout, 0, 1,
                            &g_r.compose_descriptor_set, 0, NULL);

    r_compose_push_constants_t pc = compute_viewport(
        g_r.config.render_width, g_r.config.render_height,
        g_r.swapchain.extent.width, g_r.swapchain.extent.height,
        g_r.config.aspect_fit);

    vkCmdPushConstants(cmd, g_r.compose_pipeline.layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);

    /* Draw fullscreen triangle (3 vertices, no buffer) */
    vkCmdDraw(cmd, 3, 1, 0, 0);

    g_r.stats_draw_calls++;
    g_r.stats_triangles += 1;

    vkCmdEndRenderPass(cmd);
}
