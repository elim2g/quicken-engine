/*
 * QUICKEN Engine - Physics Internal Header
 *
 * Shared declarations for all p_*.c files. Not part of the public API.
 */

#ifndef P_INTERNAL_H
#define P_INTERNAL_H

#include "physics/qk_physics.h"

/* ---- Opaque world definition ---- */

struct qk_phys_world {
    qk_collision_model_t *cm;
    bool owns_cm; /* true = world frees cm on destroy (test room), false = caller manages cm */
};

/* ---- Internal constants ---- */

#define P_PI            3.14159265358979323846f
#define P_2PI           6.28318530717958647692f
#define P_DEG2RAD       (P_PI / 180.0f)
#define P_MAX_CLIP_PLANES   5
#define P_CLIP_EPSILON      0.001f

/* ---- p_math.c ---- */

f32     p_sinf(f32 x);
f32     p_cosf(f32 x);
void    p_angle_vectors(f32 pitch, f32 yaw,
                        vec3_t *forward, vec3_t *right, vec3_t *up);

/* ---- p_brush.c ---- */

void    p_brush_compute_aabb(qk_brush_t *brush);
void    p_brush_add_bevels(qk_brush_t *brush);
bool    p_aabb_overlap(vec3_t a_mins, vec3_t a_maxs,
                       vec3_t b_mins, vec3_t b_maxs);
void    p_compute_swept_aabb(vec3_t start, vec3_t end,
                             vec3_t mins, vec3_t maxs,
                             vec3_t *out_mins, vec3_t *out_maxs);

/* ---- p_trace.c ---- */

qk_trace_result_t p_trace_brush(const qk_brush_t *brush,
                                vec3_t start, vec3_t end,
                                vec3_t mins, vec3_t maxs);
qk_trace_result_t p_trace_world(const qk_phys_world_t *world,
                                vec3_t start, vec3_t end,
                                vec3_t mins, vec3_t maxs);

/* ---- p_accel.c ---- */

void    p_accelerate(qk_player_state_t *ps, vec3_t wish_dir,
                     f32 wish_speed, f32 accel, f32 dt);
void    p_air_accelerate(qk_player_state_t *ps, vec3_t wish_dir,
                         f32 wish_speed, f32 accel, f32 dt);
void    p_apply_friction(qk_player_state_t *ps, f32 dt);

/* ---- p_slide.c ---- */

vec3_t  p_clip_velocity(vec3_t velocity, vec3_t normal, f32 overbounce);
bool    p_slide_move(qk_player_state_t *ps, const qk_phys_world_t *world,
                     f32 dt, i32 max_bumps);
void    p_step_slide_move(qk_player_state_t *ps,
                          const qk_phys_world_t *world, f32 dt);

/* ---- p_move.c ---- */

void    p_categorize_position(qk_player_state_t *ps,
                              const qk_phys_world_t *world);
void    p_check_jump(qk_player_state_t *ps, const qk_usercmd_t *cmd);
void    p_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
               const qk_phys_world_t *world);

/* ---- p_world.c ---- */

qk_phys_world_t *p_world_create(qk_collision_model_t *cm);
void              p_world_destroy(qk_phys_world_t *world);

/* ---- p_time.c ---- */

void    p_time_update(qk_phys_time_t *ts, f32 frame_dt,
                      qk_player_state_t *ps, const qk_usercmd_t *cmd,
                      const qk_phys_world_t *world);
f32     p_time_get_alpha(const qk_phys_time_t *ts);

#endif /* P_INTERNAL_H */
