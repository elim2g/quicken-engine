/*
 * QUICKEN Renderer - Forward+ Compute (Light Culling, SSBOs, Depth Pre-Pass)
 *
 * Per-tile light culling via compute shader. SSBOs for light data and
 * per-tile light index lists. Depth pre-pass to feed the cull shader.
 */

#include "r_types.h"
#include "renderer/qk_renderer.h"
#include <string.h>
#include <stdio.h>

// --- Light submission (public API) ---

void qk_renderer_submit_light(const qk_dynamic_light_t *light)
{
    if (!light) return;
    if (g_r.lights.light_count >= R_MAX_DYNAMIC_LIGHTS) return;

    r_dynamic_light_t *dst = &g_r.lights.lights[g_r.lights.light_count++];
    memcpy(dst->position, light->position, sizeof(f32) * 3);
    dst->radius    = light->radius;
    memcpy(dst->color, light->color, sizeof(f32) * 3);
    dst->intensity = light->intensity;
}

// --- Depth Pre-Pass ---

static qk_result_t depth_prepass_init(void)
{
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    // Try D32 first, fall back to D24
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(g_r.device.physical, depth_format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    }

    // Depth-only render pass: store depth so compute can sample it
    VkAttachmentDescription attachment = {
        .format         = depth_format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    };

    VkAttachmentReference depth_ref = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 0,
        .pDepthStencilAttachment = &depth_ref
    };

    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
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
                                      &g_r.depth_prepass.render_pass);
    if (vr != VK_SUCCESS) return QK_ERROR_PIPELINE;

    // Framebuffer using the world target's depth image
    VkFramebufferCreateInfo fb_info = {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = g_r.depth_prepass.render_pass,
        .attachmentCount = 1,
        .pAttachments    = &g_r.world_target.depth_view,
        .width           = g_r.config.render_width,
        .height          = g_r.config.render_height,
        .layers          = 1
    };
    vr = vkCreateFramebuffer(g_r.device.handle, &fb_info, NULL,
                              &g_r.depth_prepass.framebuffer);
    if (vr != VK_SUCCESS) return QK_ERROR_OUT_OF_MEMORY;

    // Depth pre-pass pipeline: same vertex input as world, no fragment shader
    VkShaderModule vert = r_pipeline_load_shader("src/renderer/shaders/compiled/world.vert.spv");
    if (!vert) return QK_ERROR_PIPELINE;

    VkPipelineShaderStageCreateInfo stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vert,
        .pName  = "main"
    };

    VkVertexInputBindingDescription binding = {
        .binding   = 0,
        .stride    = sizeof(r_world_vertex_t),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription attrs[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
          .offset = offsetof(r_world_vertex_t, position) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
          .offset = offsetof(r_world_vertex_t, normal) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(r_world_vertex_t, uv) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(r_world_vertex_t, lm_uv) },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT,
          .offset = offsetof(r_world_vertex_t, texture_id) }
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = 5,
        .pVertexAttributeDescriptions    = attrs
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1
    };
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_BACK_BIT,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp   = VK_COMPARE_OP_LESS
    };
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 0
    };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states
    };

    // Layout: only needs view UBO (set 0) for the VP matrix
    VkPipelineLayoutCreateInfo layout_info = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &g_r.view_set_layout
    };
    vkCreatePipelineLayout(g_r.device.handle, &layout_info, NULL,
                           &g_r.depth_prepass.pipeline.layout);

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 1,
        .pStages             = &stage,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState   = &multisample,
        .pDepthStencilState  = &depth_stencil,
        .pColorBlendState    = &color_blend,
        .pDynamicState       = &dynamic_state,
        .layout              = g_r.depth_prepass.pipeline.layout,
        .renderPass          = g_r.depth_prepass.render_pass,
        .subpass             = 0
    };

    vr = vkCreateGraphicsPipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                    1, &pipeline_info, NULL,
                                    &g_r.depth_prepass.pipeline.handle);
    vkDestroyShaderModule(g_r.device.handle, vert, NULL);

    if (vr != VK_SUCCESS) return QK_ERROR_PIPELINE;

    g_r.depth_prepass.initialized = true;
    return QK_SUCCESS;
}

// --- SSBO Setup ---

static qk_result_t ssbo_init(void)
{
    // Light SSBO (host-visible, persistently mapped)
    // Header: 4 uints (count + 3 padding) + lights array
    VkDeviceSize light_size = 16 + R_MAX_DYNAMIC_LIGHTS * sizeof(r_dynamic_light_t);
    qk_result_t res = r_memory_create_buffer(
        light_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &g_r.lights.light_ssbo, &g_r.lights.light_memory);
    if (res != QK_SUCCESS) return res;

    vkMapMemory(g_r.device.handle, g_r.lights.light_memory, 0, light_size, 0,
                &g_r.lights.light_mapped);

    // Tile SSBO (device-local)
    u32 tile_x = (g_r.config.render_width + R_TILE_SIZE - 1) / R_TILE_SIZE;
    u32 tile_y = (g_r.config.render_height + R_TILE_SIZE - 1) / R_TILE_SIZE;
    g_r.lights.tile_count_x = tile_x;
    g_r.lights.tile_count_y = tile_y;

    VkDeviceSize tile_size = (VkDeviceSize)tile_x * tile_y * (R_MAX_LIGHTS_PER_TILE + 1) * sizeof(u32);
    res = r_memory_create_buffer(
        tile_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_r.lights.tile_ssbo, &g_r.lights.tile_memory);
    if (res != QK_SUCCESS) return res;

    return QK_SUCCESS;
}

// --- Compute Pipeline ---

static qk_result_t compute_pipeline_init(void)
{
    /* Descriptor set layout for compute:
     * binding 0: combined image sampler (depth texture)
     * binding 1: storage buffer (light buffer, readonly)
     * binding 2: storage buffer (tile buffer, writeonly) */
    VkDescriptorSetLayoutBinding bindings[3] = {
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding         = 2,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings    = bindings
    };
    vkCreateDescriptorSetLayout(g_r.device.handle, &layout_info, NULL,
                                 &g_r.lights.cull_set_layout);

    /* Light SSBO layout for fragment shaders (set 2 for world, set 1 for entity):
     * binding 0: storage buffer (light buffer, readonly)
     * binding 1: storage buffer (tile buffer, readonly) */
    VkDescriptorSetLayoutBinding light_bindings[2] = {
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
        },
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo light_layout_info = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = light_bindings
    };
    vkCreateDescriptorSetLayout(g_r.device.handle, &light_layout_info, NULL,
                                 &g_r.lights.light_set_layout);

    // Allocate compute descriptor set
    {
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = g_r.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_r.lights.cull_set_layout
        };
        vkAllocateDescriptorSets(g_r.device.handle, &alloc_info,
                                  &g_r.lights.cull_descriptor_set);
    }

    // Allocate fragment light descriptor set
    {
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = g_r.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &g_r.lights.light_set_layout
        };
        vkAllocateDescriptorSets(g_r.device.handle, &alloc_info,
                                  &g_r.lights.light_descriptor_set);
    }

    // Write compute descriptor set
    VkDeviceSize light_size = 16 + R_MAX_DYNAMIC_LIGHTS * sizeof(r_dynamic_light_t);
    VkDeviceSize tile_size = (VkDeviceSize)g_r.lights.tile_count_x * g_r.lights.tile_count_y *
                             (R_MAX_LIGHTS_PER_TILE + 1) * sizeof(u32);

    VkDescriptorImageInfo depth_info = {
        .sampler     = g_r.textures.sampler_nearest,
        .imageView   = g_r.world_target.depth_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    };
    VkDescriptorBufferInfo light_buf_info = {
        .buffer = g_r.lights.light_ssbo,
        .offset = 0,
        .range  = light_size
    };
    VkDescriptorBufferInfo tile_buf_info = {
        .buffer = g_r.lights.tile_ssbo,
        .offset = 0,
        .range  = tile_size
    };

    VkWriteDescriptorSet writes[3] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.lights.cull_descriptor_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &depth_info
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.lights.cull_descriptor_set,
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &light_buf_info
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.lights.cull_descriptor_set,
            .dstBinding      = 2,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &tile_buf_info
        }
    };
    vkUpdateDescriptorSets(g_r.device.handle, 3, writes, 0, NULL);

    // Write fragment light descriptor set
    VkWriteDescriptorSet frag_writes[2] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.lights.light_descriptor_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &light_buf_info
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = g_r.lights.light_descriptor_set,
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &tile_buf_info
        }
    };
    vkUpdateDescriptorSets(g_r.device.handle, 2, frag_writes, 0, NULL);

    // Compute pipeline layout
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(r_light_cull_push_constants_t)
    };
    VkPipelineLayoutCreateInfo pipe_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &g_r.lights.cull_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range
    };
    vkCreatePipelineLayout(g_r.device.handle, &pipe_layout_info, NULL,
                           &g_r.lights.cull_layout);

    // Compute pipeline
    VkShaderModule comp = r_pipeline_load_shader("src/renderer/shaders/compiled/light_cull.comp.spv");
    if (!comp) return QK_ERROR_PIPELINE;

    VkComputePipelineCreateInfo pipeline_info = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp,
            .pName  = "main"
        },
        .layout = g_r.lights.cull_layout
    };

    VkResult vr = vkCreateComputePipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                            1, &pipeline_info, NULL,
                                            &g_r.lights.cull_pipeline);
    vkDestroyShaderModule(g_r.device.handle, comp, NULL);

    if (vr != VK_SUCCESS) return QK_ERROR_PIPELINE;

    return QK_SUCCESS;
}

// --- Public Functions ---

qk_result_t r_compute_init(void)
{
    memset(&g_r.lights, 0, sizeof(g_r.lights));
    memset(&g_r.depth_prepass, 0, sizeof(g_r.depth_prepass));

    qk_result_t res = depth_prepass_init();
    if (res != QK_SUCCESS) return res;

    res = ssbo_init();
    if (res != QK_SUCCESS) return res;

    res = compute_pipeline_init();
    if (res != QK_SUCCESS) return res;

    g_r.lights.initialized = true;
    return QK_SUCCESS;
}

void r_compute_shutdown(void)
{
    VkDevice dev = g_r.device.handle;
    if (!dev) return;

    // Depth pre-pass
    if (g_r.depth_prepass.pipeline.handle)
        vkDestroyPipeline(dev, g_r.depth_prepass.pipeline.handle, NULL);
    if (g_r.depth_prepass.pipeline.layout)
        vkDestroyPipelineLayout(dev, g_r.depth_prepass.pipeline.layout, NULL);
    if (g_r.depth_prepass.framebuffer)
        vkDestroyFramebuffer(dev, g_r.depth_prepass.framebuffer, NULL);
    if (g_r.depth_prepass.render_pass)
        vkDestroyRenderPass(dev, g_r.depth_prepass.render_pass, NULL);
    memset(&g_r.depth_prepass, 0, sizeof(g_r.depth_prepass));

    // Compute pipeline
    if (g_r.lights.cull_pipeline)
        vkDestroyPipeline(dev, g_r.lights.cull_pipeline, NULL);
    if (g_r.lights.cull_layout)
        vkDestroyPipelineLayout(dev, g_r.lights.cull_layout, NULL);

    // Descriptor layouts (sets freed with pool)
    if (g_r.lights.cull_set_layout)
        vkDestroyDescriptorSetLayout(dev, g_r.lights.cull_set_layout, NULL);
    if (g_r.lights.light_set_layout)
        vkDestroyDescriptorSetLayout(dev, g_r.lights.light_set_layout, NULL);

    // SSBOs
    if (g_r.lights.light_ssbo) vkDestroyBuffer(dev, g_r.lights.light_ssbo, NULL);
    if (g_r.lights.light_memory) {
        vkUnmapMemory(dev, g_r.lights.light_memory);
        vkFreeMemory(dev, g_r.lights.light_memory, NULL);
    }
    if (g_r.lights.tile_ssbo) vkDestroyBuffer(dev, g_r.lights.tile_ssbo, NULL);
    if (g_r.lights.tile_memory) vkFreeMemory(dev, g_r.lights.tile_memory, NULL);

    memset(&g_r.lights, 0, sizeof(g_r.lights));
}

void r_compute_upload_lights(void)
{
    if (!g_r.lights.initialized || !g_r.lights.light_mapped) return;

    // Write header: [count, pad, pad, pad] then light array
    u32 *header = (u32 *)g_r.lights.light_mapped;
    header[0] = g_r.lights.light_count;
    header[1] = 0;
    header[2] = 0;
    header[3] = 0;

    if (g_r.lights.light_count > 0) {
        r_dynamic_light_t *gpu_lights = (r_dynamic_light_t *)(header + 4);
        memcpy(gpu_lights, g_r.lights.lights,
               g_r.lights.light_count * sizeof(r_dynamic_light_t));
    }
}

void r_depth_prepass_record(VkCommandBuffer cmd, u32 frame_index)
{
    if (!g_r.depth_prepass.initialized) return;
    if (!g_r.depth_prepass.pipeline.handle) return;
    if (g_r.world.vertex_count == 0) return;

    r_debug_begin_label(cmd, "Depth Pre-Pass", 0.5f, 0.5f, 0.5f);

    r_frame_data_t *frame = &g_r.frames[frame_index];

    VkClearValue clear = { .depthStencil = { 1.0f, 0 } };
    VkRenderPassBeginInfo rp_info = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass      = g_r.depth_prepass.render_pass,
        .framebuffer     = g_r.depth_prepass.framebuffer,
        .renderArea      = { .offset = { 0, 0 },
                             .extent = { g_r.config.render_width, g_r.config.render_height } },
        .clearValueCount = 1,
        .pClearValues    = &clear
    };

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      g_r.depth_prepass.pipeline.handle);

    VkViewport viewport = {
        .x = 0, .y = 0,
        .width  = (f32)g_r.config.render_width,
        .height = (f32)g_r.config.render_height,
        .minDepth = 0.0f, .maxDepth = 1.0f
    };
    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = { g_r.config.render_width, g_r.config.render_height }
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.depth_prepass.pipeline.layout,
                            0, 1, &frame->view_descriptor_set, 0, NULL);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_r.world.vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, g_r.world.index_buffer, 0, VK_INDEX_TYPE_UINT32);

    for (u32 i = 0; i < g_r.world.surface_count; i++) {
        r_draw_surface_t *surf = &g_r.world.surfaces[i];
        vkCmdDrawIndexed(cmd, surf->index_count, 1,
                         surf->index_offset, (i32)surf->vertex_offset, 0);
    }

    vkCmdEndRenderPass(cmd);

    r_debug_end_label(cmd);
}

void r_compute_record_cull(VkCommandBuffer cmd, const f32 *inv_projection)
{
    if (!g_r.lights.initialized) return;
    if (!g_r.lights.cull_pipeline) return;

    r_debug_begin_label(cmd, "Light Cull", 0.8f, 0.8f, 0.2f);

    // Barrier: depth attachment -> shader read
    VkImageMemoryBarrier depth_barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = g_r.world_target.depth_image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        }
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &depth_barrier);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_r.lights.cull_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            g_r.lights.cull_layout,
                            0, 1, &g_r.lights.cull_descriptor_set, 0, NULL);

    r_light_cull_push_constants_t pc;
    memcpy(pc.inv_projection, inv_projection, sizeof(f32) * 16);
    pc.screen_width     = g_r.config.render_width;
    pc.screen_height    = g_r.config.render_height;
    pc.tile_count_x     = g_r.lights.tile_count_x;
    pc.light_count      = g_r.lights.light_count;

    vkCmdPushConstants(cmd, g_r.lights.cull_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    vkCmdDispatch(cmd, g_r.lights.tile_count_x, g_r.lights.tile_count_y, 1);

    // Barrier: tile SSBO write -> fragment shader read
    VkBufferMemoryBarrier tile_barrier = {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = g_r.lights.tile_ssbo,
        .offset              = 0,
        .size                = VK_WHOLE_SIZE
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 1, &tile_barrier, 0, NULL);

    r_debug_end_label(cmd);
}
