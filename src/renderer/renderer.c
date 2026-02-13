/*
 * QUICKEN Renderer Module - Stub Implementation
 *
 * All functions return success/zero/no-op.
 * The renderer agent replaces this with real code on feat/renderer.
 *
 * Note: qk_ui_draw_* functions are NOT here -- they are in src/ui/ui_draw.c
 * because they compile as part of the main exe, not the renderer static lib.
 */

#include "renderer/qk_renderer.h"

qk_result_t qk_renderer_init(const qk_renderer_config_t *config) {
    QK_UNUSED(config);
    return QK_SUCCESS;
}

void qk_renderer_shutdown(void) {}

void qk_renderer_set_render_resolution(u32 width, u32 height) {
    QK_UNUSED(width); QK_UNUSED(height);
}

void qk_renderer_set_aspect_mode(bool aspect_fit) {
    QK_UNUSED(aspect_fit);
}

void qk_renderer_handle_window_resize(u32 new_width, u32 new_height) {
    QK_UNUSED(new_width); QK_UNUSED(new_height);
}

qk_result_t qk_renderer_upload_world(
    const qk_world_vertex_t *vertices, u32 vertex_count,
    const u32 *indices, u32 index_count,
    const qk_draw_surface_t *surfaces, u32 surface_count) {
    QK_UNUSED(vertices); QK_UNUSED(vertex_count);
    QK_UNUSED(indices); QK_UNUSED(index_count);
    QK_UNUSED(surfaces); QK_UNUSED(surface_count);
    return QK_SUCCESS;
}

qk_texture_id_t qk_renderer_upload_texture(
    const u8 *pixels, u32 width, u32 height, u32 channels) {
    QK_UNUSED(pixels); QK_UNUSED(width); QK_UNUSED(height); QK_UNUSED(channels);
    return 0;
}

void qk_renderer_free_world(void) {}

void qk_renderer_begin_frame(const qk_camera_t *camera) {
    QK_UNUSED(camera);
}

void qk_renderer_draw_world(void) {}

void qk_renderer_push_ui_quad(const qk_ui_quad_t *quad) {
    QK_UNUSED(quad);
}

void qk_renderer_end_frame(void) {}

void qk_renderer_get_stats(qk_gpu_stats_t *out_stats) {
    if (out_stats) {
        qk_gpu_stats_t empty = {0};
        *out_stats = empty;
    }
}
