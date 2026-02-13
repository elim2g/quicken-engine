/*
 * QUICKEN Netcode Module - Stub Implementation
 *
 * All functions return success/zero/no-op.
 * The netcode agent replaces this with real code on feat/netcode.
 */

#include "netcode/qk_netcode.h"

/* ---- Server API stubs ---- */

qk_result_t qk_net_server_init(const qk_net_server_config_t *config) {
    QK_UNUSED(config);
    return QK_SUCCESS;
}

void qk_net_server_tick(void) {}

void qk_net_server_shutdown(void) {}

u32 qk_net_server_get_tick(void) { return 0; }

u32 qk_net_server_client_count(void) { return 0; }

void qk_net_server_set_entity(u8 entity_id, const n_entity_state_t *state) {
    QK_UNUSED(entity_id); QK_UNUSED(state);
}

void qk_net_server_remove_entity(u8 entity_id) {
    QK_UNUSED(entity_id);
}

bool qk_net_server_get_input(u8 client_id, qk_usercmd_t *out_cmd) {
    QK_UNUSED(client_id); QK_UNUSED(out_cmd);
    return false;
}

/* ---- Client API stubs ---- */

qk_result_t qk_net_client_init(const qk_net_client_config_t *config) {
    QK_UNUSED(config);
    return QK_SUCCESS;
}

qk_result_t qk_net_client_connect_remote(const char *address, u16 port) {
    QK_UNUSED(address); QK_UNUSED(port);
    return QK_SUCCESS;
}

qk_result_t qk_net_client_connect_local(void) {
    return QK_SUCCESS;
}

void qk_net_client_disconnect(void) {}

void qk_net_client_tick(void) {}

void qk_net_client_interpolate(f64 render_time) {
    QK_UNUSED(render_time);
}

void qk_net_client_shutdown(void) {}

void qk_net_client_send_input(const qk_usercmd_t *cmd) {
    QK_UNUSED(cmd);
}

static qk_interp_state_t s_empty_interp = {0};

const qk_interp_state_t *qk_net_client_get_interp_state(void) {
    return &s_empty_interp;
}

qk_conn_state_t qk_net_client_get_state(void) {
    return QK_CONN_DISCONNECTED;
}

i32 qk_net_client_get_rtt(void) { return 0; }

u8 qk_net_client_get_id(void) { return 0; }
