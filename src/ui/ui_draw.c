/*
 * QUICKEN UI Draw Primitives - Stub Implementation
 *
 * Implements qk_ui_draw_* functions declared in renderer/qk_renderer.h.
 * These compile as part of the main exe (not the renderer static lib)
 * because they bridge the UI and renderer modules.
 *
 * The gameplay agent replaces this with real code on feat/gameplay.
 */

#include "renderer/qk_renderer.h"

void qk_ui_draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color_rgba) {
    QK_UNUSED(x); QK_UNUSED(y); QK_UNUSED(w); QK_UNUSED(h); QK_UNUSED(color_rgba);
}

void qk_ui_draw_text(f32 x, f32 y, const char *text, f32 size, u32 color_rgba) {
    QK_UNUSED(x); QK_UNUSED(y); QK_UNUSED(text); QK_UNUSED(size); QK_UNUSED(color_rgba);
}

void qk_ui_draw_number(f32 x, f32 y, i32 value, f32 size, u32 color_rgba) {
    QK_UNUSED(x); QK_UNUSED(y); QK_UNUSED(value); QK_UNUSED(size); QK_UNUSED(color_rgba);
}

f32 qk_ui_text_width(const char *text, f32 size) {
    QK_UNUSED(text); QK_UNUSED(size);
    return 0.0f;
}
