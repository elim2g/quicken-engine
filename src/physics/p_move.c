/*
 * QUICKEN Engine - Top-Level Movement
 *
 * PM_Move: categorize position, jump check, friction, acceleration,
 * gravity, and slide/step-slide dispatch.
 */

#include "p_internal.h"

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

/* ---- Jump check (edge-triggered with input buffer) ---- */

void p_check_jump(qk_player_state_t *ps, const qk_usercmd_t *cmd) {
    bool want_jump = (cmd->buttons & QK_BUTTON_JUMP) != 0;

    if (!want_jump) {
        ps->jump_held = false;
        ps->jump_buffer_ticks = 0;
        return;
    }

    /* Fresh press: set held flag and start/refresh buffer */
    bool fresh_press = !ps->jump_held;
    ps->jump_held = true;

    if (fresh_press && !ps->on_ground) {
        /* Pressed jump in air — start the buffer countdown */
        ps->jump_buffer_ticks = QK_PM_JUMP_BUFFER_TICKS;
    }

    /* Fire jump if on ground AND (fresh press OR buffered) */
    if (ps->on_ground && (fresh_press || ps->jump_buffer_ticks > 0)) {
        ps->jump_buffer_ticks = 0;
        /* Q3 behavior: setting on_ground = false here means friction
           is skipped on the jump frame, which is essential for consistent
           strafejump speeds. */
        ps->on_ground = false;
        ps->velocity.z = QK_PM_JUMP_VELOCITY;
    }
}

/* ---- PM_Move: one physics tick ---- */

void p_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
            const qk_phys_world_t *world) {

    f32 dt = QK_TICK_DT;

    /* 1. Compute wish direction from input + view angles */
    vec3_t forward, right, up;
    p_angle_vectors(cmd->pitch, cmd->yaw, &forward, &right, &up);

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
    p_categorize_position(ps, world);

    /* 3. Jump check */
    p_check_jump(ps, cmd);

    /* 3b. Decrement jump buffer while airborne */
    if (!ps->on_ground && ps->jump_buffer_ticks > 0) {
        ps->jump_buffer_ticks--;
    }

    /* 4. Apply friction (ground only) */
    if (ps->on_ground) {
        p_apply_friction(ps, dt);
    }

    /* 5. Accelerate */
    if (ps->on_ground) {
        p_accelerate(ps, wish_dir, wish_speed, QK_PM_GROUND_ACCEL, dt);
    } else {
        p_accelerate(ps, wish_dir, wish_speed, QK_PM_AIR_ACCEL, dt);
    }

    /* 6. Apply gravity (air only) */
    if (!ps->on_ground) {
        ps->velocity.z -= ps->gravity * dt;
    }

    /* 7. Move and collide */
    if (ps->on_ground) {
        p_step_slide_move(ps, world, dt);
    } else {
        p_slide_move(ps, world, dt, 4);
    }

    /* 8. Re-check ground after move */
    p_categorize_position(ps, world);
}
