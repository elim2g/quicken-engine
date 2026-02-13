/*
 * QUICKEN Engine - Fixed Timestep Accumulator
 *
 * Consumes real frame time in fixed-size ticks (128Hz).
 * Provides interpolation alpha for rendering.
 */

#include "p_internal.h"

/* ---- Fixed timestep update ---- */

void p_time_update(qk_phys_time_t *ts, f32 frame_dt,
                   qk_player_state_t *ps, const qk_usercmd_t *cmd,
                   const qk_phys_world_t *world) {
    /* Clamp incoming dt to prevent spiral-of-death */
    if (frame_dt > 0.25f) frame_dt = 0.25f;

    ts->accumulator += frame_dt;

    while (ts->accumulator >= QK_TICK_DT) {
        p_move(ps, cmd, world);
        ts->accumulator -= QK_TICK_DT;
        ts->tick_count++;
    }
}

/* ---- Get interpolation alpha for rendering ---- */

f32 p_time_get_alpha(const qk_phys_time_t *ts) {
    return ts->accumulator / QK_TICK_DT;
}
