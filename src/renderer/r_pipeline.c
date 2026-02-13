/*
 * QUICKEN Renderer - Pipeline Creation, Shader Loading, Pipeline Cache
 */

#include "r_types.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef QK_PLATFORM_LINUX
    #include <sys/stat.h>
#endif

/* ---- Pipeline Cache ---- */

#define PIPELINE_CACHE_FILENAME "quicken_pipeline_cache.bin"

static char s_cache_path[512];

static void r_pipeline_cache_build_path(void)
{
    s_cache_path[0] = '\0';

#ifdef QK_PLATFORM_WINDOWS
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata) {
        snprintf(s_cache_path, sizeof(s_cache_path), "%s\\QUICKEN\\%s",
                 appdata, PIPELINE_CACHE_FILENAME);
        /* Ensure directory exists */
        char dir[512];
        snprintf(dir, sizeof(dir), "%s\\QUICKEN", appdata);
        CreateDirectoryA(dir, NULL);
    }
#else
    const char *cache_home = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (cache_home) {
        snprintf(s_cache_path, sizeof(s_cache_path), "%s/quicken/%s",
                 cache_home, PIPELINE_CACHE_FILENAME);
    } else if (home) {
        snprintf(s_cache_path, sizeof(s_cache_path), "%s/.cache/quicken/%s",
                 home, PIPELINE_CACHE_FILENAME);
    }
    /* Ensure directory exists (best-effort) */
    if (s_cache_path[0]) {
        char dir[512];
        if (cache_home)
            snprintf(dir, sizeof(dir), "%s/quicken", cache_home);
        else
            snprintf(dir, sizeof(dir), "%s/.cache/quicken", home);
        mkdir(dir, 0755);
    }
#endif

    if (!s_cache_path[0]) {
        snprintf(s_cache_path, sizeof(s_cache_path), "%s", PIPELINE_CACHE_FILENAME);
    }
}

qk_result_t r_pipeline_cache_init(void)
{
    r_pipeline_cache_build_path();

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
    };

    /* Try to load existing cache */
    FILE *f = fopen(s_cache_path, "rb");
    void *cache_data = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            cache_data = malloc((size_t)size);
            if (cache_data) {
                if (fread(cache_data, 1, (size_t)size, f) == (size_t)size) {
                    cache_info.initialDataSize = (size_t)size;
                    cache_info.pInitialData = cache_data;
                }
            }
        }
        fclose(f);
    }

    VkResult vr = vkCreatePipelineCache(g_r.device.handle, &cache_info, NULL,
                                         &g_r.pipeline_cache_handle);
    free(cache_data);

    if (vr != VK_SUCCESS) {
        /* Try without initial data */
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = NULL;
        vr = vkCreatePipelineCache(g_r.device.handle, &cache_info, NULL,
                                    &g_r.pipeline_cache_handle);
        if (vr != VK_SUCCESS) return QK_ERROR_PIPELINE;
    }

    return QK_SUCCESS;
}

void r_pipeline_cache_save(void)
{
    if (!g_r.pipeline_cache_handle) return;

    size_t size = 0;
    vkGetPipelineCacheData(g_r.device.handle, g_r.pipeline_cache_handle, &size, NULL);
    if (size == 0) return;

    void *data = malloc(size);
    if (!data) return;

    if (vkGetPipelineCacheData(g_r.device.handle, g_r.pipeline_cache_handle, &size, data) == VK_SUCCESS) {
        FILE *f = fopen(s_cache_path, "wb");
        if (f) {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }

    free(data);
}

void r_pipeline_cache_shutdown(void)
{
    r_pipeline_cache_save();
    if (g_r.pipeline_cache_handle) {
        vkDestroyPipelineCache(g_r.device.handle, g_r.pipeline_cache_handle, NULL);
        g_r.pipeline_cache_handle = VK_NULL_HANDLE;
    }
}

/* ---- Shader Module Loading ---- */

VkShaderModule r_pipeline_load_shader(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Renderer] Failed to open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return VK_NULL_HANDLE;
    }

    /* SPIR-V requires 4-byte alignment */
    u32 *code = malloc((size_t)size);
    if (!code) {
        fclose(f);
        return VK_NULL_HANDLE;
    }

    if (fread(code, 1, (size_t)size, f) != (size_t)size) {
        free(code);
        fclose(f);
        return VK_NULL_HANDLE;
    }
    fclose(f);

    VkShaderModuleCreateInfo module_info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size,
        .pCode    = code
    };

    VkShaderModule module;
    VkResult vr = vkCreateShaderModule(g_r.device.handle, &module_info, NULL, &module);
    free(code);

    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Renderer] Failed to create shader module: %s\n", path);
        return VK_NULL_HANDLE;
    }

    return module;
}

/* ---- World Pipeline ---- */

qk_result_t r_pipeline_create_world(void)
{
    VkShaderModule vert = r_pipeline_load_shader("src/renderer/shaders/compiled/world.vert.spv");
    VkShaderModule frag = r_pipeline_load_shader("src/renderer/shaders/compiled/world.frag.spv");

    if (!vert || !frag) {
        fprintf(stderr, "[Renderer] World shader compilation required. Using stub pipeline.\n");
        if (vert) vkDestroyShaderModule(g_r.device.handle, vert, NULL);
        if (frag) vkDestroyShaderModule(g_r.device.handle, frag, NULL);
        /* Pipeline is optional at this stage; draw_world will skip if no pipeline */
        return QK_SUCCESS;
    }

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
            .module = frag,
            .pName  = "main"
        }
    };

    /* Vertex input */
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
          .offset = offsetof(r_world_vertex_t, uv) }
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions    = attrs
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
        .cullMode    = VK_CULL_MODE_BACK_BIT,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp   = VK_COMPARE_OP_LESS
    };

    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_attachment
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states
    };

    /* Pipeline layout: set 0 = view UBO, set 1 = texture */
    VkDescriptorSetLayout set_layouts[] = { g_r.view_set_layout, g_r.texture_set_layout };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts    = set_layouts
    };

    vkCreatePipelineLayout(g_r.device.handle, &layout_info, NULL, &g_r.world_pipeline.layout);

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
        .layout              = g_r.world_pipeline.layout,
        .renderPass          = g_r.world_target.render_pass,
        .subpass             = 0
    };

    VkResult vr = vkCreateGraphicsPipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                             1, &pipeline_info, NULL,
                                             &g_r.world_pipeline.handle);

    vkDestroyShaderModule(g_r.device.handle, vert, NULL);
    vkDestroyShaderModule(g_r.device.handle, frag, NULL);

    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Renderer] Failed to create world pipeline\n");
        return QK_ERROR_PIPELINE;
    }

    return QK_SUCCESS;
}

/* ---- UI Pipeline ---- */

qk_result_t r_pipeline_create_ui(void)
{
    VkShaderModule vert = r_pipeline_load_shader("src/renderer/shaders/compiled/ui.vert.spv");
    VkShaderModule frag = r_pipeline_load_shader("src/renderer/shaders/compiled/ui.frag.spv");

    if (!vert || !frag) {
        fprintf(stderr, "[Renderer] UI shader compilation required. Using stub pipeline.\n");
        if (vert) vkDestroyShaderModule(g_r.device.handle, vert, NULL);
        if (frag) vkDestroyShaderModule(g_r.device.handle, frag, NULL);
        return QK_SUCCESS;
    }

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
            .module = frag,
            .pName  = "main"
        }
    };

    VkVertexInputBindingDescription binding = {
        .binding   = 0,
        .stride    = sizeof(r_ui_vertex_t),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };

    VkVertexInputAttributeDescription attrs[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(r_ui_vertex_t, position) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(r_ui_vertex_t, uv) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32_UINT,
          .offset = offsetof(r_ui_vertex_t, color) }
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions    = attrs
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

    /* Alpha blending (pre-multiplied) */
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_attachment
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states
    };

    /* Push constants for screen size */
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(f32) * 2
    };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &g_r.texture_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range
    };

    vkCreatePipelineLayout(g_r.device.handle, &layout_info, NULL, &g_r.ui_pipeline.layout);

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
        .layout              = g_r.ui_pipeline.layout,
        .renderPass          = g_r.ui_target.render_pass,
        .subpass             = 0
    };

    VkResult vr = vkCreateGraphicsPipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                             1, &pipeline_info, NULL,
                                             &g_r.ui_pipeline.handle);

    vkDestroyShaderModule(g_r.device.handle, vert, NULL);
    vkDestroyShaderModule(g_r.device.handle, frag, NULL);

    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Renderer] Failed to create UI pipeline\n");
        return QK_ERROR_PIPELINE;
    }

    return QK_SUCCESS;
}

/* ---- Composition Pipeline ---- */

qk_result_t r_pipeline_create_compose(void)
{
    VkShaderModule vert = r_pipeline_load_shader("src/renderer/shaders/compiled/compose.vert.spv");
    VkShaderModule frag = r_pipeline_load_shader("src/renderer/shaders/compiled/compose.frag.spv");

    if (!vert || !frag) {
        fprintf(stderr, "[Renderer] Compose shader compilation required. Using stub pipeline.\n");
        if (vert) vkDestroyShaderModule(g_r.device.handle, vert, NULL);
        if (frag) vkDestroyShaderModule(g_r.device.handle, frag, NULL);
        return QK_SUCCESS;
    }

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
            .module = frag,
            .pName  = "main"
        }
    };

    /* No vertex input (fullscreen triangle generated in shader) */
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

    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_attachment
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states
    };

    /* Push constants for viewport params */
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(r_compose_push_constants_t)
    };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &g_r.compose_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range
    };

    vkCreatePipelineLayout(g_r.device.handle, &layout_info, NULL, &g_r.compose_pipeline.layout);

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
        .layout              = g_r.compose_pipeline.layout,
        .renderPass          = g_r.compose_render_pass,
        .subpass             = 0
    };

    VkResult vr = vkCreateGraphicsPipelines(g_r.device.handle, g_r.pipeline_cache_handle,
                                             1, &pipeline_info, NULL,
                                             &g_r.compose_pipeline.handle);

    vkDestroyShaderModule(g_r.device.handle, vert, NULL);
    vkDestroyShaderModule(g_r.device.handle, frag, NULL);

    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Renderer] Failed to create compose pipeline\n");
        return QK_ERROR_PIPELINE;
    }

    return QK_SUCCESS;
}

/* ---- Cleanup ---- */

void r_pipeline_destroy_all(void)
{
    VkDevice dev = g_r.device.handle;

    if (g_r.world_pipeline.handle) vkDestroyPipeline(dev, g_r.world_pipeline.handle, NULL);
    if (g_r.world_pipeline.layout) vkDestroyPipelineLayout(dev, g_r.world_pipeline.layout, NULL);
    memset(&g_r.world_pipeline, 0, sizeof(g_r.world_pipeline));

    if (g_r.ui_pipeline.handle) vkDestroyPipeline(dev, g_r.ui_pipeline.handle, NULL);
    if (g_r.ui_pipeline.layout) vkDestroyPipelineLayout(dev, g_r.ui_pipeline.layout, NULL);
    memset(&g_r.ui_pipeline, 0, sizeof(g_r.ui_pipeline));

    if (g_r.compose_pipeline.handle) vkDestroyPipeline(dev, g_r.compose_pipeline.handle, NULL);
    if (g_r.compose_pipeline.layout) vkDestroyPipelineLayout(dev, g_r.compose_pipeline.layout, NULL);
    memset(&g_r.compose_pipeline, 0, sizeof(g_r.compose_pipeline));
}
