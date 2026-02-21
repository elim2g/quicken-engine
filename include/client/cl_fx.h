/*
 * QUICKEN Engine - Client Visual Effects
 *
 * Rail beams, smoke trails, explosions, entity rendering, viewmodel, LG beam.
 * All client-side VFX state and drawing lives here.
 */

#ifndef CL_FX_H
#define CL_FX_H

#include "quicken.h"
#include "qk_types.h"
#include "renderer/qk_renderer.h"
#include "core/qk_input.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"

/* Forward declaration */
typedef struct qk_phys_world qk_phys_world_t;

/* Per-frame context passed to cl_fx_draw. */
typedef struct {
    const qk_interp_state_t     *interp;
    const qk_camera_t            *camera;
    const qk_input_state_t       *input;
    const qk_player_state_t      *predicted_ps;
    const qk_phys_world_t        *world;
    u8                             local_client_id;
    f32                            cam_pitch;
    f32                            cam_yaw;
    f64                            now;
    bool                           has_prediction;
} cl_fx_frame_t;

void cl_fx_init(void);
void cl_fx_reset(void);

/* Add explosion visuals (called from server_tick after gameplay events). */
void cl_fx_add_explosions(const qk_explosion_event_t *events, u32 count, f64 now);

/* Draw all VFX: entities, smoke, explosions, viewmodel, beams. */
void cl_fx_draw(const cl_fx_frame_t *frame);

/* Diagnostic access: previous-frame entity flags (for beam edge detection). */
u8 cl_fx_get_prev_flags(u8 entity_id);

#endif /* CL_FX_H */
