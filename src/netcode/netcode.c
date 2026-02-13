/*
 * QUICKEN Engine - Netcode Public API
 *
 * Thin wrappers around the internal n_server_t and n_client_t.
 * Global singleton instances. The engine owns exactly one server
 * and one client at any time.
 */

#include "netcode/n_internal.h"

/* ---- Global instances ---- */

static n_server_t s_server;
static n_client_t s_client;

/* ---- Server API ---- */

qk_result_t qk_net_server_init(const qk_net_server_config_t *config) {
    if (!config) return QK_ERROR_INVALID_PARAM;

    n_platform_init();

    u32 max_clients = config->max_clients;
    if (max_clients == 0) max_clients = N_MAX_CLIENTS;
    if (max_clients > N_MAX_CLIENTS) max_clients = N_MAX_CLIENTS;

    f64 tick_rate = config->tick_rate;
    if (tick_rate <= 0.0) tick_rate = (f64)N_TICK_RATE;

    n_server_init(&s_server, config->server_port, max_clients, tick_rate);

    if (!s_server.initialized) {
        return QK_ERROR_SOCKET;
    }

    return QK_SUCCESS;
}

void qk_net_server_tick(void) {
    n_server_tick(&s_server, n_platform_time());
}

void qk_net_server_shutdown(void) {
    n_server_shutdown(&s_server);
}

u32 qk_net_server_get_tick(void) {
    return s_server.tick;
}

u32 qk_net_server_client_count(void) {
    return s_server.client_count;
}

void qk_net_server_set_entity(u8 entity_id, const n_entity_state_t *state) {
    if (!state) return;
    n_snapshot_set_entity(&s_server.current_snapshot, entity_id, state);
}

void qk_net_server_remove_entity(u8 entity_id) {
    n_snapshot_remove_entity(&s_server.current_snapshot, entity_id);
}

bool qk_net_server_get_input(u8 client_id, qk_usercmd_t *out_cmd) {
    if (!out_cmd) return false;
    if (client_id >= s_server.max_clients) return false;

    n_client_slot_t *cl = &s_server.clients[client_id];
    if (cl->state != N_CONN_CONNECTED) return false;

    /* Get input for current server tick */
    u32 tick = s_server.tick;
    u32 idx = tick % N_INPUT_QUEUE_SIZE;
    n_input_t *input = &cl->input_queue[idx];

    /* Check if we have a valid input for this tick */
    bool have_input = (cl->last_input_tick >= tick);

    /* If no input available, repeat last known input */
    if (!have_input) {
        input = &cl->last_input;
    }

    /* Convert n_input_t to qk_usercmd_t */
    out_cmd->server_time = tick * (u32)(1000.0 / N_TICK_RATE);
    out_cmd->forward_move = (f32)input->forward_move / 127.0f;
    out_cmd->side_move = (f32)input->side_move / 127.0f;
    out_cmd->up_move = 0.0f;
    out_cmd->yaw = (f32)input->yaw * (360.0f / 65536.0f);
    out_cmd->pitch = (f32)input->pitch * (360.0f / 65536.0f);
    out_cmd->buttons = input->buttons;
    out_cmd->weapon_select = input->weapon_select;

    return true;
}

/* ---- Client API ---- */

qk_result_t qk_net_client_init(const qk_net_client_config_t *config) {
    if (!config) return QK_ERROR_INVALID_PARAM;

    n_platform_init();

    f64 interp_delay = config->interp_delay;
    n_client_init(&s_client, interp_delay);

    if (!s_client.initialized) {
        return QK_ERROR_INIT_FAILED;
    }

    return QK_SUCCESS;
}

qk_result_t qk_net_client_connect_remote(const char *address, u16 port) {
    if (!address || port == 0) return QK_ERROR_INVALID_PARAM;
    if (!s_client.initialized) return QK_ERROR_INIT_FAILED;

    n_client_connect_remote(&s_client, address, port);

    if (s_client.conn_state == N_CONN_DISCONNECTED) {
        return QK_ERROR_SOCKET;
    }

    return QK_SUCCESS;
}

qk_result_t qk_net_client_connect_local(void) {
    if (!s_client.initialized) return QK_ERROR_INIT_FAILED;
    if (!s_server.initialized) return QK_ERROR_INIT_FAILED;

    n_client_connect_local(&s_client, &s_server);

    if (s_client.conn_state == N_CONN_DISCONNECTED) {
        return QK_ERROR_FULL;
    }

    return QK_SUCCESS;
}

void qk_net_client_disconnect(void) {
    n_client_disconnect(&s_client);
}

void qk_net_client_tick(void) {
    n_client_tick(&s_client, n_platform_time());
}

void qk_net_client_interpolate(f64 render_time) {
    n_client_interpolate(&s_client, render_time);
}

void qk_net_client_shutdown(void) {
    n_client_shutdown(&s_client);
}

void qk_net_client_send_input(const qk_usercmd_t *cmd) {
    if (!cmd) return;

    /* Convert qk_usercmd_t to n_input_t */
    n_input_t input;
    input.forward_move = (i8)(cmd->forward_move * 127.0f);
    input.side_move = (i8)(cmd->side_move * 127.0f);
    input.yaw = (u16)(cmd->yaw * (65536.0f / 360.0f));
    input.pitch = (u16)(cmd->pitch * (65536.0f / 360.0f));
    input.buttons = (u16)cmd->buttons;
    input.weapon_select = cmd->weapon_select;

    n_client_send_input(&s_client, &input, n_platform_time());
}

const qk_interp_state_t *qk_net_client_get_interp_state(void) {
    return &s_client.interp_state;
}

qk_conn_state_t qk_net_client_get_state(void) {
    return (qk_conn_state_t)s_client.conn_state;
}

i32 qk_net_client_get_rtt(void) {
    return (i32)(s_client.clock.smoothed_rtt * 1000.0);
}

u8 qk_net_client_get_id(void) {
    return s_client.client_id;
}
