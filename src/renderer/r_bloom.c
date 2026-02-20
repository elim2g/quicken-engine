/*
 * QUICKEN Renderer - Bloom Effect
 *
 * Dual-filter bloom: 13-tap downsample with brightness threshold,
 * 9-tap tent upsample with additive blending. 5-level mip chain.
 */

#include "r_types.h"
#include <string.h>

// Forward declaration from r_vulkan.c (file-scope helper)
static qk_result_t bloom_create_image(u32 w, u32 h,
                                      VkImage *out_image,
                                      VkDeviceMemory *out_memory,
                                      VkImageView *out_view)
{
    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent        = { w, h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkResult vr = vkCreateImage(g_r.device.handle, &img_info, NULL, out_image);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(g_r.device.handle, *out_image, &mem_req);

    // Try pool suballocation
    r_memory_pool_t *pool = &g_r.pools[R_MEMORY_POOL_DEVICE_LOCAL];
    if (pool->memory && (mem_req.memoryTypeBits & (1u << pool->memory_type))) {
        VkDeviceSize offset;
        if (r_memory_pool_alloc(R_MEMORY_POOL_DEVICE_LOCAL, mem_req.size,
                                 mem_req.alignment, &offset) == QK_SUCCESS) {
            vkBindImageMemory(g_r.device.handle, *out_image, pool->memory, offset);
            *out_memory = VK_NULL_HANDLE;
            goto create_view;
        }
    }

    {
        u32 mem_type;
        if (!r_memory_find_type((u32)mem_req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_type)) {
            vkDestroyImage(g_r.device.handle, *out_image, NULL);
            return QK_ERROR_OUT_OF_MEMORY;
        }
        VkMemoryAllocateInfo alloc_info = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = mem_req.size,
            .memoryTypeIndex = mem_type
        };
        vr = vkAllocateMemory(g_r.device.handle, &alloc_info, NULL, out_memory);
        if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;
        vkBindImageMemory(g_r.device.handle, *out_image, *out_memory, 0);
    }

create_view:
    ;
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = *out_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };
    vr = vkCreateImageView(g_r.device.handle, &view_info, NULL, out_view);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    return QK_SUCCESS;
}

static qk_result_t create_bloom_render_passes(void)
{
    // Downsample render pass: DONT_CARE load, STORE
    {
        VkAttachmentDescription attachment = {
            .format         = VK_FORMAT_R16G16B16A16_SFLOAT,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {
            .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color_ref
        };
        VkSubpassDependency dep = {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        };
        VkRenderPassCreateInfo rp_info = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &attachment,
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .dependencyCount = 1,
            .pDependencies   = &dep
        };
        VkResult vr = vkCreateRenderPass(g_r.device.handle, &rp_info, NULL,
                                          &g_r.bloom.render_pass);
        if (vr != VK_SUCCESS) return QK_ERROR_PIPELINE;
    }

    // Upsample render pass: LOAD existing content (downsample result), STORE
    {
        VkAttachmentDescription attachment = {
            .format         = VK_FORMAT_R16G16B16A16_SFLOAT,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {
            .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color_ref
        };
        VkSubpassDependency dep = {
            .srcSubpass    = VK_SUBPASS_EXTERNAL,
            .dstSubpass    = 0,
            .srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        };
        VkRenderPassCreateInfo rp_info = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &attachment,
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .dependencyCount = 1,
            .pDependencies   = &dep
        };
        VkResult vr = vkCreateRenderPass(g_r.device.handle, &rp_info, NULL,
                                          &g_r.bloom.additive_render_pass);
        if (vr != VK_SUCCESS) return QK_ERROR_PIPELINE;
    }

    return QK_SUCCESS;
}

static qk_result_t create_bloom_pipelines(void)
{
    VkShaderModule vert = r_pipeline_load_shader("src/renderer/shaders/compiled/compose.vert.spv");
    VkShaderModule down_frag = r_pipeline_load_shader("src/renderer/shaders/compiled/bloom_downsample.frag.spv");
    VkShaderModule up_frag = r_pipeline_load_shader("src/renderer/shaders/compiled/bloom_upsample.frag.spv");

    if (!vert || !down_frag || !up_frag) {
        if (vert) vkDestroyShaderModule(g_r.device.handle, vert, NULL);
        if (down_frag) vkDestroyShaderModule(g_r.device.handle, down_frag, NULL);
        if (up_frag) vkDestroyShaderModule(g_r.device.handle, up_frag, NULL);
        return QK_ERROR_PIPELINE;
    }

    // Shared pipeline state
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1
    };
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .lineWidth   = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states
    };

    // Push constant range: texel_size(8) + threshold(4) + intensity(4) = 16 bytes
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(r_bloom_push_constants_t)
    };

    // Layout: set 0 = single sampler (reuse texture_set_layout)
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &g_r.texture_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range
    };

    // Downsample pipeline (opaque blend)
    {
        vkCreatePipelineLayout(g_r.device.handle, &layout_info, NULL,
                               &g_r.bloom.downsample_pipeline.layout);

        VkPipelineShaderStageCreateInfo stages[2] = {
            {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vert,
                .pName  = "main"
            },
            {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = down_frag,
                .pName  = "main"
            }
        };

        VkPipelineColorBlendAttachmentState blend = {
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        };
        VkPipelineColorBlendStateCreateInfo color_blend = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &blend
        };

        VkGraphicsPipelineCreateInfo pipeline_info = {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2,
            .pStages             = stages,
            .pVertexInputState   = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState      = &viewport_state,
            .pRasterizationState = &rasterizer,
            .pMultisampleState   = &multisample,
            .pDepthStencilState  = &depth_stencil,
            .pColorBlendState    = &color_blend,
            .pDynamicState       = &dynamic_state,
            .layout              = g_r.bloom.downsample_pipeline.layout,
            .renderPass          = g_r.bloom.render_pass,
            .subpass             = 0
        };

        VkResult vr = vkCreateGraphicsPipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                                 1, &pipeline_info, NULL,
                                                 &g_r.bloom.downsample_pipeline.handle);
        if (vr != VK_SUCCESS) {
            vkDestroyShaderModule(g_r.device.handle, vert, NULL);
            vkDestroyShaderModule(g_r.device.handle, down_frag, NULL);
            vkDestroyShaderModule(g_r.device.handle, up_frag, NULL);
            return QK_ERROR_PIPELINE;
        }
    }

    // Upsample pipeline (additive blend: src ONE + dst ONE)
    {
        vkCreatePipelineLayout(g_r.device.handle, &layout_info, NULL,
                               &g_r.bloom.upsample_pipeline.layout);

        VkPipelineShaderStageCreateInfo stages[2] = {
            {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vert,
                .pName  = "main"
            },
            {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = up_frag,
                .pName  = "main"
            }
        };

        VkPipelineColorBlendAttachmentState blend = {
            .blendEnable         = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .colorBlendOp        = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp        = VK_BLEND_OP_ADD,
            .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        };
        VkPipelineColorBlendStateCreateInfo color_blend = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &blend
        };

        VkGraphicsPipelineCreateInfo pipeline_info = {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2,
            .pStages             = stages,
            .pVertexInputState   = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState      = &viewport_state,
            .pRasterizationState = &rasterizer,
            .pMultisampleState   = &multisample,
            .pDepthStencilState  = &depth_stencil,
            .pColorBlendState    = &color_blend,
            .pDynamicState       = &dynamic_state,
            .layout              = g_r.bloom.upsample_pipeline.layout,
            .renderPass          = g_r.bloom.additive_render_pass,
            .subpass             = 0
        };

        VkResult vr = vkCreateGraphicsPipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                                 1, &pipeline_info, NULL,
                                                 &g_r.bloom.upsample_pipeline.handle);
        if (vr != VK_SUCCESS) {
            vkDestroyShaderModule(g_r.device.handle, vert, NULL);
            vkDestroyShaderModule(g_r.device.handle, down_frag, NULL);
            vkDestroyShaderModule(g_r.device.handle, up_frag, NULL);
            return QK_ERROR_PIPELINE;
        }
    }

    vkDestroyShaderModule(g_r.device.handle, vert, NULL);
    vkDestroyShaderModule(g_r.device.handle, down_frag, NULL);
    vkDestroyShaderModule(g_r.device.handle, up_frag, NULL);

    return QK_SUCCESS;
}

qk_result_t r_bloom_init(void)
{
    memset(&g_r.bloom, 0, sizeof(g_r.bloom));

    u32 w = g_r.config.render_width / 2;
    u32 h = g_r.config.render_height / 2;

    // Render passes
    qk_result_t res = create_bloom_render_passes();
    if (res != QK_SUCCESS) return res;

    // Create mip chain images
    for (u32 i = 0; i < R_BLOOM_MIP_COUNT; i++) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        g_r.bloom.mip_extents[i] = (VkExtent2D){ w, h };

        res = bloom_create_image(w, h,
                                 &g_r.bloom.mip_images[i],
                                 &g_r.bloom.mip_memories[i],
                                 &g_r.bloom.mip_views[i]);
        if (res != QK_SUCCESS) return res;

        // Downsample framebuffer
        VkFramebufferCreateInfo fb_info = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = g_r.bloom.render_pass,
            .attachmentCount = 1,
            .pAttachments    = &g_r.bloom.mip_views[i],
            .width           = w,
            .height          = h,
            .layers          = 1
        };
        VkResult vr = vkCreateFramebuffer(g_r.device.handle, &fb_info, NULL,
                                           &g_r.bloom.down_framebuffers[i]);
        if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

        // Upsample framebuffer (only for mips 0 through N-2, mip N-1 is never an upsample target)
        if (i < R_BLOOM_MIP_COUNT - 1) {
            VkFramebufferCreateInfo ufb_info = {
                .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass      = g_r.bloom.additive_render_pass,
                .attachmentCount = 1,
                .pAttachments    = &g_r.bloom.mip_views[i],
                .width           = w,
                .height          = h,
                .layers          = 1
            };
            vr = vkCreateFramebuffer(g_r.device.handle, &ufb_info, NULL,
                                      &g_r.bloom.up_framebuffers[i]);
            if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;
        }

        w /= 2;
        h /= 2;
    }

    /* Allocate descriptor sets for each mip source.
     * mip_descriptors[0] = HDR world target (source for first downsample)
     * mip_descriptors[1..5] = bloom mip 0..4 */
    for (u32 i = 0; i <= R_BLOOM_MIP_COUNT; i++) {
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = g_r.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_r.texture_set_layout
        };
        vkAllocateDescriptorSets(g_r.device.handle, &alloc_info,
                                  &g_r.bloom.mip_descriptors[i]);
    }

    // Write descriptor for HDR world target (index 0)
    {
        VkDescriptorImageInfo img_info = {
            .sampler     = g_r.textures.sampler_linear,
            .imageView   = g_r.world_target.color_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.bloom.mip_descriptors[0],
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &img_info
        };
        vkUpdateDescriptorSets(g_r.device.handle, 1, &write, 0, NULL);
    }

    // Write descriptors for bloom mips (indices 1..5)
    for (u32 i = 0; i < R_BLOOM_MIP_COUNT; i++) {
        VkDescriptorImageInfo img_info = {
            .sampler     = g_r.textures.sampler_linear,
            .imageView   = g_r.bloom.mip_views[i],
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.bloom.mip_descriptors[i + 1],
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &img_info
        };
        vkUpdateDescriptorSets(g_r.device.handle, 1, &write, 0, NULL);
    }

    // Pipelines
    res = create_bloom_pipelines();
    if (res != QK_SUCCESS) return res;

    g_r.bloom.initialized = true;
    return QK_SUCCESS;
}

void r_bloom_shutdown(void)
{
    VkDevice dev = g_r.device.handle;
    if (!dev) return;

    if (g_r.bloom.downsample_pipeline.handle)
        vkDestroyPipeline(dev, g_r.bloom.downsample_pipeline.handle, NULL);
    if (g_r.bloom.downsample_pipeline.layout)
        vkDestroyPipelineLayout(dev, g_r.bloom.downsample_pipeline.layout, NULL);
    if (g_r.bloom.upsample_pipeline.handle)
        vkDestroyPipeline(dev, g_r.bloom.upsample_pipeline.handle, NULL);
    if (g_r.bloom.upsample_pipeline.layout)
        vkDestroyPipelineLayout(dev, g_r.bloom.upsample_pipeline.layout, NULL);

    for (u32 i = 0; i < R_BLOOM_MIP_COUNT; i++) {
        if (g_r.bloom.down_framebuffers[i])
            vkDestroyFramebuffer(dev, g_r.bloom.down_framebuffers[i], NULL);
        if (g_r.bloom.up_framebuffers[i])
            vkDestroyFramebuffer(dev, g_r.bloom.up_framebuffers[i], NULL);
        if (g_r.bloom.mip_views[i])
            vkDestroyImageView(dev, g_r.bloom.mip_views[i], NULL);
        if (g_r.bloom.mip_images[i])
            vkDestroyImage(dev, g_r.bloom.mip_images[i], NULL);
        if (g_r.bloom.mip_memories[i])
            vkFreeMemory(dev, g_r.bloom.mip_memories[i], NULL);
    }

    // Descriptor sets freed with pool

    if (g_r.bloom.render_pass)
        vkDestroyRenderPass(dev, g_r.bloom.render_pass, NULL);
    if (g_r.bloom.additive_render_pass)
        vkDestroyRenderPass(dev, g_r.bloom.additive_render_pass, NULL);

    memset(&g_r.bloom, 0, sizeof(g_r.bloom));
}

void r_bloom_record_commands(VkCommandBuffer cmd)
{
    if (!g_r.bloom.initialized) return;
    if (!g_r.bloom.downsample_pipeline.handle) return;
    if (!g_r.bloom.upsample_pipeline.handle) return;

    r_debug_begin_label(cmd, "Bloom", 1.0f, 0.8f, 0.2f);

    // --- Downsample chain (5 passes) ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      g_r.bloom.downsample_pipeline.handle);

    for (u32 i = 0; i < R_BLOOM_MIP_COUNT; i++) {
        VkExtent2D ext = g_r.bloom.mip_extents[i];

        // Source texel size (from the texture we're reading)
        r_bloom_push_constants_t pc;
        if (i == 0) {
            // Reading from HDR world target
            pc.texel_size[0] = 1.0f / (f32)g_r.config.render_width;
            pc.texel_size[1] = 1.0f / (f32)g_r.config.render_height;
            pc.threshold = 1.0f; // Extract >1.0 brightness on first pass
        } else {
            // Reading from previous mip
            VkExtent2D prev = g_r.bloom.mip_extents[i - 1];
            pc.texel_size[0] = 1.0f / (f32)prev.width;
            pc.texel_size[1] = 1.0f / (f32)prev.height;
            pc.threshold = 0.0f; // No threshold on subsequent passes
        }
        pc.intensity = 1.0f;

        VkRenderPassBeginInfo rp_info = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass      = g_r.bloom.render_pass,
            .framebuffer     = g_r.bloom.down_framebuffers[i],
            .renderArea      = { .offset = { 0, 0 }, .extent = ext },
            .clearValueCount = 0
        };

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {
            .x = 0, .y = 0,
            .width = (f32)ext.width, .height = (f32)ext.height,
            .minDepth = 0.0f, .maxDepth = 1.0f
        };
        VkRect2D scissor = { .offset = { 0, 0 }, .extent = ext };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g_r.bloom.downsample_pipeline.layout,
                                0, 1, &g_r.bloom.mip_descriptors[i], 0, NULL);

        vkCmdPushConstants(cmd, g_r.bloom.downsample_pipeline.layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
    }

    // --- Upsample chain (4 passes, additive) ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      g_r.bloom.upsample_pipeline.handle);

    for (u32 pass = 0; pass < R_BLOOM_MIP_COUNT - 1; pass++) {
        // Read from mip (N-1-pass), write to mip (N-2-pass)
        u32 src_mip = R_BLOOM_MIP_COUNT - 1 - pass;
        u32 dst_mip = src_mip - 1;

        VkExtent2D ext = g_r.bloom.mip_extents[dst_mip];
        VkExtent2D src_ext = g_r.bloom.mip_extents[src_mip];

        r_bloom_push_constants_t pc = {
            .texel_size = { 1.0f / (f32)src_ext.width, 1.0f / (f32)src_ext.height },
            .threshold  = 0.0f,
            .intensity  = 1.0f
        };

        VkRenderPassBeginInfo rp_info = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass      = g_r.bloom.additive_render_pass,
            .framebuffer     = g_r.bloom.up_framebuffers[dst_mip],
            .renderArea      = { .offset = { 0, 0 }, .extent = ext },
            .clearValueCount = 0
        };

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {
            .x = 0, .y = 0,
            .width = (f32)ext.width, .height = (f32)ext.height,
            .minDepth = 0.0f, .maxDepth = 1.0f
        };
        VkRect2D scissor = { .offset = { 0, 0 }, .extent = ext };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Source is mip_descriptors[src_mip + 1] (offset by 1 for HDR target at [0])
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                g_r.bloom.upsample_pipeline.layout,
                                0, 1, &g_r.bloom.mip_descriptors[src_mip + 1], 0, NULL);

        vkCmdPushConstants(cmd, g_r.bloom.upsample_pipeline.layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
    }

    r_debug_end_label(cmd);
}
