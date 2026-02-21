/*
 * QUICKEN Renderer - Public API Implementation
 *
 * Orchestrates the Vulkan renderer lifecycle and per-frame rendering.
 * Two-pass architecture: world+UI (offscreen) -> compose (swapchain).
 *
 * Note: qk_ui_draw_* functions are NOT here -- they are in src/ui/ui_draw.c
 * because they compile as part of the main exe, not the renderer static lib.
 */

#include "renderer/qk_renderer.h"
#include "r_types.h"
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Lifecycle ---

qk_result_t qk_renderer_init(const qk_renderer_config_t *config)
{
    memset(&g_r, 0, sizeof(g_r));

    g_r.config.render_width  = config->render_width  ? config->render_width  : 1920;
    g_r.config.render_height = config->render_height ? config->render_height : 1080;
    g_r.config.window_width  = config->window_width;
    g_r.config.window_height = config->window_height;
    g_r.config.aspect_fit    = config->aspect_fit;
    g_r.config.vsync         = config->vsync;
    g_r.ambient              = 0.0125f;

    qk_result_t res;

    // Vulkan instance
    res = r_vulkan_create_instance();
    if (res != QK_SUCCESS) return res;

    // Surface (from SDL window)
    res = r_vulkan_create_surface(config->sdl_window);
    if (res != QK_SUCCESS) return res;

    // Physical device
    res = r_vulkan_pick_physical_device();
    if (res != QK_SUCCESS) return res;

    // Logical device
    res = r_vulkan_create_device();
    if (res != QK_SUCCESS) return res;

    // Memory allocator
    res = r_memory_init();
    if (res != QK_SUCCESS) return res;

    // Staging buffer
    res = r_staging_init();
    if (res != QK_SUCCESS) return res;

    // Swapchain
    res = r_vulkan_create_swapchain();
    if (res != QK_SUCCESS) return res;

    // Descriptor layouts and pool
    res = r_descriptors_init();
    if (res != QK_SUCCESS) return res;

    // Offscreen render targets
    res = r_create_render_targets();
    if (res != QK_SUCCESS) return res;

    // Command pools, sync objects, per-frame buffers
    res = r_commands_init();
    if (res != QK_SUCCESS) return res;

    // Textures (creates default white texture)
    res = r_texture_init();
    if (res != QK_SUCCESS) return res;

    // Pipeline cache
    res = r_pipeline_cache_init();
    if (res != QK_SUCCESS) return res;

    /* Forward+ compute (SSBOs, depth pre-pass, light culling).
     * MUST be before pipeline creation: world and entity pipeline layouts
     * reference g_r.lights.light_set_layout created here. */
    res = r_compute_init();
    if (res != QK_SUCCESS) return res;

    // Pipelines
    res = r_pipeline_create_world();
    if (res != QK_SUCCESS) return res;

    res = r_pipeline_create_entity();
    if (res != QK_SUCCESS) return res;

    res = r_pipeline_create_fx();
    if (res != QK_SUCCESS) return res;

    res = r_pipeline_create_ui();
    if (res != QK_SUCCESS) return res;

    res = r_pipeline_create_compose();
    if (res != QK_SUCCESS) return res;

    res = r_pipeline_create_overlay_ui();
    if (res != QK_SUCCESS) return res;

    // UI index buffer
    res = r_ui_init();
    if (res != QK_SUCCESS) return res;

    // Composition pass descriptor setup
    res = r_compose_init();
    if (res != QK_SUCCESS) return res;

    // World geometry
    r_world_init();

    // Entity meshes (capsule + sphere)
    res = r_entity_init();
    if (res != QK_SUCCESS) return res;

    // FX effect buffers
    res = r_fx_init();
    if (res != QK_SUCCESS) return res;

    // Bloom mip chain
    res = r_bloom_init();
    if (res != QK_SUCCESS) return res;

    // Debug timers
    r_debug_init();

    g_r.initialized = true;
    fprintf(stderr, "[Renderer] Initialized (%ux%u render, %ux%u window)\n",
            g_r.config.render_width, g_r.config.render_height,
            g_r.config.window_width, g_r.config.window_height);

    return QK_SUCCESS;
}

void qk_renderer_shutdown(void)
{
    if (!g_r.initialized) return;

    vkDeviceWaitIdle(g_r.device.handle);

    r_debug_shutdown();
    r_bloom_shutdown();
    r_compute_shutdown();
    r_fx_shutdown();
    r_entity_shutdown();
    r_world_shutdown();
    r_compose_shutdown();
    r_ui_shutdown();
    r_pipeline_cache_shutdown();
    r_pipeline_destroy_all();
    r_texture_shutdown();
    r_commands_shutdown();
    r_destroy_render_targets();
    r_descriptors_shutdown();
    r_vulkan_destroy_swapchain();
    r_staging_shutdown();
    r_memory_shutdown();

    if (g_r.swapchain.surface) {
        vkDestroySurfaceKHR(g_r.instance.handle, g_r.swapchain.surface, NULL);
    }

#ifdef QUICKEN_DEBUG
    if (g_r.instance.debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT func =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                g_r.instance.handle, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(g_r.instance.handle, g_r.instance.debug_messenger, NULL);
        }
    }
#endif

    if (g_r.device.handle) {
        vkDestroyDevice(g_r.device.handle, NULL);
    }
    if (g_r.instance.handle) {
        vkDestroyInstance(g_r.instance.handle, NULL);
    }

    memset(&g_r, 0, sizeof(g_r));
    fprintf(stderr, "[Renderer] Shutdown complete\n");
}

// --- Resolution ---

void qk_renderer_set_render_resolution(u32 width, u32 height)
{
    if (!g_r.initialized) return;
    if (width == g_r.config.render_width && height == g_r.config.render_height) return;

    vkDeviceWaitIdle(g_r.device.handle);

    g_r.config.render_width = width;
    g_r.config.render_height = height;

    r_bloom_shutdown();
    r_compute_shutdown();
    r_destroy_render_targets();
    r_create_render_targets();

    // Forward+ compute (needs render targets for depth pre-pass)
    r_compute_init();

    // Re-create pipelines that reference render targets
    r_pipeline_destroy_all();
    r_pipeline_create_world();
    r_pipeline_create_entity();
    r_pipeline_create_fx();
    r_pipeline_create_ui();
    r_pipeline_create_compose();
    r_pipeline_create_overlay_ui();

    r_bloom_init();
    r_compose_update_descriptors();
}

void qk_renderer_get_render_resolution(u32 *out_width, u32 *out_height)
{
    if (out_width)  *out_width  = g_r.config.render_width;
    if (out_height) *out_height = g_r.config.render_height;
}

void qk_renderer_set_aspect_mode(bool aspect_fit)
{
    g_r.config.aspect_fit = aspect_fit;
}

void qk_renderer_set_ambient(f32 ambient)
{
    g_r.ambient = ambient;
}

void qk_renderer_set_vsync(bool vsync)
{
    if (!g_r.initialized) return;
    if (vsync == g_r.config.vsync) return;

    g_r.config.vsync = vsync;

    // Defer recreation to begin_frame where synchronization is correct
    g_r.swapchain_needs_recreate = true;
}

void qk_renderer_handle_window_resize(u32 new_width, u32 new_height)
{
    if (!g_r.initialized) return;
    if (new_width == g_r.config.window_width && new_height == g_r.config.window_height) return;
    g_r.config.window_width = new_width;
    g_r.config.window_height = new_height;
    g_r.swapchain_needs_recreate = true;
}

// --- Resource Upload ---

qk_result_t qk_renderer_upload_world(
    const qk_world_vertex_t *vertices, u32 vertex_count,
    const u32 *indices, u32 index_count,
    const qk_draw_surface_t *surfaces, u32 surface_count)
{
    if (!g_r.initialized) return QK_ERROR_INIT_FAILED;

    r_world_shutdown();
    r_staging_reset();

    // Upload vertices via staging buffer
    VkDeviceSize vb_size = vertex_count * sizeof(r_world_vertex_t);
    VkDeviceSize staging_offset;
    void *staging_ptr = r_staging_alloc(vb_size, &staging_offset);
    if (!staging_ptr) return QK_ERROR_OUT_OF_MEMORY;
    memcpy(staging_ptr, vertices, (size_t)vb_size);

    qk_result_t res = r_memory_create_buffer(
        vb_size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_r.world.vertex_buffer, &g_r.world.vertex_memory);
    if (res != QK_SUCCESS) return res;

    VkCommandBuffer cmd = r_commands_begin_single();
    VkBufferCopy vb_copy = { .srcOffset = staging_offset, .size = vb_size };
    vkCmdCopyBuffer(cmd, g_r.staging.buffer, g_r.world.vertex_buffer, 1, &vb_copy);
    r_commands_end_single(cmd);

    // Upload indices
    VkDeviceSize ib_size = index_count * sizeof(u32);
    staging_ptr = r_staging_alloc(ib_size, &staging_offset);
    if (!staging_ptr) return QK_ERROR_OUT_OF_MEMORY;
    memcpy(staging_ptr, indices, (size_t)ib_size);

    res = r_memory_create_buffer(
        ib_size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_r.world.index_buffer, &g_r.world.index_memory);
    if (res != QK_SUCCESS) return res;

    cmd = r_commands_begin_single();
    VkBufferCopy ib_copy = { .srcOffset = staging_offset, .size = ib_size };
    vkCmdCopyBuffer(cmd, g_r.staging.buffer, g_r.world.index_buffer, 1, &ib_copy);
    r_commands_end_single(cmd);

    g_r.world.vertex_count = vertex_count;
    g_r.world.index_count = index_count;
    g_r.world.surface_count = surface_count;

    /* Copy surface descriptors, filtering out translucent non-solid surfaces
       (light ray volumes, fog brushes, spotlight beams) */
    g_r.world.surfaces = malloc(surface_count * sizeof(r_draw_surface_t));
    if (!g_r.world.surfaces) return QK_ERROR_OUT_OF_MEMORY;

    u32 kept = 0;
    for (u32 i = 0; i < surface_count; i++) {
        u32 contents = surfaces[i].contents_flags;
        if ((contents & 0x20000000u) && !(contents & 0x1u)) continue;

        g_r.world.surfaces[kept].index_offset  = surfaces[i].index_offset;
        g_r.world.surfaces[kept].index_count   = surfaces[i].index_count;
        g_r.world.surfaces[kept].vertex_offset = surfaces[i].vertex_offset;
        g_r.world.surfaces[kept].texture_index = surfaces[i].texture_index;
        kept++;
    }
    g_r.world.surface_count = kept;

    // Sort surfaces by texture for batching
    for (u32 i = 1; i < kept; i++) {
        r_draw_surface_t key = g_r.world.surfaces[i];
        u32 j = i;
        while (j > 0 && g_r.world.surfaces[j - 1].texture_index > key.texture_index) {
            g_r.world.surfaces[j] = g_r.world.surfaces[j - 1];
            j--;
        }
        g_r.world.surfaces[j] = key;
    }

    return QK_SUCCESS;
}

qk_texture_id_t qk_renderer_upload_texture(
    const u8 *pixels, u32 width, u32 height, u32 channels, bool nearest)
{
    if (!g_r.initialized) return 0;
    r_staging_reset();
    return r_texture_upload(pixels, width, height, channels, nearest);
}

qk_result_t qk_renderer_upload_lightmap_atlas(const u8 *pixels, u32 w, u32 h)
{
    if (!g_r.initialized || !pixels || w == 0 || h == 0)
        return QK_ERROR_INVALID_PARAM;

    r_staging_reset();
    u32 tex_id = r_texture_upload(pixels, w, h, 4, false);
    if (tex_id == 0) return QK_ERROR_OUT_OF_MEMORY;

    g_r.lightmap_texture_id = tex_id;
    g_r.has_lightmaps = true;
    return QK_SUCCESS;
}

void qk_renderer_free_world(void)
{
    if (!g_r.initialized) return;
    vkDeviceWaitIdle(g_r.device.handle);
    r_world_shutdown();
}

// --- Matrix inverse (for light culling) ---

static void mat4_inverse(const f32 *m, f32 *out)
{
    f32 inv[16];
    inv[ 0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[ 4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[ 8] =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[ 9];
    inv[12] = -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[ 9];
    inv[ 1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[ 5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[ 9] = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[ 9];
    inv[13] =  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[ 9];
    inv[ 2] =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[ 6];
    inv[ 6] = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[ 6];
    inv[10] =  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[ 5];
    inv[14] = -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[ 5];
    inv[ 3] = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[ 9]*m[2]*m[ 7] + m[ 9]*m[3]*m[ 6];
    inv[ 7] =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[ 8]*m[2]*m[ 7] - m[ 8]*m[3]*m[ 6];
    inv[11] = -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[ 9] - m[ 8]*m[1]*m[ 7] + m[ 8]*m[3]*m[ 5];
    inv[15] =  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[ 9] + m[ 8]*m[1]*m[ 6] - m[ 8]*m[2]*m[ 5];

    f32 det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) {
        memset(out, 0, sizeof(f32) * 16);
        return;
    }
    f32 inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * inv_det;
}

// --- Frame Rendering ---

static u64 s_start_ticks = 0;

void qk_renderer_begin_frame(const qk_camera_t *camera)
{
    if (!g_r.initialized) return;

    if (s_start_ticks == 0) {
        s_start_ticks = SDL_GetPerformanceCounter();
    }

    u32 fi = g_r.frame_index % R_FRAMES_IN_FLIGHT;
    r_frame_data_t *frame = &g_r.frames[fi];

    // Wait for this frame's previous GPU work (timed)
    u64 fence_t0 = SDL_GetPerformanceCounter();
    vkWaitForFences(g_r.device.handle, 1, &frame->in_flight, VK_TRUE, UINT64_MAX);
    u64 fence_t1 = SDL_GetPerformanceCounter();
    g_r.stats_fence_wait_ms = (f32)((f64)(fence_t1 - fence_t0) / (f64)SDL_GetPerformanceFrequency() * 1000.0);

    // Read GPU timers from previous frame
    r_debug_timers_read();

    /* Recreate swapchain BEFORE acquire if flagged (vsync change, etc).
       Must happen after fence wait but before acquire to avoid
       double-signaling the image_available semaphore. */
    if (g_r.swapchain_needs_recreate) {
        r_vulkan_recreate_swapchain();
    }

    // Acquire swapchain image (timed)
    u64 acq_t0 = SDL_GetPerformanceCounter();
    VkResult result = vkAcquireNextImageKHR(
        g_r.device.handle, g_r.swapchain.handle, UINT64_MAX,
        frame->image_available, VK_NULL_HANDLE, &g_r.current_image_index);
    u64 acq_t1 = SDL_GetPerformanceCounter();
    g_r.stats_acquire_ms = (f32)((f64)(acq_t1 - acq_t0) / (f64)SDL_GetPerformanceFrequency() * 1000.0);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        r_vulkan_recreate_swapchain();
        result = vkAcquireNextImageKHR(
            g_r.device.handle, g_r.swapchain.handle, UINT64_MAX,
            frame->image_available, VK_NULL_HANDLE, &g_r.current_image_index);
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return;
    }

    vkResetFences(g_r.device.handle, 1, &frame->in_flight);

    // Update view uniforms
    if (camera && frame->view_ubo_mapped) {
        r_view_uniforms_t uniforms;
        memcpy(uniforms.view_projection, camera->view_projection, sizeof(f32) * 16);
        memcpy(uniforms.camera_pos, camera->position, sizeof(f32) * 3);
        u64 now = SDL_GetPerformanceCounter();
        uniforms.time = (f32)((f64)(now - s_start_ticks) / (f64)SDL_GetPerformanceFrequency());
        uniforms.tile_count_x  = g_r.lights.tile_count_x;
        uniforms.screen_width  = g_r.config.render_width;
        uniforms.screen_height = g_r.config.render_height;
        uniforms._pad = 0;
        memcpy(frame->view_ubo_mapped, &uniforms, sizeof(uniforms));

        // Cache inverse VP for compute light culling
        mat4_inverse(camera->view_projection, g_r.inv_view_projection);
    }

    // Reset per-frame draw lists
    g_r.ui_quad_count = 0;
    g_r.overlay_quad_count = 0;
    g_r.ui_overlay_active = false;
    g_r.entities.draw_count = 0;
    g_r.fx.draw_count = 0;
    g_r.fx.vertex_count = 0;
    g_r.lights.light_count = 0;
    g_r.stats_draw_calls = 0;
    g_r.stats_triangles = 0;
}

void qk_renderer_draw_world(void)
{
    // World drawing is deferred to end_frame where we record all commands
}

void qk_renderer_set_ui_layer(bool overlay)
{
    g_r.ui_overlay_active = overlay;
}

void qk_renderer_push_ui_quad(const qk_ui_quad_t *quad)
{
    r_ui_quad_t *dst;

    if (g_r.ui_overlay_active) {
        if (g_r.overlay_quad_count >= R_UI_MAX_QUADS) return;
        dst = &g_r.overlay_quads[g_r.overlay_quad_count++];
    } else {
        if (g_r.ui_quad_count >= R_UI_MAX_QUADS) return;
        dst = &g_r.ui_quads[g_r.ui_quad_count++];
    }

    dst->x  = quad->x;
    dst->y  = quad->y;
    dst->w  = quad->w;
    dst->h  = quad->h;
    dst->u0 = quad->u0;
    dst->v0 = quad->v0;
    dst->u1 = quad->u1;
    dst->v1 = quad->v1;
    dst->color      = quad->color;
    dst->texture_id = quad->texture_id;
}

void qk_renderer_end_frame(void)
{
    if (!g_r.initialized) return;

    u32 fi = g_r.frame_index % R_FRAMES_IN_FLIGHT;
    r_frame_data_t *frame = &g_r.frames[fi];
    VkCommandBuffer cmd = frame->command_buffer;

    // Reset and begin command buffer
    vkResetCommandPool(g_r.device.handle, frame->command_pool, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd, &begin_info);

    // Reset GPU timestamp queries
    if (g_r.gpu_timers.pool) {
        vkCmdResetQueryPool(cmd, g_r.gpu_timers.pool, 0, R_TIMESTAMP_COUNT);
    }

    // Timestamp: frame start
    r_debug_timestamp_write(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    // Upload dynamic lights to GPU SSBO
    r_compute_upload_lights();

    // --- Depth Pre-Pass ---
    r_depth_prepass_record(cmd, fi);

    // --- Light Cull Compute (always dispatch to keep tile SSBO valid) ---
    r_compute_record_cull(cmd, g_r.inv_view_projection);

    // --- Pass 1: World + UI ---
    r_debug_begin_label(cmd, "World Pass", 0.2f, 0.8f, 0.2f);
    {
        VkClearValue clears[2] = {
            { .color = { .float32 = { 0.1f, 0.1f, 0.15f, 1.0f } } },
            { .depthStencil = { 1.0f, 0 } }
        };

        VkRenderPassBeginInfo rp_info = {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass      = g_r.world_target.render_pass,
            .framebuffer     = g_r.world_target.framebuffer,
            .renderArea      = { .offset = { 0, 0 }, .extent = g_r.world_target.extent },
            .clearValueCount = 2,
            .pClearValues    = clears
        };

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
        r_world_record_commands(cmd, fi);
        r_entity_record_commands(cmd, fi);
        r_fx_record_commands(cmd, fi);
        r_ui_record_commands(cmd, fi);
        vkCmdEndRenderPass(cmd);
    }
    r_debug_end_label(cmd);

    // Timestamp: after world + UI
    r_debug_timestamp_write(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 1);

    // --- Bloom pass ---
    r_bloom_record_commands(cmd);

    // Timestamp: after bloom
    r_debug_timestamp_write(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 2);

    // --- Pass 2: Composition ---
    r_debug_begin_label(cmd, "Compose Pass", 0.2f, 0.2f, 0.8f);
    r_compose_record_commands(cmd, g_r.current_image_index, fi);
    r_debug_end_label(cmd);

    // Timestamp: after compose
    r_debug_timestamp_write(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 3);

    vkEndCommandBuffer(cmd);

    // Submit
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &frame->image_available,
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &frame->render_finished
    };

    vkQueueSubmit(g_r.device.graphics_queue, 1, &submit, frame->in_flight);

    // Present
    VkPresentInfoKHR present = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &frame->render_finished,
        .swapchainCount     = 1,
        .pSwapchains        = &g_r.swapchain.handle,
        .pImageIndices      = &g_r.current_image_index
    };

    VkResult result = vkQueuePresentKHR(g_r.device.graphics_queue, &present);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_r.swapchain_needs_recreate = true;
    }

    g_r.frame_index++;
}

// --- Debug ---

void qk_renderer_get_stats(qk_gpu_stats_t *out_stats)
{
    if (!out_stats) return;

    out_stats->gpu_frame_ms   = g_r.gpu_timers.gpu_frame_ms;
    out_stats->world_pass_ms  = g_r.gpu_timers.world_pass_ms;
    out_stats->ui_pass_ms     = g_r.gpu_timers.ui_pass_ms;
    out_stats->compose_pass_ms = g_r.gpu_timers.compose_pass_ms;
    out_stats->draw_calls     = g_r.stats_draw_calls;
    out_stats->triangles      = g_r.stats_triangles;
    out_stats->fence_wait_ms  = g_r.stats_fence_wait_ms;
    out_stats->acquire_ms     = g_r.stats_acquire_ms;
}
