/*
 * QUICKEN Renderer - Texture Upload, VkImage/VkImageView/VkSampler
 */

#include "r_types.h"
#include <stdio.h>
#include <string.h>

static void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                    VkImageLayout old_layout, VkImageLayout new_layout)
{
    VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

qk_result_t r_texture_init(void)
{
    memset(&g_r.textures, 0, sizeof(g_r.textures));
    g_r.textures.next_free = 0;

    // Create samplers
    VkSamplerCreateInfo nearest_info = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_NEAREST,
        .minFilter    = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .maxLod       = 1.0f
    };
    vkCreateSampler(g_r.device.handle, &nearest_info, NULL, &g_r.textures.sampler_nearest);

    VkSamplerCreateInfo linear_info = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .maxLod       = 1.0f
    };
    vkCreateSampler(g_r.device.handle, &linear_info, NULL, &g_r.textures.sampler_linear);

    // Create 1x1 white pixel default texture (texture 0)
    u8 white_pixel[4] = { 255, 255, 255, 255 };
    u32 tex_id = r_texture_upload(white_pixel, 1, 1, 4, false);
    if (tex_id != 0) {
        fprintf(stderr, "[Renderer] Warning: default texture got unexpected id %u\n", tex_id);
    }

    return QK_SUCCESS;
}

void r_texture_shutdown(void)
{
    VkDevice dev = g_r.device.handle;

    for (u32 i = 0; i < R_MAX_TEXTURES; i++) {
        r_texture_t *tex = &g_r.textures.textures[i];
        if (tex->in_use) {
            if (tex->view) vkDestroyImageView(dev, tex->view, NULL);
            if (tex->image) vkDestroyImage(dev, tex->image, NULL);
            if (tex->memory) vkFreeMemory(dev, tex->memory, NULL);
        }
    }

    if (g_r.textures.sampler_nearest) vkDestroySampler(dev, g_r.textures.sampler_nearest, NULL);
    if (g_r.textures.sampler_linear) vkDestroySampler(dev, g_r.textures.sampler_linear, NULL);

    memset(&g_r.textures, 0, sizeof(g_r.textures));
}

u32 r_texture_upload(const u8 *pixels, u32 width, u32 height, u32 channels, bool nearest)
{
    if (g_r.textures.next_free >= R_MAX_TEXTURES) {
        fprintf(stderr, "[Renderer] Texture limit reached\n");
        return 0;
    }

    u32 id = g_r.textures.next_free;
    r_texture_t *tex = &g_r.textures.textures[id];

    // Determine format
    VkFormat format;
    u32 bpp;
    switch (channels) {
        case 4:  format = VK_FORMAT_R8G8B8A8_SRGB; bpp = 4; break;
        case 3:  format = VK_FORMAT_R8G8B8A8_SRGB; bpp = 4; break; // expand to RGBA
        case 1:  format = VK_FORMAT_R8_UNORM;       bpp = 1; break;
        default: format = VK_FORMAT_R8G8B8A8_SRGB;  bpp = 4; break;
    }

    VkDeviceSize image_size = (VkDeviceSize)width * height * bpp;

    // Stage pixel data
    VkDeviceSize staging_offset;
    void *staging_ptr = r_staging_alloc(image_size, &staging_offset);
    if (!staging_ptr) return 0;

    if (channels == 3) {
        // Expand RGB to RGBA
        u8 *dst = (u8 *)staging_ptr;
        for (u32 i = 0; i < width * height; i++) {
            dst[i * 4 + 0] = pixels[i * 3 + 0];
            dst[i * 4 + 1] = pixels[i * 3 + 1];
            dst[i * 4 + 2] = pixels[i * 3 + 2];
            dst[i * 4 + 3] = 255;
        }
    } else {
        memcpy(staging_ptr, pixels, (size_t)image_size);
    }

    // Create image
    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    vkCreateImage(g_r.device.handle, &img_info, NULL, &tex->image);

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(g_r.device.handle, tex->image, &mem_req);

    // Try pool suballocation for device-local texture memory
    r_memory_pool_t *pool = &g_r.pools[R_MEMORY_POOL_DEVICE_LOCAL];
    if (pool->memory && (mem_req.memoryTypeBits & (1u << pool->memory_type))) {
        VkDeviceSize pool_offset;
        if (r_memory_pool_alloc(R_MEMORY_POOL_DEVICE_LOCAL, mem_req.size,
                                 mem_req.alignment, &pool_offset) == QK_SUCCESS) {
            vkBindImageMemory(g_r.device.handle, tex->image, pool->memory, pool_offset);
            tex->memory = VK_NULL_HANDLE; // Pool-owned
            goto texture_create_view;
        }
    }

    {
        u32 tex_mem_type;
        if (!r_memory_find_type((u32)mem_req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tex_mem_type)) {
            vkDestroyImage(g_r.device.handle, tex->image, NULL);
            tex->image = VK_NULL_HANDLE;
            return 0;
        }

        VkMemoryAllocateInfo alloc_info = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = mem_req.size,
            .memoryTypeIndex = tex_mem_type
        };
        vkAllocateMemory(g_r.device.handle, &alloc_info, NULL, &tex->memory);
        vkBindImageMemory(g_r.device.handle, tex->image, tex->memory, 0);
    }

texture_create_view:
    ;

    // Create image view
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = tex->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };
    vkCreateImageView(g_r.device.handle, &view_info, NULL, &tex->view);

    // Transfer: transition, copy, transition
    VkCommandBuffer cmd = r_commands_begin_single();

    transition_image_layout(cmd, tex->image,
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset      = staging_offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 }
    };

    vkCmdCopyBufferToImage(cmd, g_r.staging.buffer, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_image_layout(cmd, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    r_commands_end_single(cmd);

    // Allocate per-texture descriptor set (UI pipeline compatibility)
    VkDescriptorSetAllocateInfo desc_alloc = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = g_r.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &g_r.texture_set_layout
    };
    vkAllocateDescriptorSets(g_r.device.handle, &desc_alloc, &tex->descriptor_set);

    VkDescriptorImageInfo img_desc = {
        .sampler     = nearest ? g_r.textures.sampler_nearest : g_r.textures.sampler_linear,
        .imageView   = tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    // Write into per-texture descriptor set AND bindless array
    VkWriteDescriptorSet writes[2] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = tex->descriptor_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &img_desc
        },
        {
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet           = g_r.bindless_descriptor_set,
            .dstBinding       = 0,
            .dstArrayElement  = id,
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo       = &img_desc
        }
    };
    vkUpdateDescriptorSets(g_r.device.handle, 2, writes, 0, NULL);

    tex->width  = width;
    tex->height = height;
    tex->format = format;
    tex->in_use = true;

    // Find next free slot
    g_r.textures.next_free = id + 1;
    while (g_r.textures.next_free < R_MAX_TEXTURES &&
           g_r.textures.textures[g_r.textures.next_free].in_use) {
        g_r.textures.next_free++;
    }

    return id;
}

VkDescriptorSet r_texture_get_descriptor(u32 texture_id)
{
    if (texture_id >= R_MAX_TEXTURES || !g_r.textures.textures[texture_id].in_use) {
        // Return default (white) texture descriptor
        return g_r.textures.textures[0].descriptor_set;
    }
    return g_r.textures.textures[texture_id].descriptor_set;
}
