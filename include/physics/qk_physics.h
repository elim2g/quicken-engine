/*
 * QUICKEN Engine - Physics Public API
 *
 * World creation, player movement, box tracing.
 * Compiled with precise floating-point for cross-platform determinism.
 */

#ifndef QK_PHYSICS_H
#define QK_PHYSICS_H

#include "quicken.h"
#include "qk_math.h"
#include "qk_types.h"

/* Opaque world handle */
typedef struct qk_phys_world qk_phys_world_t;

/* Collision model (provided by map loader) */
typedef struct {
    vec3_t  normal;
    f32     dist;
} qk_plane_t;

typedef struct {
    qk_plane_t *planes;
    u32          plane_count;
    vec3_t       mins;
    vec3_t       maxs;
} qk_brush_t;

typedef struct {
    qk_brush_t *brushes;
    u32          brush_count;
} qk_collision_model_t;

/* Physics time state */
typedef struct {
    f32     accumulator;
    u32     tick_count;
} qk_phys_time_t;

/* Lifecycle */
qk_phys_world_t *qk_physics_world_create(qk_collision_model_t *cm);
void              qk_physics_world_destroy(qk_phys_world_t *world);

/* Create a hardcoded test room (512x512x256 box) for testing without a .map file */
qk_phys_world_t *qk_physics_world_create_test_room(void);

/* Player init */
void qk_physics_player_init(qk_player_state_t *ps, vec3_t spawn_origin);

/* Run one physics tick (fixed timestep, called from game tick) */
void qk_physics_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                      const qk_phys_world_t *world);

/* Fixed-timestep wrapper (accumulates real time, runs fixed ticks) */
void qk_physics_update(qk_phys_time_t *ts, f32 frame_dt,
                        qk_player_state_t *ps, const qk_usercmd_t *cmd,
                        const qk_phys_world_t *world);

/* Trace a box through the world */
qk_trace_result_t qk_physics_trace(const qk_phys_world_t *world,
                                     vec3_t start, vec3_t end,
                                     vec3_t mins, vec3_t maxs);

/* Calculate launch velocity for a jump pad given start and target positions.
   Returns the velocity vector that will arc the player from 'start' to 'target'
   under QK_PM_GRAVITY. Used by gameplay to apply jump pad impulse. */
vec3_t qk_physics_jumppad_velocity(vec3_t start, vec3_t target);

/* Get interpolation alpha for rendering */
f32 qk_physics_get_alpha(const qk_phys_time_t *ts);

/* Debug: run strafejump validation test, prints speed log to stdout.
   Returns true if speed gain matches expected Q3 behavior. */
bool qk_physics_validate_strafejump(void);

/* Debug: load a .map file and validate traces + movement against it.
   Returns true if all trace/movement tests pass. */
bool qk_physics_validate_map(const char *map_path);

/* Constants */
#define QK_PHYSICS_TICK_RATE    128
#define QK_PHYSICS_TICK_DT      (1.0f / 128.0f)

#endif /* QK_PHYSICS_H */
