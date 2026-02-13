/*
 * QUICKEN Engine - Client
 *
 * Connects to a server (local or remote), receives snapshots,
 * manages interpolation buffer, sends inputs, handles clock sync.
 */

#include "netcode/n_internal.h"
#include <math.h>

#ifdef QK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

/* ---- Helpers ---- */

static f64 client_server_time(const n_client_t *cl) {
    return n_platform_time() + cl->clock.smoothed_offset;
}

static u32 client_server_tick(const n_client_t *cl) {
    f64 st = client_server_time(cl);
    if (st < 0.0) return 0;
    return (u32)(st * (f64)N_TICK_RATE);
}

static f64 client_render_time(const n_client_t *cl) {
    return client_server_time(cl) - cl->interp_delay;
}

/* ---- Lifecycle ---- */

void n_client_init(n_client_t *cl, f64 interp_delay) {
    memset(cl, 0, sizeof(*cl));

    cl->conn_state = N_CONN_DISCONNECTED;
    cl->transport.socket_fd = -1;
    n_reliable_init(&cl->reliable);
    n_clock_init(&cl->clock);

    if (interp_delay > 0.0) {
        if (interp_delay < N_INTERP_DELAY_MIN) interp_delay = N_INTERP_DELAY_MIN;
        if (interp_delay > N_INTERP_DELAY_MAX) interp_delay = N_INTERP_DELAY_MAX;
        cl->interp_delay = interp_delay;
    } else {
        cl->interp_delay = N_INTERP_DELAY_DEFAULT;
    }

    cl->initialized = true;
}

void n_client_shutdown(n_client_t *cl) {
    if (!cl->initialized) return;
    if (cl->conn_state != N_CONN_DISCONNECTED) {
        n_client_disconnect(cl);
    }
    cl->initialized = false;
}

/* ---- Connection ---- */

/* Helper to reset a slot for loopback without losing the client_id */
static void slot_reset_for_loopback(n_client_slot_t *slot, u8 id) {
    memset(slot, 0, sizeof(*slot));
    slot->state = N_CONN_DISCONNECTED;
    slot->client_id = id;
    n_reliable_init(&slot->reliable);
}

void n_client_connect_remote(n_client_t *cl, const char *address, u16 port) {
    if (cl->conn_state != N_CONN_DISCONNECTED) return;

    n_platform_init();

    /* Parse IP address */
    struct in_addr addr;
    if (inet_pton(AF_INET, address, &addr) != 1) return;

    cl->server_address.ip = ntohl(addr.s_addr);
    cl->server_address.port = port;

    /* Open a UDP socket with OS-assigned port */
    if (!n_transport_open_udp(&cl->transport, 0)) {
        return;
    }

    cl->is_loopback = false;
    cl->conn_state = N_CONN_CONNECTING;
    cl->client_challenge = n_random_u32();
    cl->connect_start_time = n_platform_time();
    cl->last_connect_retry_time = 0.0;

    /* Send initial connect request */
    n_client_tick(cl, n_platform_time());
}

void n_client_connect_local(n_client_t *cl, n_server_t *srv) {
    if (cl->conn_state != N_CONN_DISCONNECTED) return;

    cl->loopback_server = srv;
    cl->is_loopback = true;

    /* Allocate a loopback slot on the server */
    i32 slot = n_server_allocate_slot(srv);
    if (slot < 0) return;

    n_client_slot_t *server_slot = &srv->clients[slot];
    slot_reset_for_loopback(server_slot, (u8)slot);

    /* Set up cross-wired loopback transports */
    n_transport_open_loopback(&cl->transport, &server_slot->transport,
                              &srv->loopback_queues[slot][0],
                              &srv->loopback_queues[slot][1]);

    server_slot->state = N_CONN_CONNECTING;
    server_slot->is_loopback = true;
    server_slot->last_packet_recv_time = n_platform_time();
    server_slot->connect_start_time = n_platform_time();

    cl->conn_state = N_CONN_CONNECTING;
    cl->client_challenge = n_random_u32();
    cl->connect_start_time = n_platform_time();
    cl->last_connect_retry_time = 0.0;

    /* The handshake will run through the normal tick path */
}

void n_client_disconnect(n_client_t *cl) {
    if (cl->conn_state == N_CONN_DISCONNECTED) return;

    /* Send disconnect message */
    if (cl->conn_state == N_CONN_CONNECTED) {
        u8 pkt[N_TRANSPORT_MTU];
        n_packet_header_t hdr = {0};
        hdr.sequence = cl->outgoing_sequence++;
        hdr.ack = cl->incoming_sequence;
        hdr.ack_bitfield = cl->ack_bitfield;
        n_packet_header_write(pkt, &hdr);

        n_bitwriter_t w;
        n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                         N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
        n_msg_header_write(&w, N_MSG_DISCONNECT, 0);
        n_msg_header_write(&w, N_MSG_NOP, 0);

        u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
        n_transport_send(&cl->transport, &cl->server_address, pkt, total);
    }

    n_transport_close(&cl->transport);
    cl->conn_state = N_CONN_DISCONNECTED;
    cl->has_baseline = false;
    cl->interp_count = 0;
    cl->interp_write = 0;
    n_clock_init(&cl->clock);
}

/* ---- Send connect request ---- */

static void send_connect_request(n_client_t *cl) {
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_CONNECT_REQUEST, 4);
    n_write_u32(&w, cl->client_challenge);
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_transport_send(&cl->transport, &cl->server_address, pkt, total);
    cl->stats.packets_sent++;
}

static void send_connect_response(n_client_t *cl) {
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_CONNECT_RESPONSE, 8);
    n_write_u32(&w, cl->server_challenge);
    n_write_u32(&w, cl->client_challenge);
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_transport_send(&cl->transport, &cl->server_address, pkt, total);
    cl->stats.packets_sent++;
}

/* ---- Send clock sync probe ---- */

static void send_clock_sync(n_client_t *cl) {
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    hdr.ack = cl->incoming_sequence;
    hdr.ack_bitfield = cl->ack_bitfield;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_CLOCK_SYNC, 8);

    f64 now = n_platform_time();
    cl->clock_sync_send_time = now;
    n_write_f64(&w, now);
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_transport_send(&cl->transport, &cl->server_address, pkt, total);
    n_clock_mark_sent(&cl->clock, now);
    cl->stats.packets_sent++;
}

/* ---- Packet processing ---- */

static void update_client_ack_bitfield(n_client_t *cl, u16 remote_seq) {
    if (cl->incoming_sequence == 0 && cl->ack_bitfield == 0) {
        cl->incoming_sequence = remote_seq;
        return;
    }

    if (n_sequence_more_recent(remote_seq, cl->incoming_sequence)) {
        u16 diff = remote_seq - cl->incoming_sequence;
        if (diff <= 32) {
            cl->ack_bitfield = (cl->ack_bitfield << diff) | (1u << (diff - 1));
        } else {
            cl->ack_bitfield = 0;
        }
        cl->incoming_sequence = remote_seq;
    } else {
        u16 diff = cl->incoming_sequence - remote_seq;
        if (diff > 0 && diff <= 32) {
            cl->ack_bitfield |= (1u << (diff - 1));
        }
    }
}

static void handle_snapshot_message(n_client_t *cl, const u8 *payload, u32 len) {
    if (len < 8) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);

    u32 base_tick = n_read_u32(&r);
    u32 current_tick = n_read_u32(&r);

    if (n_bitreader_overflowed(&r)) return;

    /* Extract remaining delta data */
    u32 delta_bytes = len - 8;
    u8 delta_buf[N_TRANSPORT_MTU];
    for (u32 i = 0; i < delta_bytes; i++) {
        delta_buf[i] = n_read_u8(&r);
    }

    /* Find baseline */
    const n_snapshot_t *baseline = NULL;
    if (base_tick != 0 && cl->has_baseline) {
        /* Search interp buffer for matching tick */
        for (u32 i = 0; i < cl->interp_count; i++) {
            u32 idx = (cl->interp_write - 1 - i + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
            if (cl->interp_snapshots[idx].tick == base_tick) {
                baseline = &cl->interp_snapshots[idx];
                break;
            }
        }

        /* Also check baseline_snapshot */
        if (!baseline && cl->baseline_snapshot.tick == base_tick) {
            baseline = &cl->baseline_snapshot;
        }

        if (!baseline) {
            /* Can't find baseline, skip this snapshot.
             * Server will eventually send a full snapshot. */
            return;
        }
    }

    /* Decode snapshot */
    u32 write_idx = cl->interp_write;
    n_snapshot_t *dest = &cl->interp_snapshots[write_idx];

    if (!n_snapshot_delta_decode(baseline, dest, delta_buf, delta_bytes, current_tick)) {
        return;
    }

    /* Update write pointer */
    cl->interp_write = (write_idx + 1) % N_INTERP_BUFFER_SIZE;
    if (cl->interp_count < N_INTERP_BUFFER_SIZE) {
        cl->interp_count++;
    }

    /* Update baseline: the most recent fully decoded snapshot */
    cl->baseline_snapshot = *dest;
    cl->has_baseline = true;
}

static void handle_connect_challenge(n_client_t *cl, const u8 *payload, u32 len) {
    if (cl->conn_state != N_CONN_CONNECTING) return;
    if (len < 8) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);
    u32 server_challenge = n_read_u32(&r);
    u32 client_challenge = n_read_u32(&r);

    if (client_challenge != cl->client_challenge) return;

    cl->server_challenge = server_challenge;
    send_connect_response(cl);
}

static void handle_connect_accepted(n_client_t *cl, const u8 *payload, u32 len) {
    if (cl->conn_state != N_CONN_CONNECTING) return;
    if (len < 5) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);
    cl->client_id = n_read_u8(&r);
    u32 server_tick = n_read_u32(&r);

    cl->conn_state = N_CONN_CONNECTED;
    cl->input_tick = server_tick;

    /* Initialize clock offset estimate from server tick */
    f64 now = n_platform_time();
    f64 server_time = (f64)server_tick * N_TICK_INTERVAL;
    cl->clock.smoothed_offset = server_time - now;
}

static void handle_connect_rejected(n_client_t *cl) {
    cl->conn_state = N_CONN_DISCONNECTED;
    n_transport_close(&cl->transport);
}

static void handle_clock_sync_response(n_client_t *cl, const u8 *payload, u32 len) {
    if (len < 16) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);
    f64 client_send_time = n_read_f64(&r);
    f64 server_time = n_read_f64(&r);

    f64 now = n_platform_time();
    f64 rtt = now - client_send_time;
    if (rtt < 0.0) return;

    f64 one_way = rtt * 0.5;
    f64 server_time_now = server_time + one_way;
    f64 offset = server_time_now - now;

    n_clock_add_sample(&cl->clock, rtt, offset);
}

void n_client_process_packet(n_client_t *cl, const u8 *data, u32 len, f64 now) {
    if (len < N_PACKET_HEADER_SIZE) {
        cl->stats.packets_dropped++;
        return;
    }

    n_packet_header_t hdr;
    n_packet_header_read(data, &hdr);

    update_client_ack_bitfield(cl, hdr.sequence);
    cl->last_packet_recv_time = now;

    /* Parse messages */
    n_bitreader_t r;
    n_bitreader_init(&r, data + N_PACKET_HEADER_SIZE, len - N_PACKET_HEADER_SIZE);

    while (!n_bitreader_overflowed(&r)) {
        n_msg_header_t msg;
        if (!n_msg_header_read(&r, &msg)) break;
        if (msg.type == N_MSG_NOP) break;

        /* Extract payload bytes */
        u8 payload_buf[N_TRANSPORT_MTU];
        u32 payload_bytes = msg.length;
        if (payload_bytes > sizeof(payload_buf)) payload_bytes = sizeof(payload_buf);
        for (u32 b = 0; b < payload_bytes; b++) {
            payload_buf[b] = n_read_u8(&r);
        }
        /* Skip any excess */
        for (u32 b = payload_bytes; b < msg.length; b++) {
            n_read_u8(&r);
        }

        switch (msg.type) {
            case N_MSG_SNAPSHOT:
                handle_snapshot_message(cl, payload_buf, payload_bytes);
                break;
            case N_MSG_CONNECT_CHALLENGE:
                handle_connect_challenge(cl, payload_buf, payload_bytes);
                break;
            case N_MSG_CONNECT_ACCEPTED:
                handle_connect_accepted(cl, payload_buf, payload_bytes);
                break;
            case N_MSG_CONNECT_REJECTED:
                handle_connect_rejected(cl);
                break;
            case N_MSG_CLOCK_SYNC:
                handle_clock_sync_response(cl, payload_buf, payload_bytes);
                break;
            case N_MSG_DISCONNECT:
                cl->conn_state = N_CONN_DISCONNECTED;
                break;
            default:
                break;
        }
    }

    cl->stats.packets_received++;
    cl->stats.bytes_received += len;
}

/* ---- Client tick ---- */

void n_client_tick(n_client_t *cl, f64 now) {
    if (!cl->initialized) return;

    /* Receive packets */
    u8 recv_buf[N_TRANSPORT_MTU];
    n_address_t from;
    i32 recv_len;

    while ((recv_len = n_transport_recv(&cl->transport, &from,
                                        recv_buf, sizeof(recv_buf))) > 0) {
        n_client_process_packet(cl, recv_buf, (u32)recv_len, now);
    }

    /* State-dependent logic */
    switch (cl->conn_state) {
        case N_CONN_CONNECTING: {
            /* Retry connect request */
            if (now - cl->last_connect_retry_time >= N_CONNECT_RETRY_SEC) {
                send_connect_request(cl);
                cl->last_connect_retry_time = now;
            }
            /* Timeout */
            if (now - cl->connect_start_time >= N_CONNECT_TIMEOUT_SEC) {
                cl->conn_state = N_CONN_DISCONNECTED;
                n_transport_close(&cl->transport);
            }
            break;
        }

        case N_CONN_CONNECTED: {
            /* Clock sync */
            if (n_clock_should_sync(&cl->clock, now)) {
                send_clock_sync(cl);
            }

            /* Timeout check (not for loopback) */
            if (!cl->is_loopback &&
                now - cl->last_packet_recv_time > N_TIMEOUT_SEC) {
                cl->conn_state = N_CONN_DISCONNECTED;
                n_transport_close(&cl->transport);
            }
            break;
        }

        default:
            break;
    }
}

/* ---- Send input ---- */

void n_client_send_input(n_client_t *cl, const n_input_t *input, f64 now) {
    if (cl->conn_state != N_CONN_CONNECTED) return;

    /* Store in history */
    u32 idx = cl->input_history_head % N_INPUT_QUEUE_SIZE;
    cl->input_history[idx] = *input;
    cl->input_history_head++;

    /* Build input packet with redundancy */
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    hdr.ack = cl->incoming_sequence;
    hdr.ack_bitfield = cl->ack_bitfield;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);

    /* Determine how many redundant inputs to send */
    u32 available = cl->input_history_head;
    u32 count = available < N_INPUT_REDUNDANCY ? available : N_INPUT_REDUNDANCY;
    u32 start_idx = cl->input_history_head - count;
    u32 start_tick = cl->input_tick > (count - 1) ? cl->input_tick - (count - 1) : 0;

    /* Input message payload size: 2 bits (count) + 32 bits (start_tick) + count * 9 bytes */
    u16 payload_len = (u16)(1 + 4 + count * 9); /* approximate byte size */
    n_msg_header_write(&w, N_MSG_INPUT, payload_len);

    n_write_bits(&w, count - 1, 2); /* 0=1 input, 1=2 inputs, 2=3 inputs */
    n_write_u32(&w, start_tick);

    for (u32 i = 0; i < count; i++) {
        u32 hist_idx = (start_idx + i) % N_INPUT_QUEUE_SIZE;
        const n_input_t *inp = &cl->input_history[hist_idx];
        n_write_u8(&w, (u8)inp->forward_move);
        n_write_u8(&w, (u8)inp->side_move);
        n_write_u16(&w, inp->yaw);
        n_write_u16(&w, inp->pitch);
        n_write_u16(&w, inp->buttons);
        n_write_u8(&w, inp->weapon_select);
    }

    /* Terminate */
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_transport_send(&cl->transport, &cl->server_address, pkt, total);

    cl->input_tick++;
    cl->stats.packets_sent++;
    cl->stats.bytes_sent += total;

    QK_UNUSED(now);
}

/* ---- Interpolation ---- */

/* Find two snapshots that bracket the given render tick */
static bool find_interp_pair(const n_client_t *cl, f64 render_tick,
                              const n_snapshot_t **out_a, const n_snapshot_t **out_b) {
    if (cl->interp_count < 2) return false;

    /* Find newest and oldest ticks, then find the pair that brackets render_tick */
    const n_snapshot_t *best_a = NULL;
    const n_snapshot_t *best_b = NULL;

    for (u32 i = 0; i < cl->interp_count; i++) {
        u32 idx = (cl->interp_write - 1 - i + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
        const n_snapshot_t *snap = &cl->interp_snapshots[idx];

        if ((f64)snap->tick <= render_tick) {
            if (!best_a || snap->tick > best_a->tick) {
                best_a = snap;
            }
        }
        if ((f64)snap->tick > render_tick) {
            if (!best_b || snap->tick < best_b->tick) {
                best_b = snap;
            }
        }
    }

    if (best_a && best_b) {
        *out_a = best_a;
        *out_b = best_b;
        return true;
    }

    /* Fallback: if render_tick is beyond all snapshots, use the two most recent */
    if (!best_b && cl->interp_count >= 2) {
        const n_snapshot_t *newest = NULL;
        const n_snapshot_t *second = NULL;
        for (u32 i = 0; i < cl->interp_count; i++) {
            u32 idx = (cl->interp_write - 1 - i + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
            const n_snapshot_t *snap = &cl->interp_snapshots[idx];
            if (!newest || snap->tick > newest->tick) {
                second = newest;
                newest = snap;
            } else if (!second || snap->tick > second->tick) {
                second = snap;
            }
        }
        if (newest && second) {
            *out_a = second;
            *out_b = newest;
            return true;
        }
    }

    return false;
}

/* Lerp a single f32 value */
static f32 lerpf(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

/* Lerp 16-bit quantized angle (shortest arc) to float degrees */
static f32 lerp_angle(u16 a_q, u16 b_q, f32 t) {
    f32 a = (f32)a_q * (360.0f / 65536.0f);
    f32 b = (f32)b_q * (360.0f / 65536.0f);

    f32 diff = b - a;
    if (diff > 180.0f) diff -= 360.0f;
    if (diff < -180.0f) diff += 360.0f;

    return a + diff * t;
}

/* Dequantize position from 13.3 fixed-point */
static f32 dequant_pos(i16 q) {
    return (f32)q * 0.125f;
}

/* Dequantize velocity (1 unit/sec precision) */
static f32 dequant_vel(i16 q) {
    return (f32)q;
}

void n_client_interpolate(n_client_t *cl, f64 render_time) {
    if (cl->conn_state != N_CONN_CONNECTED) return;

    f64 render_tick = render_time * (f64)N_TICK_RATE;

    const n_snapshot_t *snap_a = NULL;
    const n_snapshot_t *snap_b = NULL;

    if (!find_interp_pair(cl, render_tick, &snap_a, &snap_b)) {
        /* Not enough data to interpolate. If we have at least one snapshot,
         * use it directly. */
        if (cl->interp_count > 0) {
            u32 idx = (cl->interp_write - 1 + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
            const n_snapshot_t *snap = &cl->interp_snapshots[idx];
            for (u32 id = 0; id < N_MAX_ENTITIES; id++) {
                qk_interp_entity_t *ie = &cl->interp_state.entities[id];
                if (n_snapshot_has_entity(snap, (u8)id)) {
                    const n_entity_state_t *e = &snap->entities[id];
                    ie->pos_x = dequant_pos(e->pos_x);
                    ie->pos_y = dequant_pos(e->pos_y);
                    ie->pos_z = dequant_pos(e->pos_z);
                    ie->vel_x = dequant_vel(e->vel_x);
                    ie->vel_y = dequant_vel(e->vel_y);
                    ie->vel_z = dequant_vel(e->vel_z);
                    ie->yaw = (f32)e->yaw * (360.0f / 65536.0f);
                    ie->pitch = (f32)e->pitch * (360.0f / 65536.0f);
                    ie->entity_type = e->entity_type;
                    ie->flags = e->flags;
                    ie->health = e->health;
                    ie->armor = e->armor;
                    ie->weapon = e->weapon;
                    ie->ammo = e->ammo;
                    ie->active = true;
                } else {
                    ie->active = false;
                }
            }
        }
        return;
    }

    /* Compute interpolation factor */
    f32 t = 0.0f;
    if (snap_b->tick != snap_a->tick) {
        t = (f32)(render_tick - (f64)snap_a->tick) /
            (f32)(snap_b->tick - snap_a->tick);
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    for (u32 id = 0; id < N_MAX_ENTITIES; id++) {
        qk_interp_entity_t *ie = &cl->interp_state.entities[id];
        bool in_a = n_snapshot_has_entity(snap_a, (u8)id);
        bool in_b = n_snapshot_has_entity(snap_b, (u8)id);

        if (in_a && in_b) {
            /* Interpolate */
            const n_entity_state_t *ea = &snap_a->entities[id];
            const n_entity_state_t *eb = &snap_b->entities[id];

            ie->pos_x = lerpf(dequant_pos(ea->pos_x), dequant_pos(eb->pos_x), t);
            ie->pos_y = lerpf(dequant_pos(ea->pos_y), dequant_pos(eb->pos_y), t);
            ie->pos_z = lerpf(dequant_pos(ea->pos_z), dequant_pos(eb->pos_z), t);
            ie->vel_x = lerpf(dequant_vel(ea->vel_x), dequant_vel(eb->vel_x), t);
            ie->vel_y = lerpf(dequant_vel(ea->vel_y), dequant_vel(eb->vel_y), t);
            ie->vel_z = lerpf(dequant_vel(ea->vel_z), dequant_vel(eb->vel_z), t);

            ie->yaw = lerp_angle(ea->yaw, eb->yaw, t);
            ie->pitch = lerp_angle(ea->pitch, eb->pitch, t);

            /* Discrete fields: use B if t >= 0.5 */
            const n_entity_state_t *src = (t >= 0.5f) ? eb : ea;
            ie->entity_type = src->entity_type;
            ie->flags = src->flags;
            ie->health = src->health;
            ie->armor = src->armor;
            ie->weapon = src->weapon;
            ie->ammo = src->ammo;
            ie->active = true;

        } else if (in_b && !in_a) {
            /* Entity spawned: snap to B if close enough */
            if (render_tick >= (f64)snap_b->tick - 0.5) {
                const n_entity_state_t *eb = &snap_b->entities[id];
                ie->pos_x = dequant_pos(eb->pos_x);
                ie->pos_y = dequant_pos(eb->pos_y);
                ie->pos_z = dequant_pos(eb->pos_z);
                ie->vel_x = dequant_vel(eb->vel_x);
                ie->vel_y = dequant_vel(eb->vel_y);
                ie->vel_z = dequant_vel(eb->vel_z);
                ie->yaw = (f32)eb->yaw * (360.0f / 65536.0f);
                ie->pitch = (f32)eb->pitch * (360.0f / 65536.0f);
                ie->entity_type = eb->entity_type;
                ie->flags = eb->flags;
                ie->health = eb->health;
                ie->armor = eb->armor;
                ie->weapon = eb->weapon;
                ie->ammo = eb->ammo;
                ie->active = true;
            } else {
                ie->active = false;
            }

        } else if (in_a && !in_b) {
            /* Entity despawned: keep showing until past B */
            if (render_tick < (f64)snap_b->tick) {
                const n_entity_state_t *ea = &snap_a->entities[id];
                ie->pos_x = dequant_pos(ea->pos_x);
                ie->pos_y = dequant_pos(ea->pos_y);
                ie->pos_z = dequant_pos(ea->pos_z);
                ie->vel_x = dequant_vel(ea->vel_x);
                ie->vel_y = dequant_vel(ea->vel_y);
                ie->vel_z = dequant_vel(ea->vel_z);
                ie->yaw = (f32)ea->yaw * (360.0f / 65536.0f);
                ie->pitch = (f32)ea->pitch * (360.0f / 65536.0f);
                ie->entity_type = ea->entity_type;
                ie->flags = ea->flags;
                ie->health = ea->health;
                ie->armor = ea->armor;
                ie->weapon = ea->weapon;
                ie->ammo = ea->ammo;
                ie->active = true;
            } else {
                ie->active = false;
            }

        } else {
            ie->active = false;
        }
    }
}
