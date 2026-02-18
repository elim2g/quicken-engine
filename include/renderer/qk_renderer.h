/*
 * QUICKEN Engine - Renderer Public API
 *
 * Vulkan-backed renderer with offscreen composition.
 * Compiled with fast floating-point for maximum performance.
 */

#ifndef QK_RENDERER_H
#define QK_RENDERER_H

#include "quicken.h"
#include "qk_math.h"

/* Configuration */
typedef struct {
    void    *sdl_window;
    u32      render_width;      /* 0 = default (1920) */
    u32      render_height;     /* 0 = default (1080) */
    u32      window_width;
    u32      window_height;
    bool     aspect_fit;
    bool     vsync;
} qk_renderer_config_t;

/* Camera */
typedef struct {
    f32     view_projection[16];    /* column-major 4x4 */
    f32     position[3];
} qk_camera_t;

/* World vertex (produced by map loader, consumed by renderer) */
typedef struct {
    f32     position[3];
    f32     normal[3];
    f32     uv[2];
    u32     texture_id;
} qk_world_vertex_t;

/* Surface draw info */
typedef struct {
    u32     index_offset;
    u32     index_count;
    u32     vertex_offset;
    u32     texture_index;      /* BSP texture index (into texture lump) */
    u32     surface_flags;      /* Q3 surface flags (NODRAW, SKY, TRANS33, etc.) */
    u32     contents_flags;     /* Q3 contents flags (SOLID, FOG, PLAYERCLIP, etc.) */
} qk_draw_surface_t;

/* UI quad (low-level, used by UI module internally) */
typedef struct {
    f32     x, y, w, h;
    f32     u0, v0, u1, v1;
    u32     color;
    u32     texture_id;
} qk_ui_quad_t;

typedef u32 qk_texture_id_t;

/* GPU stats */
typedef struct {
    f64     gpu_frame_ms;
    f64     world_pass_ms;
    f64     ui_pass_ms;
    f64     compose_pass_ms;
    u32     draw_calls;
    u32     triangles;
    f32     fence_wait_ms;
    f32     acquire_ms;
} qk_gpu_stats_t;

/* Lifecycle */
qk_result_t qk_renderer_init(const qk_renderer_config_t *config);
void        qk_renderer_shutdown(void);

/* Resolution / display */
void qk_renderer_set_render_resolution(u32 width, u32 height);
void qk_renderer_set_aspect_mode(bool aspect_fit);
void qk_renderer_set_vsync(bool vsync);
void qk_renderer_handle_window_resize(u32 new_width, u32 new_height);

/* Resource upload (map load) */
qk_result_t qk_renderer_upload_world(
    const qk_world_vertex_t *vertices, u32 vertex_count,
    const u32 *indices, u32 index_count,
    const qk_draw_surface_t *surfaces, u32 surface_count);
qk_texture_id_t qk_renderer_upload_texture(
    const u8 *pixels, u32 width, u32 height, u32 channels);
void qk_renderer_free_world(void);

/* Frame rendering */
void qk_renderer_begin_frame(const qk_camera_t *camera);
void qk_renderer_draw_world(void);
void qk_renderer_push_ui_quad(const qk_ui_quad_t *quad);
void qk_renderer_end_frame(void);

/* Entity rendering (debug visuals for vertical slice) */
void qk_renderer_draw_capsule(f32 pos_x, f32 pos_y, f32 pos_z,
                               f32 radius, f32 half_height,
                               f32 yaw, u32 color_rgba);
void qk_renderer_draw_sphere(f32 pos_x, f32 pos_y, f32 pos_z,
                              f32 radius, u32 color_rgba);

/* Beam effects */
void qk_renderer_draw_rail_beam(f32 start_x, f32 start_y, f32 start_z,
                                 f32 end_x, f32 end_y, f32 end_z,
                                 f32 age_seconds, u32 color_rgba);

void qk_renderer_draw_lg_beam(f32 start_x, f32 start_y, f32 start_z,
                                f32 end_x, f32 end_y, f32 end_z,
                                f32 time_seconds);

/* Viewmodel (first-person weapon model, right-handed placement) */
void qk_renderer_draw_viewmodel(u32 weapon_id, f32 pitch_deg, f32 yaw_deg,
                                 f32 time_seconds, bool firing);

/* Rocket smoke trail (retro zdoom-style particles behind rocket) */
void qk_renderer_draw_rocket_trail(f32 pos_x, f32 pos_y, f32 pos_z,
                                    f32 vel_x, f32 vel_y, f32 vel_z,
                                    f32 age_seconds);

/* Smoke particle batch: camera-facing billboard quads through beam pipeline.
 * Call begin, emit N puffs, then end. All puffs go into one draw call. */
void qk_renderer_begin_smoke(void);
void qk_renderer_emit_smoke_puff(f32 x, f32 y, f32 z,
                                  f32 half_size, u32 color_rgba,
                                  f32 angle_rad);
void qk_renderer_end_smoke(void);

/* Rocket explosion: multi-billboard expanding fireball effect.
 * radius = splash damage radius (visual scale reference).
 * age_seconds = time since detonation (fizzles out over ~1 second).
 * r,g,b,a = color tint (1.0 = full intensity). */
void qk_renderer_draw_explosion(f32 x, f32 y, f32 z,
                                 f32 radius, f32 age_seconds,
                                 f32 r, f32 g, f32 b, f32 a);

/* Railgun surface impact: buzzy electrical particles at hit point.
 * normal_{x,y,z} = surface normal at impact (orients the particle burst).
 * in_dir_{x,y,z} = incoming ray direction (sparks reflect off surface).
 * age_seconds = time since impact (effect fades over ~1.5s).
 * color_rgba = base color (RRGGBBAA packed u32). */
void qk_renderer_draw_rail_impact(f32 x, f32 y, f32 z,
                                   f32 normal_x, f32 normal_y, f32 normal_z,
                                   f32 in_dir_x, f32 in_dir_y, f32 in_dir_z,
                                   f32 age_seconds, u32 color_rgba);

/* Debug */
void qk_renderer_get_stats(qk_gpu_stats_t *out_stats);

/* High-level UI drawing (convenience functions built on push_ui_quad).
 * Declared here, implemented in src/ui/ui_draw.c */
void qk_ui_draw_rect(f32 x, f32 y, f32 w, f32 h, u32 color_rgba);
void qk_ui_draw_text(f32 x, f32 y, const char *text, f32 size,
                      u32 color_rgba);
void qk_ui_draw_number(f32 x, f32 y, i32 value, f32 size, u32 color_rgba);
f32  qk_ui_text_width(const char *text, f32 size);

#endif /* QK_RENDERER_H */
