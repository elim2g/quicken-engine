/*
 * QUICKEN Engine - Window Management
 *
 * SDL3 window creation and management.
 */

#ifndef QK_WINDOW_H
#define QK_WINDOW_H

#include "quicken.h"

typedef struct {
    u32         width;
    u32         height;
    const char *title;
    bool        fullscreen;
} qk_window_config_t;

typedef struct qk_window qk_window_t;

qk_result_t qk_window_create(const qk_window_config_t *config, qk_window_t **out);
void        qk_window_destroy(qk_window_t *window);
void       *qk_window_get_native_handle(qk_window_t *window);
void        qk_window_get_size(qk_window_t *window, u32 *width, u32 *height);
void        qk_window_set_size(qk_window_t *window, u32 width, u32 height);
void        qk_window_set_fullscreen(qk_window_t *window, bool fullscreen);
bool        qk_window_is_fullscreen(qk_window_t *window);

#endif // QK_WINDOW_H
