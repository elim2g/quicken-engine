/*
 * QUICKEN Netcode - Server-Side Input Prediction
 *
 * The server maintains a jitter buffer of inputs per client. Each tick
 * it consumes one input. If the buffer is empty, it predicts.
 *
 * Prediction strategy depends on the player's movement state:
 *
 *   GROUNDED:    Repeat last input. Ground movement is roughly linear
 *                and repeating is a good approximation for short gaps.
 *
 *   AIRBORNE:    Zero directional input. In Quake-style physics, zero
 *                air input preserves the velocity vector (no air drag
 *                on the movement axes). This is CRITICAL for CPM air
 *                strafing — repeating stale strafe keys would curve
 *                the player in the wrong direction, producing large
 *                corrections. Zero input maintains the tangent of their
 *                flight arc, minimizing prediction error.
 *
 *   CROUCHSLIDE: Maintain crouch, zero directional. Preserves slide
 *                momentum without steering into walls.
 *
 *   FALLING:     Same as airborne (walked off edge, no jump input).
 *
 * During the grace period (first N predicted ticks, configurable via
 * profile), we repeat the last input regardless of state. The gap is
 * short enough that repetition is a better bet than zeroing, even in air.
 *
 * After predict_decel_start ticks, the server begins decelerating the
 * predicted player to cap the maximum positional error. After
 * predict_max_ticks, the player is frozen until real inputs arrive.
 */

#include "netcode/netcode_predict.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static quicken_input_t make_zero_input(u32 tick)
{
    quicken_input_t input;
    memset(&input, 0, sizeof(input));
    input.tick = tick;
    return input;
}

/* ------------------------------------------------------------------ */
/* Init / Reset                                                        */
/* ------------------------------------------------------------------ */

void netcode_predict_init(netcode_client_pred_t *client, u32 client_id)
{
    memset(client, 0, sizeof(*client));
    client->client_id = client_id;
    client->speed_scale = 1.0f;
}

void netcode_predict_reset(netcode_client_pred_t *client)
{
    u32 id = client->client_id;
    netcode_predict_init(client, id);
}

/* ------------------------------------------------------------------ */
/* Input buffer                                                        */
/* ------------------------------------------------------------------ */

u32 netcode_predict_buffered_count(const netcode_client_pred_t *client)
{
    return client->buf_write - client->buf_read;
}

void netcode_predict_buffer_input(netcode_client_pred_t *client,
                                  const quicken_input_t *input)
{
    u32 count = netcode_predict_buffered_count(client);
    if (count >= NETCODE_INPUT_BUFFER_SIZE) {
        /* Buffer full — drop oldest. This shouldn't happen under normal
         * conditions; if it does, the client is flooding or our buffer
         * depth is too conservative. */
        client->buf_read++;
    }

    u32 idx = client->buf_write & NETCODE_INPUT_BUFFER_MASK;
    client->input_buf[idx] = *input;
    client->buf_write++;
}

/* ------------------------------------------------------------------ */
/* Core prediction                                                     */
/* ------------------------------------------------------------------ */

/*
 * Generate a predicted input based on the player's movement state.
 * Called when the jitter buffer is empty and grace period has expired.
 */
static quicken_input_t predict_for_state(const netcode_client_pred_t *client,
                                         quicken_move_state_t move_state,
                                         u32 tick)
{
    quicken_input_t pred;

    switch (move_state) {
    case QUICKEN_MOVE_GROUNDED:
        /* Ground: repeat last input — movement is linear enough */
        pred = client->last_real;
        pred.tick = tick;
        /* Clear jump so we don't re-trigger a jump */
        pred.buttons &= (u16)~QUICKEN_BUTTON_JUMP;
        break;

    case QUICKEN_MOVE_AIRBORNE:
    case QUICKEN_MOVE_FALLING:
        /* Air: zero directional input, preserve look direction.
         * In Quake physics: no air input = maintain velocity vector.
         * This is the tangent line of their strafe arc — far better
         * than continuing to curve with stale strafe inputs. */
        pred = make_zero_input(tick);
        pred.angles[0] = client->last_real.angles[0];
        pred.angles[1] = client->last_real.angles[1];
        pred.angles[2] = client->last_real.angles[2];
        break;

    case QUICKEN_MOVE_CROUCHSLIDE:
        /* Crouchslide: hold crouch to maintain slide, zero directional.
         * Steering a crouchslide requires precise input — guessing wrong
         * is worse than not steering at all. */
        pred = make_zero_input(tick);
        pred.angles[0] = client->last_real.angles[0];
        pred.angles[1] = client->last_real.angles[1];
        pred.angles[2] = client->last_real.angles[2];
        pred.buttons = QUICKEN_BUTTON_CROUCH;
        break;

    default:
        pred = make_zero_input(tick);
        break;
    }

    return pred;
}

netcode_input_result_t netcode_predict_consume(
    netcode_client_pred_t *client,
    const netcode_profile_t *profile,
    quicken_move_state_t current_move_state)
{
    netcode_input_result_t result;
    client->move_state = current_move_state;

    /* --- Real input available --- */
    if (netcode_predict_buffered_count(client) > 0) {
        u32 idx = client->buf_read & NETCODE_INPUT_BUFFER_MASK;
        result.input = client->input_buf[idx];
        client->buf_read++;

        client->last_real = result.input;
        client->predicted_ticks = 0;
        client->predicting = false;
        client->speed_scale = 1.0f;

        result.was_predicted = false;
        result.speed_scale = 1.0f;
        return result;
    }

    /* --- Must predict --- */
    client->predicted_ticks++;
    client->predicting = true;

    u32 pred_tick = client->last_real.tick + client->predicted_ticks;

    /*
     * Grace period: repeat last input verbatim regardless of move state.
     * The gap is short (1-3 ticks = 8-24ms at 128Hz) so repetition is
     * the best approximation — even for air strafing, because a player's
     * input doesn't change much over 1-2 ticks.
     */
    quicken_input_t predicted;
    if (client->predicted_ticks <= profile->predict_grace_ticks) {
        predicted = client->last_real;
        predicted.tick = pred_tick;
        predicted.buttons &= (u16)~QUICKEN_BUTTON_JUMP;
    } else {
        predicted = predict_for_state(client, current_move_state, pred_tick);
    }

    /* Deceleration: after enough predicted ticks, bleed speed to cap
     * maximum positional error when real inputs finally arrive. */
    if (client->predicted_ticks >= profile->predict_decel_start) {
        client->speed_scale *= (1.0f - profile->predict_decel_rate);
        if (client->speed_scale < 0.01f) {
            client->speed_scale = 0.0f;
        }
    }

    /* Hard freeze: beyond max prediction depth, stop the player */
    if (client->predicted_ticks >= profile->predict_max_ticks) {
        predicted.forward = 0;
        predicted.side = 0;
        predicted.buttons = 0;
        client->speed_scale = 0.0f;
    }

    result.input = predicted;
    result.was_predicted = true;
    result.speed_scale = client->speed_scale;
    return result;
}

/* ------------------------------------------------------------------ */
/* Jitter estimation                                                   */
/* ------------------------------------------------------------------ */

void netcode_predict_update_jitter(netcode_client_pred_t *client,
                                   const netcode_profile_t *profile,
                                   f32 observed_jitter_ms)
{
    /* Exponential moving average */
    f32 rate = profile->jitter_adapt_rate;
    client->jitter_ms = client->jitter_ms * (1.0f - rate)
                      + observed_jitter_ms * rate;

    /* Adapt buffer depth: convert jitter estimate to ticks,
     * add 1 tick of headroom, clamp to profile range. */
    u32 needed = (u32)(client->jitter_ms / QUICKEN_TICK_MS) + 1;

    if (needed < profile->jitter_buf_min) needed = profile->jitter_buf_min;
    if (needed > profile->jitter_buf_max) needed = profile->jitter_buf_max;

    client->adapted_depth = needed;
}

/* ------------------------------------------------------------------ */
/* Correction blending                                                 */
/* ------------------------------------------------------------------ */

/*
 * After the server detects a misprediction (predicted state != server
 * authoritative state), the client's authoritative position snaps to
 * the correct value. To hide the visual pop, we store the positional
 * error and blend it out over several ticks.
 *
 * error_pos = predicted_position - authoritative_position
 *
 * Each tick, the visual offset decays linearly from error_pos to zero.
 * The renderer adds this offset to the authoritative position, creating
 * a smooth visual transition.
 */

void netcode_predict_begin_correction(netcode_client_pred_t *client,
                                      const netcode_profile_t *profile,
                                      const f32 error_pos[3],
                                      quicken_move_state_t move_state)
{
    /* Squared distance to avoid sqrt */
    f32 dist_sq = error_pos[0] * error_pos[0]
                + error_pos[1] * error_pos[1]
                + error_pos[2] * error_pos[2];

    f32 small_sq = profile->correct_small_dist * profile->correct_small_dist;
    f32 large_sq = profile->correct_large_dist * profile->correct_large_dist;

    u32 blend_ticks;
    if (dist_sq <= small_sq) {
        blend_ticks = profile->correct_small_ticks;
    } else if (dist_sq <= large_sq) {
        blend_ticks = profile->correct_medium_ticks;
    } else {
        blend_ticks = 1;  /* snap — error is too large to blend */
    }

    /* Longer blend window while airborne — mid-air snaps are far more
     * visually jarring than ground-level corrections. */
    if (move_state == QUICKEN_MOVE_AIRBORNE ||
        move_state == QUICKEN_MOVE_FALLING) {
        f32 scaled = (f32)blend_ticks * profile->correct_air_mult;
        blend_ticks = (u32)(scaled + 0.5f);
    }

    if (blend_ticks < 1) blend_ticks = 1;

    client->correction_pos[0] = error_pos[0];
    client->correction_pos[1] = error_pos[1];
    client->correction_pos[2] = error_pos[2];
    client->correction_progress = 0.0f;
    client->correction_total_ticks = blend_ticks;
}

void netcode_predict_tick_correction(netcode_client_pred_t *client,
                                     f32 out_offset[3])
{
    if (client->correction_total_ticks == 0 ||
        client->correction_progress >= 1.0f) {
        out_offset[0] = 0.0f;
        out_offset[1] = 0.0f;
        out_offset[2] = 0.0f;
        return;
    }

    f32 step = 1.0f / (f32)client->correction_total_ticks;
    client->correction_progress += step;
    if (client->correction_progress > 1.0f) {
        client->correction_progress = 1.0f;
    }

    /* Linear decay: visual offset shrinks from full error to zero */
    f32 remaining = 1.0f - client->correction_progress;
    out_offset[0] = client->correction_pos[0] * remaining;
    out_offset[1] = client->correction_pos[1] * remaining;
    out_offset[2] = client->correction_pos[2] * remaining;
}
