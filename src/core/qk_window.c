/*
 * QUICKEN Window Management - Real SDL3 Implementation
 *
 * Creates an SDL3 window with Vulkan support.
 */

#include "core/qk_window.h"

#ifndef QK_HEADLESS

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>

struct qk_window {
    SDL_Window *sdl_window;
    u32 width;
    u32 height;
};

qk_result_t qk_window_create(const qk_window_config_t *config, qk_window_t **out) {
    if (!config || !out) return QK_ERROR_INVALID_PARAM;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[Window] SDL_Init failed: %s\n", SDL_GetError());
        return QK_ERROR_INIT_FAILED;
    }

    u32 w = config->width  ? config->width  : 1280;
    u32 h = config->height ? config->height : 720;
    const char *title = config->title ? config->title : "QUICKEN Engine";

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (config->fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    SDL_Window *sdl_win = SDL_CreateWindow(title, (int)w, (int)h, flags);
    if (!sdl_win) {
        fprintf(stderr, "[Window] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return QK_ERROR_INIT_FAILED;
    }

    qk_window_t *window = (qk_window_t *)calloc(1, sizeof(qk_window_t));
    if (!window) {
        SDL_DestroyWindow(sdl_win);
        return QK_ERROR_OUT_OF_MEMORY;
    }

    window->sdl_window = sdl_win;
    window->width = w;
    window->height = h;

    *out = window;

    fprintf(stderr, "[Window] Created %ux%u (%s)\n", w, h,
            config->fullscreen ? "fullscreen" : "windowed");
    return QK_SUCCESS;
}

void qk_window_destroy(qk_window_t *window) {
    if (!window) return;
    if (window->sdl_window) {
        SDL_DestroyWindow(window->sdl_window);
    }
    free(window);
    SDL_Quit();
}

void *qk_window_get_native_handle(qk_window_t *window) {
    if (!window) return NULL;
    return window->sdl_window;
}

void qk_window_get_size(qk_window_t *window, u32 *width, u32 *height) {
    if (!window) {
        if (width)  *width = 0;
        if (height) *height = 0;
        return;
    }

    int w, h;
    SDL_GetWindowSize(window->sdl_window, &w, &h);
    window->width  = (u32)w;
    window->height = (u32)h;

    if (width)  *width  = window->width;
    if (height) *height = window->height;
}

void qk_window_set_size(qk_window_t *window, u32 width, u32 height) {
    if (!window) return;
    SDL_SetWindowSize(window->sdl_window, (int)width, (int)height);
    window->width = width;
    window->height = height;
}

void qk_window_set_fullscreen(qk_window_t *window, bool fullscreen) {
    if (!window) return;
    SDL_SetWindowFullscreen(window->sdl_window, fullscreen);
}

bool qk_window_is_fullscreen(qk_window_t *window) {
    if (!window) return false;
    return (SDL_GetWindowFlags(window->sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

#else /* QK_HEADLESS */

/* Headless: no window support */
struct qk_window { u32 dummy; };

qk_result_t qk_window_create(const qk_window_config_t *config, qk_window_t **out) {
    QK_UNUSED(config); QK_UNUSED(out);
    return QK_ERROR_INIT_FAILED;
}

void qk_window_destroy(qk_window_t *window) { QK_UNUSED(window); }
void *qk_window_get_native_handle(qk_window_t *window) { QK_UNUSED(window); return NULL; }
void qk_window_get_size(qk_window_t *window, u32 *width, u32 *height) {
    QK_UNUSED(window);
    if (width)  *width = 0;
    if (height) *height = 0;
}
void qk_window_set_size(qk_window_t *window, u32 width, u32 height) {
    QK_UNUSED(window); QK_UNUSED(width); QK_UNUSED(height);
}
void qk_window_set_fullscreen(qk_window_t *window, bool fullscreen) {
    QK_UNUSED(window); QK_UNUSED(fullscreen);
}
bool qk_window_is_fullscreen(qk_window_t *window) {
    QK_UNUSED(window); return false;
}

#endif /* QK_HEADLESS */
