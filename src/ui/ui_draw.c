/*
 * QUICKEN UI Draw Primitives
 *
 * Implements qk_ui_draw_* functions declared in renderer/qk_renderer.h.
 * These bridge the UI module and the renderer by converting high-level
 * draw calls into qk_ui_quad_t pushes.
 */

#include "renderer/qk_renderer.h"
#include <string.h>
#include <stdio.h>

/* ---- Solid Rectangle ---- */
void qk_ui_draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color_rgba) {
    qk_ui_quad_t quad = {0};
    quad.x = x;
    quad.y = y;
    quad.w = w;
    quad.h = h;
    quad.u0 = 0.0f;
    quad.v0 = 0.0f;
    quad.u1 = 1.0f;
    quad.v1 = 1.0f;
    quad.color = color_rgba;
    quad.texture_id = 0; /* 0 = white/solid fill */
    qk_renderer_push_ui_quad(&quad);
}

/* ---- Text ---- */
void qk_ui_draw_text(f32 x, f32 y, const char *text, f32 size, u32 color_rgba) {
    if (!text || !text[0]) return;

    /*
     * Minimal glyph rendering: each character is drawn as a solid quad.
     * This is a placeholder until the font atlas system is implemented.
     * Characters are approximated as 0.6 * size width monospace glyphs.
     */
    f32 glyph_w = size * 0.6f;
    f32 glyph_h = size;
    f32 cx = x;

    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            cx += glyph_w;
            continue;
        }

        qk_ui_quad_t quad = {0};
        quad.x = cx;
        quad.y = y;
        quad.w = glyph_w * 0.8f; /* slight gap between characters */
        quad.h = glyph_h;
        quad.u0 = 0.0f;
        quad.v0 = 0.0f;
        quad.u1 = 1.0f;
        quad.v1 = 1.0f;
        quad.color = color_rgba;
        quad.texture_id = 0; /* placeholder: solid quads until font atlas */
        qk_renderer_push_ui_quad(&quad);

        cx += glyph_w;
    }
}

/* ---- Number ---- */
void qk_ui_draw_number(f32 x, f32 y, i32 value, f32 size, u32 color_rgba) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    qk_ui_draw_text(x, y, buf, size, color_rgba);
}

/* ---- Text Width Measurement ---- */
f32 qk_ui_text_width(const char *text, f32 size) {
    if (!text) return 0.0f;
    f32 glyph_w = size * 0.6f;
    f32 width = 0.0f;
    for (const char *p = text; *p; p++) {
        width += glyph_w;
    }
    return width;
}
