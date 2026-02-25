/*
 * QUICKEN Renderer - SIMD Dispatch Table
 *
 * Runtime dispatch for hot particle generation paths.
 * At init, function pointers are set based on CPU SIMD tier:
 *   - Baseline (SSE2): scalar loops in r_fx.c
 *   - AVX2: vectorized loops in r_fx_avx2.c
 *
 * The public API (qk_renderer_draw_rail_beam, qk_renderer_draw_explosion)
 * calls through these pointers transparently.
 */

#ifndef R_SIMD_DISPATCH_H
#define R_SIMD_DISPATCH_H

#include "r_types.h"

/* Function pointer types for dispatchable particle generators.
 * These generate vertices directly into the FX vertex buffer. */

typedef void (*r_fx_rail_beam_fn)(f32 start_x, f32 start_y, f32 start_z,
                                   f32 end_x, f32 end_y, f32 end_z,
                                   f32 age_seconds, u32 color_rgba);

typedef void (*r_fx_explosion_fn)(f32 x, f32 y, f32 z,
                                   f32 radius, f32 age_seconds,
                                   f32 r, f32 g, f32 b, f32 a);

typedef struct {
    r_fx_rail_beam_fn  draw_rail_beam;
    r_fx_explosion_fn  draw_explosion;
} r_simd_dispatch_t;

extern r_simd_dispatch_t g_r_dispatch;

void r_simd_dispatch_init(void);

#endif /* R_SIMD_DISPATCH_H */
