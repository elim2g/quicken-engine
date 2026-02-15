/*
 * QUICKEN Renderer - Beam Effects (Rail + Lightning Gun)
 *
 * Stubs. Renderer agent implements the real visuals.
 */

#include "renderer/qk_renderer.h"

void qk_renderer_draw_rail_beam(f32 start_x, f32 start_y, f32 start_z,
                                 f32 end_x, f32 end_y, f32 end_z,
                                 f32 age_seconds, u32 color_rgba)
{
    (void)start_x; (void)start_y; (void)start_z;
    (void)end_x;   (void)end_y;   (void)end_z;
    (void)age_seconds; (void)color_rgba;
}

void qk_renderer_draw_lg_beam(f32 start_x, f32 start_y, f32 start_z,
                                f32 end_x, f32 end_y, f32 end_z,
                                f32 time_seconds)
{
    (void)start_x; (void)start_y; (void)start_z;
    (void)end_x;   (void)end_y;   (void)end_z;
    (void)time_seconds;
}
