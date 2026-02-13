/*
 * QUICKEN Renderer - Command Pool/Buffer and Frame Sync
 */

#include "r_types.h"
#include <stdio.h>
#include <string.h>

/* Temp pool and shared fence for one-shot commands */
static VkCommandPool s_temp_pool = VK_NULL_HANDLE;
static VkFence       s_temp_fence = VK_NULL_HANDLE;

qk_result_t r_commands_init(void)
{
    VkDevice dev = g_r.device.handle;

    /* Create a persistent temp pool for single-use commands */
    {
        VkCommandPoolCreateInfo pool_info = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = g_r.device.families.graphics,
            .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
        };
        VkResult vr = vkCreateCommandPool(dev, &pool_info, NULL, &s_temp_pool);
        if (vr != VK_SUCCESS) return QK_ERROR_VULKAN_INIT;
    }

    /* Shared fence for single-use commands */
    {
        VkFenceCreateInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
        };
        VkResult vr = vkCreateFence(dev, &fence_info, NULL, &s_temp_fence);
        if (vr != VK_SUCCESS) return QK_ERROR_VULKAN_INIT;
    }

    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        r_frame_data_t *frame = &g_r.frames[i];

        /* Command pool (one per frame for efficient reset) */
        VkCommandPoolCreateInfo pool_info = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = g_r.device.families.graphics,
            .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
        };
        VkResult vr = vkCreateCommandPool(dev, &pool_info, NULL, &frame->command_pool);
        if (vr != VK_SUCCESS) return QK_ERROR_VULKAN_INIT;

        /* Command buffer */
        VkCommandBufferAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = frame->command_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };
        vr = vkAllocateCommandBuffers(dev, &alloc_info, &frame->command_buffer);
        if (vr != VK_SUCCESS) return QK_ERROR_VULKAN_INIT;

        /* Semaphores */
        VkSemaphoreCreateInfo sem_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
        };
        vkCreateSemaphore(dev, &sem_info, NULL, &frame->image_available);
        vkCreateSemaphore(dev, &sem_info, NULL, &frame->render_finished);

        /* Fence (start signaled so first wait doesn't block) */
        VkFenceCreateInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT
        };
        vkCreateFence(dev, &fence_info, NULL, &frame->in_flight);

        /* View UBO (host-visible, persistently mapped) */
        qk_result_t res = r_memory_create_buffer(
            sizeof(r_view_uniforms_t),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &frame->view_ubo, &frame->view_ubo_memory);
        if (res != QK_SUCCESS) return res;

        vkMapMemory(dev, frame->view_ubo_memory, 0, sizeof(r_view_uniforms_t), 0,
                    &frame->view_ubo_mapped);

        /* Update view descriptor set */
        VkDescriptorBufferInfo buf_info_desc = {
            .buffer = frame->view_ubo,
            .offset = 0,
            .range  = sizeof(r_view_uniforms_t)
        };
        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = frame->view_descriptor_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &buf_info_desc
        };
        vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);

        /* UI vertex buffer (host-visible, persistently mapped) */
        VkDeviceSize ui_vb_size = R_UI_MAX_QUADS * 4 * sizeof(r_ui_vertex_t);
        res = r_memory_create_buffer(
            ui_vb_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &frame->ui_vertex_buffer, &frame->ui_vertex_memory);
        if (res != QK_SUCCESS) return res;

        vkMapMemory(dev, frame->ui_vertex_memory, 0, ui_vb_size, 0,
                    &frame->ui_vertex_mapped);
    }

    return QK_SUCCESS;
}

void r_commands_shutdown(void)
{
    VkDevice dev = g_r.device.handle;

    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        r_frame_data_t *frame = &g_r.frames[i];

        if (frame->ui_vertex_memory) {
            vkUnmapMemory(dev, frame->ui_vertex_memory);
            vkFreeMemory(dev, frame->ui_vertex_memory, NULL);
        }
        if (frame->ui_vertex_buffer) vkDestroyBuffer(dev, frame->ui_vertex_buffer, NULL);

        if (frame->view_ubo_memory) {
            vkUnmapMemory(dev, frame->view_ubo_memory);
            vkFreeMemory(dev, frame->view_ubo_memory, NULL);
        }
        if (frame->view_ubo) vkDestroyBuffer(dev, frame->view_ubo, NULL);

        if (frame->in_flight) vkDestroyFence(dev, frame->in_flight, NULL);
        if (frame->render_finished) vkDestroySemaphore(dev, frame->render_finished, NULL);
        if (frame->image_available) vkDestroySemaphore(dev, frame->image_available, NULL);
        if (frame->command_pool) vkDestroyCommandPool(dev, frame->command_pool, NULL);
    }

    if (s_temp_fence) {
        vkDestroyFence(dev, s_temp_fence, NULL);
        s_temp_fence = VK_NULL_HANDLE;
    }

    if (s_temp_pool) {
        vkDestroyCommandPool(dev, s_temp_pool, NULL);
        s_temp_pool = VK_NULL_HANDLE;
    }

    memset(g_r.frames, 0, sizeof(g_r.frames));
}

VkCommandBuffer r_commands_begin_single(void)
{
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = s_temp_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_r.device.handle, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    return cmd;
}

void r_commands_end_single(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd
    };

    vkResetFences(g_r.device.handle, 1, &s_temp_fence);
    vkQueueSubmit(g_r.device.graphics_queue, 1, &submit, s_temp_fence);
    vkWaitForFences(g_r.device.handle, 1, &s_temp_fence, VK_TRUE, UINT64_MAX);

    vkFreeCommandBuffers(g_r.device.handle, s_temp_pool, 1, &cmd);
}
