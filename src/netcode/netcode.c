/*
 * QUICKEN Engine - Netcode Public API
 *
 * Thin wrappers around the internal n_server_t and n_client_t.
 * Heap-allocated singleton instances. The engine owns exactly one server
 * and one client at any time.
 */

#include "n_internal.h"
#include "gameplay/qk_gameplay.h"
#include "core/qk_demo.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef QUICKEN_DEBUG
#define N_DBG(fmt, ...) fprintf(stderr, "[NET] " fmt "\n", ##__VA_ARGS__)
#else
#define N_DBG(fmt, ...) ((void)0)
#endif

/* ---- Global instances (heap-allocated to avoid MB-scale BSS) ---- */

static n_server_t *s_server;
static n_client_t *s_client;

/* ---- Server API ---- */

qk_result_t qk_net_server_init(const qk_net_server_config_t *config) {
    if (!config) return QK_ERROR_INVALID_PARAM;

    n_platform_init();

    if (!s_server) {
        s_server = (n_server_t *)calloc(1, sizeof(n_server_t));
        if (!s_server) return QK_ERROR_INIT_FAILED;
    }

    u32 max_clients = config->max_clients;
    if (max_clients == 0) max_clients = N_MAX_CLIENTS;
    if (max_clients > N_MAX_CLIENTS) max_clients = N_MAX_CLIENTS;

    f64 tick_rate = config->tick_rate;
    if (tick_rate <= 0.0) tick_rate = (f64)N_TICK_RATE;

    n_server_init(s_server, config->server_port, max_clients, tick_rate);

    if (!s_server->initialized) {
        return QK_ERROR_SOCKET;
    }

    return QK_SUCCESS;
}

void qk_net_server_tick(void) {
    if (!s_server) return;
    n_server_tick(s_server, n_platform_time());
}

void qk_net_server_shutdown(void) {
    if (!s_server) return;
    n_server_shutdown(s_server);
    free(s_server);
    s_server = NULL;
}

u32 qk_net_server_get_tick(void) {
    return s_server ? s_server->tick : 0;
}

u32 qk_net_server_client_count(void) {
    return s_server ? s_server->client_count : 0;
}

void qk_net_server_set_entity(u8 entity_id, const n_entity_state_t *state) {
    if (!state || !s_server) return;
    n_snapshot_set_entity(&s_server->current_snapshot, entity_id, state);
}

void qk_net_server_remove_entity(u8 entity_id) {
    if (!s_server) return;
    n_snapshot_remove_entity(&s_server->current_snapshot, entity_id);
}

bool qk_net_server_get_input(u8 client_id, qk_usercmd_t *out_cmd) {
    if (!out_cmd || !s_server) return false;
    if (client_id >= s_server->max_clients) return false;

    n_client_slot_t *cl = &s_server->clients[client_id];
    if (cl->state != N_CONN_CONNECTED) return false;

    /* Get input for current server tick */
    u32 tick = s_server->tick;
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

qk_conn_state_t qk_net_server_get_client_state(u8 client_id) {
    if (!s_server) return QK_CONN_DISCONNECTED;
    if (client_id >= s_server->max_clients) return QK_CONN_DISCONNECTED;
    return (qk_conn_state_t)s_server->clients[client_id].state;
}

bool qk_net_server_is_client_map_ready(u8 client_id) {
    if (!s_server) return false;
    if (client_id >= s_server->max_clients) return false;
    n_client_slot_t *cl = &s_server->clients[client_id];
    return cl->state == N_CONN_CONNECTED && cl->map_ready;
}

/* ---- Client API ---- */

qk_result_t qk_net_client_init(const qk_net_client_config_t *config) {
    if (!config) return QK_ERROR_INVALID_PARAM;

    n_platform_init();

    if (!s_client) {
        s_client = (n_client_t *)calloc(1, sizeof(n_client_t));
        if (!s_client) return QK_ERROR_INIT_FAILED;
    }

    f64 interp_delay = config->interp_delay;
    n_client_init(s_client, interp_delay);

    if (!s_client->initialized) {
        return QK_ERROR_INIT_FAILED;
    }

    return QK_SUCCESS;
}

qk_result_t qk_net_client_connect_remote(const char *address, u16 port) {
    if (!address || port == 0) return QK_ERROR_INVALID_PARAM;
    if (!s_client || !s_client->initialized) return QK_ERROR_INIT_FAILED;

    n_client_connect_remote(s_client, address, port);

    if (s_client->conn_state == N_CONN_DISCONNECTED) {
        return QK_ERROR_SOCKET;
    }

    return QK_SUCCESS;
}

qk_result_t qk_net_client_connect_local(void) {
    if (!s_client || !s_client->initialized) return QK_ERROR_INIT_FAILED;
    if (!s_server || !s_server->initialized) return QK_ERROR_INIT_FAILED;

    n_client_connect_local(s_client, s_server);

    if (s_client->conn_state == N_CONN_DISCONNECTED) {
        N_DBG("connect_local: FAILED (server full or no slot)");
        return QK_ERROR_FULL;
    }

    N_DBG("connect_local: OK client_id=%u", (u32)s_client->client_id);
    return QK_SUCCESS;
}

void qk_net_client_disconnect(void) {
    if (!s_client) return;
    n_client_disconnect(s_client);
}

void qk_net_client_tick(void) {
    if (!s_client) return;
    n_client_tick(s_client, n_platform_time());
}

void qk_net_client_interpolate(f64 render_time) {
    if (!s_client) return;
    n_client_interpolate(s_client, render_time);
}

void qk_net_client_shutdown(void) {
    if (!s_client) return;
    n_client_shutdown(s_client);
    free(s_client);
    s_client = NULL;
}

void qk_net_client_send_input(const qk_usercmd_t *cmd) {
    if (!cmd || !s_client) return;

    /* Convert qk_usercmd_t to n_input_t */
    n_input_t input;
    input.forward_move = (i8)(cmd->forward_move * 127.0f);
    input.side_move = (i8)(cmd->side_move * 127.0f);
    input.yaw = (u16)(cmd->yaw * (65536.0f / 360.0f));
    input.pitch = (u16)(cmd->pitch * (65536.0f / 360.0f));
    input.buttons = (u16)cmd->buttons;
    input.weapon_select = cmd->weapon_select;

    n_client_send_input(s_client, &input, n_platform_time());

    /* Demo recording hook */
    if (qk_demo_is_recording()) {
        qk_demo_record_usercmd(s_client->input_tick - 1, cmd);
    }
}

const qk_interp_state_t *qk_net_client_get_interp_state(void) {
    return s_client ? &s_client->interp_state : NULL;
}

const qk_interp_diag_t *qk_net_client_get_interp_diag(void) {
    return s_client ? &s_client->interp_diag : NULL;
}

qk_conn_state_t qk_net_client_get_state(void) {
    return s_client ? (qk_conn_state_t)s_client->conn_state : QK_CONN_DISCONNECTED;
}

i32 qk_net_client_get_rtt(void) {
    return s_client ? (i32)(s_client->clock.smoothed_rtt * 1000.0) : 0;
}

u8 qk_net_client_get_id(void) {
    return s_client ? s_client->client_id : 0;
}

u32 qk_net_client_get_input_sequence(void) {
    return s_client ? s_client->input_tick : 0;
}

u32 qk_net_client_get_server_cmd_ack(void) {
    return s_client ? s_client->last_server_cmd_ack : 0;
}

void qk_net_client_inject_demo_snapshot(u32 tick, u32 entity_count,
                                         const u64 *entity_mask,
                                         const n_entity_state_t *entities) {
    if (!s_client) return;

    u32 write_idx = s_client->interp_write;
    n_snapshot_t *dest = &s_client->interp_snapshots[write_idx];

    dest->tick = tick;
    dest->entity_count = entity_count;
    memcpy(dest->entity_mask, entity_mask, sizeof(dest->entity_mask));
    memcpy(dest->entities, entities, sizeof(dest->entities));

    s_client->interp_write = (write_idx + 1) % N_INTERP_BUFFER_SIZE;
    if (s_client->interp_count < N_INTERP_BUFFER_SIZE) {
        s_client->interp_count++;
    }

    s_client->baseline_snapshot = *dest;
    s_client->has_baseline = true;
}

bool qk_net_client_get_server_player_state(qk_player_state_t *out) {
    if (!out || !s_client) return false;
    if (s_client->conn_state != N_CONN_CONNECTED) return false;

    /* Loopback: read directly from gameplay module (authoritative) */
    if (s_client->is_loopback) {
        const qk_player_state_t *ps = qk_game_get_player_state(s_client->client_id);
        if (!ps) return false;
        *out = *ps;
        return true;
    }

    /* Remote: extract from latest snapshot */
    if (s_client->interp_count == 0) return false;

    u32 idx = (s_client->interp_write - 1 + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
    const n_snapshot_t *snap = &s_client->interp_snapshots[idx];

    u8 eid = s_client->client_id;
    if (!n_snapshot_has_entity(snap, eid)) return false;

    const n_entity_state_t *e = &snap->entities[eid];

    /* Zero-init sets physics-internal fields (last_land_tick, skim_ticks,
     * jump_buffer_ticks, etc.) to 0.  These are not transmitted on the wire;
     * the physics engine reconstructs them during prediction input replay.
     * For CPM double-jump: last_land_tick=0 means "no recent landing",
     * which is the safe default -- a brief misprediction is corrected on the
     * next server snapshot. */
    memset(out, 0, sizeof(*out));
    out->origin.x = (f32)e->pos_x * 0.5f;
    out->origin.y = (f32)e->pos_y * 0.5f;
    out->origin.z = (f32)e->pos_z * 0.5f;
    out->velocity.x = (f32)e->vel_x;
    out->velocity.y = (f32)e->vel_y;
    out->velocity.z = (f32)e->vel_z;
    out->on_ground = (e->flags & QK_ENT_FLAG_ON_GROUND) != 0;
    out->jump_held = (e->flags & QK_ENT_FLAG_JUMP_HELD) != 0;
    out->mins = QK_PLAYER_MINS;
    out->maxs = QK_PLAYER_MAXS;
    out->max_speed = QK_PM_MAX_SPEED;
    out->gravity = QK_PM_GRAVITY;
    out->health = (i16)e->health;
    out->armor = (i16)e->armor;
    out->weapon = (qk_weapon_id_t)e->weapon;

    return true;
}

/* ---- Map handshake ---- */

void qk_net_client_notify_map_loaded(const char *map_name) {
    if (!s_client) return;
    if (s_client->conn_state != N_CONN_CONNECTED) return;

    /* For loopback, fast-track: both sides share the same process */
    if (s_client->is_loopback) {
        s_client->map_ready = true;
        if (s_server) {
            n_client_slot_t *slot = &s_server->clients[s_client->client_id];
            slot->map_ready = true;
            slot->last_acked_snapshot_tick = 0;
        }
        N_DBG("map_loaded: loopback fast-track (map=%s)", map_name ? map_name : "NULL");
        return;
    }

    /* Remote: send N_MSG_MAP_LOADED to server */
    u32 map_hash = n_hash_map_name(map_name);

    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = s_client->outgoing_sequence++;
    hdr.ack = s_client->incoming_sequence;
    hdr.ack_bitfield = s_client->ack_bitfield;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_MAP_LOADED, 4);
    n_write_u32(&w, map_hash);
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_transport_send(&s_client->transport, &s_client->server_address, pkt, total);
    s_client->stats.packets_sent++;

    N_DBG("map_loaded: sent hash=0x%08x (map=%s)", map_hash, map_name ? map_name : "NULL");
}

bool qk_net_client_is_map_ready(void) {
    if (!s_client) return false;
    return s_client->map_ready;
}

void qk_net_server_set_map(const char *map_name) {
    if (!s_server) return;
    s_server->map_name_hash = n_hash_map_name(map_name);

    /* Store map name for CONNECT_ACCEPTED (tells joining clients which map to load) */
    if (map_name) {
        size_t len = strlen(map_name);
        if (len >= sizeof(s_server->map_name)) len = sizeof(s_server->map_name) - 1;
        memcpy(s_server->map_name, map_name, len);
        s_server->map_name[len] = '\0';
    } else {
        s_server->map_name[0] = '\0';
    }

    N_DBG("server_set_map: hash=0x%08x (map=%s)",
          s_server->map_name_hash, map_name ? map_name : "NULL");

    /* Invalidate all clients' map_ready state (forces re-handshake on map change) */
    for (u32 i = 0; i < s_server->max_clients; i++) {
        s_server->clients[i].map_ready = false;
    }
}

const char *qk_net_client_get_server_map(void) {
    if (!s_client) return NULL;
    if (s_client->conn_state != N_CONN_CONNECTED) return NULL;
    if (s_client->server_map_name[0] == '\0') return NULL;
    return s_client->server_map_name;
}
