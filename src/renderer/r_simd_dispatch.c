/*
 * QUICKEN Renderer - SIMD Dispatch Initialization
 *
 * Populates the global dispatch table with function pointers based on
 * the CPU's SIMD tier (detected at boot via qk_cpuid).
 *
 * Baseline (SSE2) implementations live in r_fx.c.
 * AVX2 implementations live in r_fx_avx2.c (compiled with /arch:AVX2).
 */

#include "r_simd_dispatch.h"
#include "core/qk_simd_dispatch.h"
#include <stdio.h>

/* --- Baseline (SSE2) prototypes from r_fx.c --- */

extern void r_fx_draw_rail_beam_baseline(f32 start_x, f32 start_y, f32 start_z,
                                          f32 end_x, f32 end_y, f32 end_z,
                                          f32 age_seconds, u32 color_rgba);

extern void r_fx_draw_explosion_baseline(f32 x, f32 y, f32 z,
                                          f32 radius, f32 age_seconds,
                                          f32 r, f32 g, f32 b, f32 a);

/* --- AVX2 prototypes from r_fx_avx2.c --- */

extern void r_fx_draw_rail_beam_avx2(f32 start_x, f32 start_y, f32 start_z,
                                      f32 end_x, f32 end_y, f32 end_z,
                                      f32 age_seconds, u32 color_rgba);

extern void r_fx_draw_explosion_avx2(f32 x, f32 y, f32 z,
                                      f32 radius, f32 age_seconds,
                                      f32 r, f32 g, f32 b, f32 a);

/* --- Global dispatch table --- */

r_simd_dispatch_t g_r_dispatch;

void r_simd_dispatch_init(void)
{
    qk_simd_tier_t tier = qk_simd_get_tier();

    if (tier >= QK_SIMD_AVX2) {
        g_r_dispatch.draw_rail_beam = r_fx_draw_rail_beam_avx2;
        g_r_dispatch.draw_explosion = r_fx_draw_explosion_avx2;
    } else {
        g_r_dispatch.draw_rail_beam = r_fx_draw_rail_beam_baseline;
        g_r_dispatch.draw_explosion = r_fx_draw_explosion_baseline;
    }

    fprintf(stderr, "[Renderer] FX SIMD dispatch: %s\n", qk_simd_tier_name(tier));
}
