/*
 * QUICKEN Window Management - Stub Implementation
 *
 * Will be replaced with real SDL3 window creation by the Principal Engineer.
 */

#include "core/qk_window.h"

struct qk_window {
    u32 width;
    u32 height;
};

qk_result_t qk_window_create(const qk_window_config_t *config, qk_window_t **out) {
    QK_UNUSED(config); QK_UNUSED(out);
    return QK_SUCCESS;
}

void qk_window_destroy(qk_window_t *window) {
    QK_UNUSED(window);
}

void *qk_window_get_native_handle(qk_window_t *window) {
    QK_UNUSED(window);
    return NULL;
}

void qk_window_get_size(qk_window_t *window, u32 *width, u32 *height) {
    QK_UNUSED(window);
    if (width)  *width = 0;
    if (height) *height = 0;
}
