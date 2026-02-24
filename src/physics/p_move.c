/*
 * QUICKEN Engine - Top-Level Movement
 *
 * PM_Move: categorize position, jump check, friction, acceleration,
 * gravity, and slide/step-slide dispatch.
 *
 * CPM (Challenge ProMode) movement with exclusive air-input branches:
 *   - Diagonal (W+A/W+D/S+A/S+D): VQ3 strafejump (low accel, high wishspeed)
 *   - A/D only: CPM air strafing (high accel, low wishspeed — tight curves)
 *   - W/S only: CPM air control (velocity rotation, no speed change)
 *   - Double-jump: boosted jump velocity within a window after landing
 *   - Higher ground acceleration (15.0 vs 10.0)
 */

#include "p_internal.h"
#include <math.h>

// Convert the double-jump window from ms to ticks at compile time
#define P_CPM_DOUBLE_JUMP_TICKS \
    ((u32)(QK_PM_CPM_DOUBLE_JUMP_WINDOW * QK_TICK_RATE / 1000))

// --- Categorize position (ground check) ---

void p_categorize_position(qk_player_state_t *ps,
                           const qk_phys_world_t *world) {
    // Trace from slightly above current position to below it.
    // The small upward offset ensures the trace starts clearly
    // outside the floor brush even when origin sits exactly on
    // the Minkowski-expanded surface boundary.
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

// --- Jump check (hold-to-jump with input buffer + CPM double-jump) ---

void p_check_jump(qk_player_state_t *ps, const qk_usercmd_t *cmd) {
    bool want_jump = (cmd->buttons & QK_BUTTON_JUMP) != 0;

    if (!want_jump) {
        ps->jump_held = false;
        ps->jump_buffer_ticks = 0;
        ps->autohop_cooldown = 0;   // releasing jump resets cooldown
        return;
    }

    // Fresh press while airborne: start the buffer countdown
    bool fresh_press = !ps->jump_held;
    ps->jump_held = true;

    if (fresh_press && !ps->on_ground) {
        ps->jump_buffer_ticks = QK_PM_JUMP_BUFFER_TICKS;
    }

    // Fire jump every frame the player is on ground and holding jump.
    // The buffer handles the air-press-then-land case automatically
    // since want_jump is still true when we land.
    if (ps->on_ground && want_jump) {
        // Autohop cooldown: held jump (not a fresh press) must wait.
        // Fresh presses always fire immediately (spam-to-bypass is fine).
        if (!fresh_press && ps->autohop_cooldown > 0) {
            return;
        }

        ps->jump_buffer_ticks = 0;
        // Q3 behavior: setting on_ground = false here means friction
        // is skipped on the jump frame, which is essential for consistent
        // strafejump speeds.
        ps->on_ground = false;

        // CPM double-jump: if jumping within the window after the LAST jump,
        // boost the impulse. last_jump_tick == 0 means fresh spawn, no boost.
        u32 since_jump = ps->command_time - ps->last_jump_tick;
        bool is_double = (ps->last_jump_tick > 0 &&
                          since_jump <= P_CPM_DOUBLE_JUMP_TICKS);

        f32 amount = is_double
            ? QK_PM_JUMP_VELOCITY + QK_PM_CPM_DOUBLE_JUMP_BOOST
            : QK_PM_JUMP_VELOCITY;

        // Additive impulse with floor: preserves upward momentum from
        // previous jumps (chain doublejumps), but never gives less than
        // a clean jump impulse.
        f32 added = ps->velocity.z + amount;
        ps->velocity.z = (added > amount) ? added : amount;

        // Timer starts/restarts on every valid jump (enables chaining)
        ps->last_jump_tick = ps->command_time;
        ps->autohop_cooldown = QK_PM_AUTOHOP_COOLDOWN_TICKS;
    }
}

// --- CPM air control (velocity rotation toward wish direction) ---

/*
 * CPM air control: rotate velocity toward wish direction without
 * changing speed.  Called whenever forward is held (W, W+A, W+D, etc.).
 *
 * Algorithm from CPMA (PM_CPMAirControl):
 * 1. Strip vertical velocity
 * 2. Normalize horizontal velocity, save speed
 * 3. Dot velocity direction with wish direction
 * 4. If dot > 0: rotate velocity toward wish by k = 32 * aircontrol * dot^2 * dt
 * 5. Re-normalize, restore speed and Z velocity
 *
 * The dot^2 factor makes turning strongest when nearly aligned (small
 * corrections) and weakest when perpendicular.  For W-only this is the
 * entire air mechanic (subtle adjustments).  For W+A/W+D it supplements
 * VQ3 strafejumping by helping maintain the optimal strafe angle.
 */

// Strength of W-turning rotation. Affects how "wide" the angular range
// from the velocity direction one can aim while still experiencing Free Turns(TM)
static const f32 P_CPM_AIRCONTROL_MULT = 150.0f;

// PQL-style forward-accel if airborne speed is low
static const f32 P_CPM_W_ONLY_ACCEL = 1.0f;

static void p_cpm_air_control(qk_player_state_t *ps, vec3_t wish_dir,
                               f32 wish_speed, f32 dt) {
    if (wish_speed < 0.0001f) return;

    f32 saved_z = ps->velocity.z;
    ps->velocity.z = 0.0f;

    f32 speed = vec3_length(ps->velocity);
    if (speed < 1.0f) speed = 1.0f;

    vec3_t vel_dir = vec3_scale(ps->velocity, 1.0f / speed);
    f32 dot = vec3_dot(vel_dir, wish_dir);

    // Only rotate toward wish direction (dot > 0), not away
    if (dot > 0.0f) {
        f32 k = 32.0f * P_CPM_AIRCONTROL_MULT * dot * dot * dt;

        // Blend velocity direction toward wish direction, then normalize
        // to maintain constant speed (pure rotation, no acceleration)
        vec3_t blended = vec3_add(vec3_scale(vel_dir, speed),
                                  vec3_scale(wish_dir, k));
        vel_dir = vec3_normalize(blended);
    }

    ps->velocity = vec3_scale(vel_dir, speed);
    ps->velocity.z = saved_z;
}

// --- CPM air movement (additive layers, matching real CPMA) ---

/*
 * CPM air movement: exclusive branches based on input combination.
 *
 * 1. W+A / W+D / S+A / S+D (diagonal): VQ3 strafejump
 *    Low accel (1.0), high wish_speed (320).  The 30-cap inside
 *    p_air_accelerate keeps the projection test tight while the full
 *    wish_speed drives the accel magnitude — classic strafejumping.
 *
 * 2. A / D only (strafe, no forward): CPM air strafing
 *    High accel (70), low wish_speed (30).  Tight responsive air
 *    curves without exponential VQ3 speed gain.
 *
 * 3. W / S only (forward, no strafe): CPM air control (W-turn)
 *    Rotates velocity toward wish direction without changing speed.
 *    No acceleration — purely directional adjustment.
 *
 * 4. No input: VQ3 air accel fallback (effectively a no-op at speed).
 */

static void p_cpm_air_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                           vec3_t wish_dir, f32 dt) {
    bool has_forward = (cmd->forward_move > 0.0001f || cmd->forward_move < -0.0001f);
    bool has_side    = (cmd->side_move > 0.0001f || cmd->side_move < -0.0001f);

    if (has_forward && has_side) {
        // W+A / W+D / S+A / S+D: strafejump.
        // Uses QK_PM_AIR_SPEED (~0.84 * max_speed) as wish speed, giving
        // a wide strafe window (~32 deg at ground speed) that narrows as
        // speed increases.  Same accelerate function as ground -- no
        // separate wishspeed cap.
        p_accelerate(ps, wish_dir, QK_PM_AIR_SPEED, QK_PM_AIR_ACCEL, dt);
    } else if (!has_forward && has_side) {
        // A/D only: CPM air strafing.
        // High accel (70), low wish_speed (30) -- gives tight, responsive
        // air curves without the exponential speed gain of strafejumping.
        p_accelerate(ps, wish_dir, QK_PM_CPM_WISH_SPEED,
                     QK_PM_CPM_STRAFE_ACCEL, dt);
    } else if (has_forward && !has_side) {
        // W/S only: CPM air control (W-turn).
        // Rotates velocity toward wish direction without changing speed.
        // No acceleration -- purely directional adjustment.
        p_cpm_air_control(ps, wish_dir, QK_PM_AIR_SPEED, dt);
    } else {
        // No movement input: air accel fallback
        p_accelerate(ps, wish_dir, QK_PM_AIR_SPEED, QK_PM_AIR_ACCEL, dt);
    }
}

// --- PM_Move: one physics tick ---

void p_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
            const qk_phys_world_t *world) {

    f32 dt = QK_TICK_DT;

    // Advance the tick counter (used for double-jump timing)
    ps->command_time++;

    // 1. Compute wish direction from yaw only (pitch must not affect
    // movement speed -- looking up/down should not slow acceleration)
    vec3_t forward, right, up;
    p_angle_vectors(0.0f, cmd->yaw, &forward, &right, &up);

    vec3_t wish_dir;
    wish_dir.x = forward.x * cmd->forward_move + right.x * cmd->side_move;
    wish_dir.y = forward.y * cmd->forward_move + right.y * cmd->side_move;
    wish_dir.z = 0.0f; // no vertical component from input

    // Normalize wish_dir to a pure direction; wish_speed is set per
    // movement context (ground vs air), NOT derived from the vector length.
    // This matches the reference model where direction and speed are
    // independent parameters to the accelerate function.
    f32 wish_len = vec3_length(wish_dir);
    if (wish_len > 0.0001f) {
        wish_dir = vec3_scale(wish_dir, 1.0f / wish_len);
    } else {
        wish_dir = (vec3_t){0.0f, 0.0f, 0.0f};
    }
    f32 wish_speed = (wish_len > 0.0001f) ? ps->max_speed : 0.0f;

    // 2. Check ground
    bool was_airborne = !ps->on_ground;
    p_categorize_position(ps, world);

    // 3. Jump check (includes CPM double-jump boost)
    p_check_jump(ps, cmd);

    // 3b. Decrement jump buffer while airborne
    if (!ps->on_ground && ps->jump_buffer_ticks > 0) {
        ps->jump_buffer_ticks--;
    }

    // 4. Apply friction (ground only, skip during rocket-jump slick
    // and stair-skim).  Stair-skim: during skim, if the player is
    // holding jump but autohop cooldown prevents it, they're stuck
    // on a stair tread for 1-2 ticks.  Suppress friction in that
    // exact case so stair traversal doesn't bleed speed.
    bool stair_skim = ps->skim_ticks > 0 &&
                      ps->jump_held &&
                      ps->autohop_cooldown > 0;
    if (ps->on_ground && ps->splash_slick_ticks == 0 && !stair_skim) {
        p_apply_friction(ps, dt);
    }

    // 5. Accelerate
    if (ps->on_ground) {
        // CPM: higher ground acceleration for snappier movement
        p_accelerate(ps, wish_dir, wish_speed, QK_PM_CPM_GROUND_ACCEL, dt);
    } else {
        // CPM air movement: dispatch based on input type
        p_cpm_air_move(ps, cmd, wish_dir, dt);
    }

    // 6. Apply gravity (air only)
    if (!ps->on_ground) {
        ps->velocity.z -= ps->gravity * dt;
    }

    // 7. Move and collide.
    // StepSlideMove is used for both ground AND air movement. This is
    // critical for stair traversal while holding jump: the player is
    // briefly airborne between hops and would otherwise hit the vertical
    // face of the next step without step-up kicking in. Q3 uses
    // PM_StepSlideMove in both code paths.
    vec3_t skim_saved_vel = ps->velocity;
    f32 pre_collision_vz = ps->velocity.z;

    p_step_slide_move(ps, world, dt);

    // 8. Re-check ground after move
    p_categorize_position(ps, world);

    // Clip velocity to ground plane while firmly grounded.
    // On flat ground (normal = 0,0,1) this zeroes Z as before.
    // On slopes, this projects velocity along the surface -- landing
    // on a downslope converts vertical speed to horizontal, and
    // jumping off an upramp deflects velocity upward.  Q3 does this
    // via PM_ClipVelocity(vel, groundplane.normal, OVERCLIP).
    if (ps->on_ground && ps->skim_ticks == 0) {
        ps->velocity = p_clip_velocity(ps->velocity, ps->ground_normal,
                                        QK_PM_OVERCLIP);
    }

    // Stair-glide override: while rising, don't let flat stair treads
    // ground us.  Skim handles velocity preservation for ~195ms, but
    // the upward phase of a regular jump lasts ~340ms -- after skim
    // expires, treads still ground the player and zero vz, consuming
    // the double-jump window partway up the staircase.  Restore
    // pre-collision vz (which already includes gravity) so the arc
    // continues naturally until the apex.
    //
    // ground_normal.z > 0.99 limits this to near-flat surfaces (stair
    // treads) so ramps with angled normals still interact normally.
    if (pre_collision_vz > 0.0f && ps->on_ground &&
        ps->ground_normal.z > 0.99f) {
        ps->on_ground = false;
        ps->velocity.z = pre_collision_vz;
    }

    // Skim velocity restore: preserve speed through wall clips.
    // Done after step 8 so on_ground reflects post-collision state.
    // Only restore z when airborne -- on ground, the collision response
    // properly zeroes z against the floor; restoring a stale negative z
    // causes vertical oscillation (floor vibration).
    if (ps->skim_ticks > 0) {
        ps->velocity.x = skim_saved_vel.x;
        ps->velocity.y = skim_saved_vel.y;
        if (!ps->on_ground) {
            ps->velocity.z = skim_saved_vel.z;
        }
    }

    // 8b. Skim activation: landing after meaningful airtime.
    // Checked after step 8 so on_ground is reliable regardless of
    // fall speed (old step 2b missed fast landings where the player
    // passed through the 0.25-unit ground-trace window in one tick).
    // Uses pre-collision velocity since floor collision clips z to 0.
    if (was_airborne && ps->on_ground && pre_collision_vz < -50.0f &&
        ps->ground_normal.z > 0.99f) {
        ps->skim_ticks = QK_PM_SKIM_TICKS;
    }

    // Slick period: don't snap to ground while moving upward (rocket jump)
    if (ps->splash_slick_ticks > 0 && ps->velocity.z > 0.0f) {
        ps->on_ground = false;
    }

    // 9. Decrement splash slick counter
    if (ps->splash_slick_ticks > 0) {
        ps->splash_slick_ticks--;
    }

    // 10. Decrement skim counter. The timer runs regardless of ground
    // state -- the whole point is that after landing, the player jumps
    // and skims wall corners while AIRBORNE during the skim window.
    if (ps->skim_ticks > 0) {
        ps->skim_ticks--;
    }

    // 11. Decrement autohop cooldown
    if (ps->autohop_cooldown > 0) {
        ps->autohop_cooldown--;
    }
}
