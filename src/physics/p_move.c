/*
 * QUICKEN Engine - Top-Level Movement
 *
 * PM_Move: categorize position, jump check, friction, acceleration,
 * gravity, and slide/step-slide dispatch.
 *
 * CPM (Challenge ProMode) movement layered on top of VQ3 strafejumping:
 *   - A/D-only air input: high air accel (70.0) for smooth air curves
 *   - W/S-only air input: speed-clamped turning (no gain, no loss)
 *   - Diagonal air input: standard VQ3 air accel (strafejumping)
 *   - Double-jump: boosted jump velocity within a window after landing
 *   - Higher ground acceleration (15.0 vs 10.0)
 */

#include "p_internal.h"
#include <math.h>

/* Convert the double-jump window from ms to ticks at compile time */
#define P_CPM_DOUBLE_JUMP_TICKS \
    ((u32)(QK_PM_CPM_DOUBLE_JUMP_WINDOW * QK_TICK_RATE / 1000))

/* ---- Categorize position (ground check) ---- */

void p_categorize_position(qk_player_state_t *ps,
                           const qk_phys_world_t *world) {
    /* Trace from slightly above current position to below it.
       The small upward offset ensures the trace starts clearly
       outside the floor brush even when origin sits exactly on
       the Minkowski-expanded surface boundary. */
    vec3_t start = ps->origin;
    start.z += 0.125f;

    vec3_t end = ps->origin;
    end.z -= 0.25f;

    qk_trace_result_t trace = p_trace_world(world, start, end,
                                             ps->mins, ps->maxs);

    if (trace.fraction < 1.0f &&
        trace.hit_normal.z >= QK_PM_MIN_WALK_NORMAL) {
        ps->on_ground = true;
        ps->ground_normal = trace.hit_normal;
    } else {
        ps->on_ground = false;
        ps->ground_normal = (vec3_t){0.0f, 0.0f, 0.0f};
    }
}

/* ---- Jump check (hold-to-jump with input buffer + CPM double-jump) ---- */

void p_check_jump(qk_player_state_t *ps, const qk_usercmd_t *cmd) {
    bool want_jump = (cmd->buttons & QK_BUTTON_JUMP) != 0;

    if (!want_jump) {
        ps->jump_held = false;
        ps->jump_buffer_ticks = 0;
        return;
    }

    /* Fresh press while airborne: start the buffer countdown */
    bool fresh_press = !ps->jump_held;
    ps->jump_held = true;

    if (fresh_press && !ps->on_ground) {
        ps->jump_buffer_ticks = QK_PM_JUMP_BUFFER_TICKS;
    }

    /* Fire jump every frame the player is on ground and holding jump.
       The buffer handles the air-press-then-land case automatically
       since want_jump is still true when we land. */
    if (ps->on_ground && want_jump) {
        ps->jump_buffer_ticks = 0;
        /* Q3 behavior: setting on_ground = false here means friction
           is skipped on the jump frame, which is essential for consistent
           strafejump speeds. */
        ps->on_ground = false;

        /* CPM double-jump: if jumping within the window after landing,
           boost jump velocity. last_land_tick == 0 means never landed
           (fresh spawn), so no boost in that case. */
        u32 since_land = ps->command_time - ps->last_land_tick;
        if (ps->last_land_tick > 0 && since_land <= P_CPM_DOUBLE_JUMP_TICKS) {
            ps->velocity.z = QK_PM_JUMP_VELOCITY * QK_PM_CPM_DOUBLE_JUMP_BOOST;
        } else {
            ps->velocity.z = QK_PM_JUMP_VELOCITY;
        }
    }
}

/* ---- CPM air acceleration dispatch ---- */

static void p_cpm_air_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                           vec3_t wish_dir, f32 wish_speed, f32 dt) {
    bool has_forward = (cmd->forward_move > 0.0001f || cmd->forward_move < -0.0001f);
    bool has_side    = (cmd->side_move > 0.0001f || cmd->side_move < -0.0001f);

    if (!has_forward && has_side) {
        /* A/D only: CPM air strafing -- high air accel for smooth curves */
        p_air_accelerate(ps, wish_dir, wish_speed,
                         QK_PM_CPM_AIR_ACCEL, dt);
    } else if (has_forward && !has_side) {
        /* W/S only: CPM W-turning -- allow turning but clamp speed.
           Apply high air accel, then scale horizontal velocity back
           to the pre-acceleration speed if it increased. */
        f32 speed_before = sqrtf(ps->velocity.x * ps->velocity.x +
                                 ps->velocity.y * ps->velocity.y);

        p_air_accelerate(ps, wish_dir, wish_speed,
                         QK_PM_CPM_AIR_ACCEL, dt);

        f32 speed_after = sqrtf(ps->velocity.x * ps->velocity.x +
                                ps->velocity.y * ps->velocity.y);

        if (speed_after > speed_before && speed_before > 0.0001f) {
            f32 scale = speed_before / speed_after;
            ps->velocity.x *= scale;
            ps->velocity.y *= scale;
        }
    } else {
        /* Diagonal (W+A, W+D, etc.) or no input: standard VQ3 air accel.
           This preserves classic strafejumping. */
        p_air_accelerate(ps, wish_dir, wish_speed, QK_PM_AIR_ACCEL, dt);
    }
}

/* ---- PM_Move: one physics tick ---- */

void p_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
            const qk_phys_world_t *world) {

    f32 dt = QK_TICK_DT;

    /* Advance the tick counter (used for double-jump timing) */
    ps->command_time++;

    /* 1. Compute wish direction from yaw only (pitch must not affect
          movement speed -- looking up/down should not slow acceleration) */
    vec3_t forward, right, up;
    p_angle_vectors(0.0f, cmd->yaw, &forward, &right, &up);

    vec3_t wish_dir;
    wish_dir.x = forward.x * cmd->forward_move + right.x * cmd->side_move;
    wish_dir.y = forward.y * cmd->forward_move + right.y * cmd->side_move;
    wish_dir.z = 0.0f; /* no vertical component from input */

    f32 wish_speed = vec3_length(wish_dir);
    if (wish_speed > 0.0001f) {
        wish_dir = vec3_scale(wish_dir, 1.0f / wish_speed);
        wish_speed *= ps->max_speed;
        if (wish_speed > ps->max_speed) wish_speed = ps->max_speed;
    } else {
        wish_dir = (vec3_t){0.0f, 0.0f, 0.0f};
        wish_speed = 0.0f;
    }

    /* 2. Check ground */
    bool was_airborne = !ps->on_ground;
    p_categorize_position(ps, world);

    /* 2b. Track landing for CPM double-jump */
    if (was_airborne && ps->on_ground) {
        ps->last_land_tick = ps->command_time;
    }

    /* 3. Jump check (includes CPM double-jump boost) */
    p_check_jump(ps, cmd);

    /* 3b. Decrement jump buffer while airborne */
    if (!ps->on_ground && ps->jump_buffer_ticks > 0) {
        ps->jump_buffer_ticks--;
    }

    /* 4. Apply friction (ground only, skip during rocket-jump slick) */
    if (ps->on_ground && ps->splash_slick_ticks == 0) {
        p_apply_friction(ps, dt);
    }

    /* 5. Accelerate */
    if (ps->on_ground) {
        /* CPM: higher ground acceleration for snappier movement */
        p_accelerate(ps, wish_dir, wish_speed, QK_PM_CPM_GROUND_ACCEL, dt);
    } else {
        /* CPM air movement: dispatch based on input type */
        p_cpm_air_move(ps, cmd, wish_dir, wish_speed, dt);
    }

    /* 6. Apply gravity (air only) */
    if (!ps->on_ground) {
        ps->velocity.z -= ps->gravity * dt;
    }

    /* 7. Move and collide.
       StepSlideMove is used for both ground AND air movement. This is
       critical for stair traversal while holding jump: the player is
       briefly airborne between hops and would otherwise hit the vertical
       face of the next step without step-up kicking in. Q3 uses
       PM_StepSlideMove in both code paths. */
    vec3_t skim_saved_vel = ps->velocity;
    f32 pre_collision_vz = ps->velocity.z;

    p_step_slide_move(ps, world, dt);

    /* 8. Re-check ground after move */
    p_categorize_position(ps, world);

    /* Zero vertical velocity while firmly grounded. The collision
       response in p_slide_move clips velocity against the floor normal,
       but floating-point residuals can cause micro-oscillation.
       Only do this when NOT in skim (skim needs to preserve momentum). */
    if (ps->on_ground && ps->skim_ticks == 0) {
        ps->velocity.z = 0.0f;
    }

    /* Skim velocity restore: preserve speed through wall clips.
       Done after step 8 so on_ground reflects post-collision state.
       Only restore z when airborne -- on ground, the collision response
       properly zeroes z against the floor; restoring a stale negative z
       causes vertical oscillation (floor vibration). */
    if (ps->skim_ticks > 0) {
        ps->velocity.x = skim_saved_vel.x;
        ps->velocity.y = skim_saved_vel.y;
        if (!ps->on_ground) {
            ps->velocity.z = skim_saved_vel.z;
        }
    }

    /* 8b. Skim activation: landing after meaningful airtime.
       Checked after step 8 so on_ground is reliable regardless of
       fall speed (old step 2b missed fast landings where the player
       passed through the 0.25-unit ground-trace window in one tick).
       Uses pre-collision velocity since floor collision clips z to 0. */
    if (was_airborne && ps->on_ground && pre_collision_vz < -50.0f) {
        ps->skim_ticks = QK_PM_SKIM_TICKS;
    }

    /* Slick period: don't snap to ground while moving upward (rocket jump) */
    if (ps->splash_slick_ticks > 0 && ps->velocity.z > 0.0f) {
        ps->on_ground = false;
    }

    /* 9. Decrement splash slick counter */
    if (ps->splash_slick_ticks > 0) {
        ps->splash_slick_ticks--;
    }

    /* 10. Decrement skim counter. The timer runs regardless of ground
       state -- the whole point is that after landing, the player jumps
       and skims wall corners while AIRBORNE during the skim window. */
    if (ps->skim_ticks > 0) {
        ps->skim_ticks--;
    }
}
