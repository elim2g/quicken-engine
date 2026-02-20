/*
 * QUICKEN Renderer - GPU Memory Management
 *
 * Simple bump allocator and staging buffer for uploads.
 */

#include "r_types.h"
#include <stdio.h>
#include <string.h>

bool r_memory_find_type(u32 type_filter, VkMemoryPropertyFlags properties, u32 *out_type)
{
    for (u32 i = 0; i < g_r.device.mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (g_r.device.mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            *out_type = i;
            return true;
        }
    }
    fprintf(stderr, "[Renderer] Failed to find suitable memory type\n");
    return false;
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

    /* Try pool suballocation for device-local buffers only.
     * Host-visible buffers need individual allocations for vkMapMemory. */
    i32 pool_index = -1;
    if ((properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
        !(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        g_r.pools[R_MEMORY_POOL_DEVICE_LOCAL].memory) {
        pool_index = R_MEMORY_POOL_DEVICE_LOCAL;
    }

    if (pool_index >= 0) {
        r_memory_pool_t *pool = &g_r.pools[pool_index];
        // Verify the pool's memory type is compatible
        if (mem_req.memoryTypeBits & (1u << pool->memory_type)) {
            VkDeviceSize offset;
            if (r_memory_pool_alloc((u32)pool_index, mem_req.size,
                                     mem_req.alignment, &offset) == QK_SUCCESS) {
                vkBindBufferMemory(g_r.device.handle, *out_buffer, pool->memory, offset);
                *out_memory = VK_NULL_HANDLE; // Pool-owned; caller must not free
                return QK_SUCCESS;
            }
        }
    }

    // Fallback: individual allocation
    u32 mem_type;
    if (!r_memory_find_type((u32)mem_req.memoryTypeBits, properties, &mem_type)) {
        vkDestroyBuffer(g_r.device.handle, *out_buffer, NULL);
        *out_buffer = VK_NULL_HANDLE;
        return QK_ERROR_OUT_OF_MEMORY;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mem_req.size,
        .memoryTypeIndex = mem_type
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

qk_result_t r_memory_pool_alloc(u32 pool_index, VkDeviceSize size, VkDeviceSize alignment,
                                VkDeviceSize *out_offset)
{
    if (pool_index >= R_MEMORY_POOL_COUNT) return QK_ERROR_INVALID_PARAM;

    r_memory_pool_t *pool = &g_r.pools[pool_index];
    if (!pool->memory) return QK_ERROR_INIT_FAILED;

    VkDeviceSize aligned = (pool->offset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > pool->size) {
        fprintf(stderr, "[Renderer] Memory pool %u overflow (need %llu, have %llu)\n",
                pool_index, (unsigned long long)(aligned + size),
                (unsigned long long)(pool->size - pool->offset));
        return QK_ERROR_OUT_OF_MEMORY;
    }

    *out_offset = aligned;
    pool->offset = aligned + size;
    return QK_SUCCESS;
}

static qk_result_t r_memory_pool_create(u32 pool_index, VkDeviceSize size,
                                         VkMemoryPropertyFlags properties)
{
    r_memory_pool_t *pool = &g_r.pools[pool_index];

    // Find a suitable memory type for a generic buffer
    VkBufferCreateInfo dummy_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = 256,
        .usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer dummy;
    vkCreateBuffer(g_r.device.handle, &dummy_info, NULL, &dummy);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_r.device.handle, dummy, &req);
    vkDestroyBuffer(g_r.device.handle, dummy, NULL);

    if (!r_memory_find_type((u32)req.memoryTypeBits, properties, &pool->memory_type)) {
        return QK_ERROR_OUT_OF_MEMORY;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = size,
        .memoryTypeIndex = pool->memory_type
    };

    VkResult vr = vkAllocateMemory(g_r.device.handle, &alloc_info, NULL, &pool->memory);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    pool->size = size;
    pool->offset = 0;

    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vkMapMemory(g_r.device.handle, pool->memory, 0, size, 0, &pool->mapped);
    }

    return QK_SUCCESS;
}

qk_result_t r_memory_init(void)
{
    qk_result_t res;

    res = r_memory_pool_create(R_MEMORY_POOL_DEVICE_LOCAL,
                                R_DEVICE_LOCAL_POOL_SIZE,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (res != QK_SUCCESS) return res;

    /* Host-visible pool not used yet; host-visible buffers (UBOs, UI vertex)
     * use individual allocations since callers need vkMapMemory per-buffer. */

    return QK_SUCCESS;
}

void r_memory_shutdown(void)
{
    for (u32 i = 0; i < R_MEMORY_POOL_COUNT; i++) {
        r_memory_pool_t *pool = &g_r.pools[i];
        if (pool->mapped) {
            vkUnmapMemory(g_r.device.handle, pool->memory);
        }
        if (pool->memory) {
            vkFreeMemory(g_r.device.handle, pool->memory, NULL);
        }
        memset(pool, 0, sizeof(*pool));
    }
}

// --- Staging Buffer ---

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
    // Align to 16 bytes
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
