/*
 * QUICKEN Renderer - AVX2 Particle Effects
 *
 * Compiled with /arch:AVX2 (MSVC) or -mavx2 -mfma (GCC/Clang).
 * Contains AVX2-optimized versions of particle generation loops.
 * These are called via runtime dispatch when the CPU supports AVX2.
 *
 * For now, these are stubs that call the baseline implementations.
 * TODO: Implement actual AVX2 vectorized versions of the hot loops.
 */

#include <immintrin.h>

#include "r_types.h"

/* --- Baseline prototypes from r_fx.c --- */

extern void r_fx_draw_rail_beam_baseline(f32 start_x, f32 start_y, f32 start_z,
                                          f32 end_x, f32 end_y, f32 end_z,
                                          f32 age_seconds, u32 color_rgba);

extern void r_fx_draw_explosion_baseline(f32 x, f32 y, f32 z,
                                          f32 radius, f32 age_seconds,
                                          f32 r, f32 g, f32 b, f32 a);

/* --- AVX2 stubs (forward to baseline for now) --- */

void r_fx_draw_rail_beam_avx2(f32 start_x, f32 start_y, f32 start_z,
                               f32 end_x, f32 end_y, f32 end_z,
                               f32 age_seconds, u32 color_rgba)
{
    r_fx_draw_rail_beam_baseline(start_x, start_y, start_z,
                                 end_x, end_y, end_z,
                                 age_seconds, color_rgba);
}

void r_fx_draw_explosion_avx2(f32 x, f32 y, f32 z,
                               f32 radius, f32 age_seconds,
                               f32 r, f32 g, f32 b, f32 a)
{
    r_fx_draw_explosion_baseline(x, y, z, radius, age_seconds, r, g, b, a);
}
