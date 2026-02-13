/*
 * QUICKEN Renderer - GPU Memory Management
 *
 * Simple bump allocator and staging buffer for uploads.
 */

#include "renderer/r_types.h"
#include <stdio.h>
#include <string.h>

u32 r_memory_find_type(u32 type_filter, VkMemoryPropertyFlags properties)
{
    for (u32 i = 0; i < g_r.device.mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (g_r.device.mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "[Renderer] Failed to find suitable memory type\n");
    return 0;
}

qk_result_t r_memory_create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags properties,
                                   VkBuffer *out_buffer, VkDeviceMemory *out_memory)
{
    VkBufferCreateInfo buf_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkResult vr = vkCreateBuffer(g_r.device.handle, &buf_info, NULL, out_buffer);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(g_r.device.handle, *out_buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mem_req.size,
        .memoryTypeIndex = r_memory_find_type((u32)mem_req.memoryTypeBits, properties)
    };

    vr = vkAllocateMemory(g_r.device.handle, &alloc_info, NULL, out_memory);
    if (vr != VK_SUCCESS) {
        vkDestroyBuffer(g_r.device.handle, *out_buffer, NULL);
        *out_buffer = VK_NULL_HANDLE;
        return QK_ERROR_OUT_OF_MEMORY;
    }

    vkBindBufferMemory(g_r.device.handle, *out_buffer, *out_memory, 0);
    return QK_SUCCESS;
}

qk_result_t r_memory_init(void)
{
    /* Nothing to do for now -- we allocate per-resource.
     * A pool allocator can be added later for better performance. */
    return QK_SUCCESS;
}

void r_memory_shutdown(void)
{
    /* Per-resource cleanup happens in their respective modules */
}

/* ---- Staging Buffer ---- */

qk_result_t r_staging_init(void)
{
    qk_result_t res = r_memory_create_buffer(
        R_STAGING_BUFFER_SIZE,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &g_r.staging.buffer, &g_r.staging.memory);
    if (res != QK_SUCCESS) return res;

    g_r.staging.size = R_STAGING_BUFFER_SIZE;
    g_r.staging.offset = 0;

    vkMapMemory(g_r.device.handle, g_r.staging.memory, 0, g_r.staging.size, 0, &g_r.staging.mapped);

    return QK_SUCCESS;
}

void r_staging_shutdown(void)
{
    if (g_r.staging.memory) {
        vkUnmapMemory(g_r.device.handle, g_r.staging.memory);
    }
    if (g_r.staging.buffer) {
        vkDestroyBuffer(g_r.device.handle, g_r.staging.buffer, NULL);
    }
    if (g_r.staging.memory) {
        vkFreeMemory(g_r.device.handle, g_r.staging.memory, NULL);
    }
    memset(&g_r.staging, 0, sizeof(g_r.staging));
}

void r_staging_reset(void)
{
    g_r.staging.offset = 0;
}

void *r_staging_alloc(VkDeviceSize size, VkDeviceSize *out_offset)
{
    /* Align to 16 bytes */
    VkDeviceSize aligned = (g_r.staging.offset + 15) & ~((VkDeviceSize)15);

    if (aligned + size > g_r.staging.size) {
        fprintf(stderr, "[Renderer] Staging buffer overflow\n");
        return NULL;
    }

    *out_offset = aligned;
    g_r.staging.offset = aligned + size;

    return (u8 *)g_r.staging.mapped + aligned;
}

void r_staging_flush_copies(VkCommandBuffer cmd)
{
    QK_UNUSED(cmd);
    /* Copies are recorded inline by callers.
     * This function exists as a hook for future batching. */
}
