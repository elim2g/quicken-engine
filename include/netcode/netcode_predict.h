/*
 * QUICKEN Netcode - Server-Side Input Prediction
 *
 * When a client's input buffer runs dry, the server predicts inputs
 * rather than stalling. Other clients see smooth movement; only the
 * laggy player receives corrections.
 *
 * Prediction strategy varies by movement state:
 *   Grounded    -> repeat last input (linear movement, predictable)
 *   Airborne    -> zero directional input (preserves momentum vector)
 *   Crouchslide -> maintain crouch, zero directional (preserve slide)
 *
 * Zero-input airborne prediction is critical for CPM air strafing.
 * Repeating stale strafe inputs would curve the player in the wrong
 * direction. Zero input maintains the velocity tangent, which produces
 * a smaller correction when real inputs arrive.
 */

#ifndef QUICKEN_NETCODE_PREDICT_H
#define QUICKEN_NETCODE_PREDICT_H

#include "quicken.h"
#include "core/input.h"
#include "netcode/netcode_profile.h"

#define NETCODE_INPUT_BUFFER_SIZE 64  /* power of 2 for ring buffer masking */
#define NETCODE_INPUT_BUFFER_MASK (NETCODE_INPUT_BUFFER_SIZE - 1)

/*
 * Server-side per-client prediction state.
 *
 * At ~900 bytes per instance (dominated by the 64-slot input ring buffer),
 * allocate these in an array at server init — do not put on the stack in
 * recursive contexts. Expected usage:
 *
 *     netcode_client_pred_t clients[QUICKEN_MAX_PLAYERS];
 */
typedef struct netcode_client_pred {
    u32 client_id;

    /* Input ring buffer (jitter buffer) */
    quicken_input_t input_buf[NETCODE_INPUT_BUFFER_SIZE];
    u32 buf_write;
    u32 buf_read;

    /* Jitter estimation */
    f32 jitter_ms;
    u32 adapted_depth;

    /* Prediction tracking */
    u32 predicted_ticks;
    quicken_move_state_t move_state;
    quicken_input_t last_real;
    bool predicting;

    /* Deceleration during extended prediction */
    f32 speed_scale;        /* 1.0 = full speed, decays toward 0.0 */

    /* Correction blending (visual offset that decays to zero) */
    f32 correction_pos[3];
    f32 correction_progress;    /* 0.0 = start of blend, 1.0 = done */
    u32 correction_total_ticks;
} netcode_client_pred_t;

typedef struct netcode_input_result {
    quicken_input_t input;
    bool            was_predicted;
    f32             speed_scale;    /* 0.0 - 1.0, apply to movement */
} netcode_input_result_t;

void netcode_predict_init(netcode_client_pred_t *client, u32 client_id);
void netcode_predict_reset(netcode_client_pred_t *client);

void netcode_predict_buffer_input(netcode_client_pred_t *client,
                                  const quicken_input_t *input);
u32  netcode_predict_buffered_count(const netcode_client_pred_t *client);

/* Core: consume next input for this tick, predicting if buffer is empty */
netcode_input_result_t netcode_predict_consume(netcode_client_pred_t *client,
                                               const netcode_profile_t *profile,
                                               quicken_move_state_t current_move_state);

void netcode_predict_update_jitter(netcode_client_pred_t *client,
                                   const netcode_profile_t *profile,
                                   f32 observed_jitter_ms);

/* Correction blending after misprediction */
void netcode_predict_begin_correction(netcode_client_pred_t *client,
                                      const netcode_profile_t *profile,
                                      const f32 error_pos[3],
                                      quicken_move_state_t move_state);

void netcode_predict_tick_correction(netcode_client_pred_t *client,
                                     f32 out_offset[3]);

#endif /* QUICKEN_NETCODE_PREDICT_H */
