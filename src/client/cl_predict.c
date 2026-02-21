/*
 * QUICKEN Engine - Client Prediction
 */

#include "client/cl_predict.h"

#include <string.h>

#include "qk_types.h"
#include "core/qk_input.h"
#include "physics/qk_physics.h"
#include "netcode/qk_netcode.h"

// --- Prediction Ring Buffer ---

#define CL_CMD_BUFFER_SIZE 128

typedef struct {
    qk_usercmd_t cmd;
    u32 sequence;
} cl_stored_cmd_t;

typedef struct {
    qk_player_state_t state;
    u32 sequence;
} cl_predicted_state_t;

static cl_stored_cmd_t      s_cmd_buffer[CL_CMD_BUFFER_SIZE];
static cl_predicted_state_t s_pred_history[CL_CMD_BUFFER_SIZE];
static qk_player_state_t   s_predicted_ps;
static u32                  s_cmd_sequence;
static bool                 s_has_prediction;
static f32                  s_accumulator;
static u32                  s_last_reconciled_ack;

// --- Init / Reset ---

void cl_predict_init(void) {
    cl_predict_reset();
}

void cl_predict_reset(void) {
    memset(s_cmd_buffer, 0, sizeof(s_cmd_buffer));
    memset(s_pred_history, 0, sizeof(s_pred_history));
    memset(&s_predicted_ps, 0, sizeof(s_predicted_ps));
    s_cmd_sequence = 0;
    s_has_prediction = false;
    s_accumulator = 0.0f;
    s_last_reconciled_ack = 0;
}

// --- Prediction Tick ---

void cl_predict_tick(const qk_input_state_t *input,
                      qk_phys_world_t *world, f32 dt, bool is_remote) {
    s_accumulator += dt;

    while (s_accumulator >= QK_TICK_DT) {
        u32 server_time = is_remote
            ? s_cmd_sequence * QK_TICK_DT_MS_NOM
            : qk_net_server_get_tick() * QK_TICK_DT_MS_NOM;

        qk_usercmd_t cmd = qk_input_build_usercmd(input, server_time);

        u32 cmd_idx = s_cmd_sequence % CL_CMD_BUFFER_SIZE;
        s_cmd_buffer[cmd_idx].cmd = cmd;
        s_cmd_buffer[cmd_idx].sequence = s_cmd_sequence;

        qk_net_client_send_input(&cmd);

        if (!s_has_prediction) {
            qk_player_state_t srv_state;
            if (qk_net_client_get_server_player_state(&srv_state)) {
                s_predicted_ps = srv_state;
                s_has_prediction = true;
            }
        }

        if (s_has_prediction) {
            qk_physics_move(&s_predicted_ps, &cmd, world);
            s_pred_history[cmd_idx].state = s_predicted_ps;
            s_pred_history[cmd_idx].sequence = s_cmd_sequence;
        }

        s_cmd_sequence++;
        s_accumulator -= QK_TICK_DT;
    }
}

// --- Reconciliation ---

void cl_predict_reconcile(qk_phys_world_t *world) {
    u32 ack = qk_net_client_get_server_cmd_ack();
    if (ack <= s_last_reconciled_ack || !s_has_prediction) return;

    qk_player_state_t server_state;
    if (!qk_net_client_get_server_player_state(&server_state)) return;

    // Find predicted state for ack_sequence
    u32 ack_idx = ack % CL_CMD_BUFFER_SIZE;
    cl_predicted_state_t *predicted = &s_pred_history[ack_idx];
    if (predicted->sequence != ack) {
        s_last_reconciled_ack = ack;
        return;
    }

    // Always sync authoritative gameplay state from server
    // (weapon, ammo, health, alive_state, etc.) so changes
    // are visible immediately even when standing still.
    s_predicted_ps.weapon          = server_state.weapon;
    s_predicted_ps.pending_weapon  = server_state.pending_weapon;
    s_predicted_ps.weapon_time     = server_state.weapon_time;
    s_predicted_ps.switch_time     = server_state.switch_time;
    memcpy(s_predicted_ps.ammo, server_state.ammo, sizeof(server_state.ammo));
    s_predicted_ps.health          = server_state.health;
    s_predicted_ps.armor           = server_state.armor;
    s_predicted_ps.alive_state     = server_state.alive_state;
    s_predicted_ps.frags           = server_state.frags;
    s_predicted_ps.deaths          = server_state.deaths;
    s_predicted_ps.damage_given    = server_state.damage_given;
    s_predicted_ps.damage_taken    = server_state.damage_taken;
    s_predicted_ps.respawn_time    = server_state.respawn_time;

    // Compare positions (epsilon = 0.1 units squared)
    f32 delta_x = server_state.origin.x - predicted->state.origin.x;
    f32 delta_y = server_state.origin.y - predicted->state.origin.y;
    f32 delta_z = server_state.origin.z - predicted->state.origin.z;
    f32 dist_sq = delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;

    // Detect teleport: snap input angles to server-provided view direction
    if (server_state.teleport_bit != s_predicted_ps.teleport_bit) {
        qk_input_set_angles(server_state.pitch, server_state.yaw);
    }

    if (dist_sq < 0.1f) {
        s_last_reconciled_ack = ack;
        return;  // Position matches, no replay needed
    }

    // Position misprediction: snap to server state and replay
    s_predicted_ps = server_state;

    for (u32 seq = ack + 1; seq < s_cmd_sequence; seq++) {
        u32 idx = seq % CL_CMD_BUFFER_SIZE;
        cl_stored_cmd_t *stored = &s_cmd_buffer[idx];
        if (stored->sequence != seq) break;

        qk_physics_move(&s_predicted_ps, &stored->cmd, world);

        s_pred_history[idx].state = s_predicted_ps;
        s_pred_history[idx].sequence = seq;
    }

    s_last_reconciled_ack = ack;
}

// --- Getters ---

const qk_player_state_t *cl_predict_get_state(void) {
    return s_has_prediction ? &s_predicted_ps : NULL;
}

qk_player_state_t *cl_predict_get_state_mut(void) {
    return &s_predicted_ps;
}

bool cl_predict_has_state(void) {
    return s_has_prediction;
}

f32 cl_predict_get_accumulator(void) {
    return s_accumulator;
}

u32 cl_predict_get_cmd_sequence(void) {
    return s_cmd_sequence;
}
