/*
 * QUICKEN Engine - Server
 *
 * Authoritative server: manages client slots, handles connection handshake,
 * processes client inputs, captures snapshots, delta-encodes and broadcasts.
 */

#include "n_internal.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef QUICKEN_DEBUG
#define N_DBG(fmt, ...) fprintf(stderr, "[NET-SV] " fmt "\n", ##__VA_ARGS__)
#else
#define N_DBG(fmt, ...) ((void)0)
#endif

/* ---- Helpers ---- */

static void slot_reset(n_client_slot_t *slot) {
    u8 id = slot->client_id;
    memset(slot, 0, sizeof(*slot));
    slot->client_id = id;
    slot->state = N_CONN_DISCONNECTED;
    n_reliable_init(&slot->reliable);
}

static void update_ack_bitfield(u16 *incoming_seq, u32 *ack_bits, u16 remote_seq) {
    if (*incoming_seq == 0 && *ack_bits == 0) {
        /* First packet */
        *incoming_seq = remote_seq;
        return;
    }

    if (n_sequence_more_recent(remote_seq, *incoming_seq)) {
        u16 diff = remote_seq - *incoming_seq;
        if (diff <= 32) {
            *ack_bits = (*ack_bits << diff) | (1u << (diff - 1));
        } else {
            *ack_bits = 0;
        }
        *incoming_seq = remote_seq;
    } else {
        u16 diff = *incoming_seq - remote_seq;
        if (diff > 0 && diff <= 32) {
            *ack_bits |= (1u << (diff - 1));
        }
    }
}

/* ---- Server lifecycle ---- */

void n_server_init(n_server_t *srv, u16 port, u32 max_clients, f64 tick_rate) {
    memset(srv, 0, sizeof(*srv));

    srv->server_port = port;
    srv->max_clients = max_clients > N_MAX_CLIENTS ? N_MAX_CLIENTS : max_clients;
    srv->tick_interval = (tick_rate > 0.0) ? (1.0 / tick_rate) : N_TICK_INTERVAL;
    srv->tick = 0;

    for (u32 i = 0; i < N_MAX_CLIENTS; i++) {
        slot_reset(&srv->clients[i]);
        srv->clients[i].client_id = (u8)i;
    }

    n_snapshot_init(&srv->current_snapshot);

    if (port > 0) {
        n_platform_init();
        if (n_transport_open_udp(&srv->transport, port)) {
            srv->initialized = true;
            N_DBG("init: UDP port=%u max_clients=%u", (u32)port, max_clients);
        } else {
            N_DBG("init: FAILED to bind UDP port=%u", (u32)port);
        }
    } else {
        /* Loopback-only server (no UDP socket needed) */
        memset(&srv->transport, 0, sizeof(srv->transport));
        srv->transport.socket_fd = -1;
        srv->initialized = true;
        N_DBG("init: loopback-only max_clients=%u", max_clients);
    }
}

void n_server_shutdown(n_server_t *srv) {
    if (!srv->initialized) return;

    for (u32 i = 0; i < srv->max_clients; i++) {
        if (srv->clients[i].state != N_CONN_DISCONNECTED) {
            n_server_disconnect_client(srv, i);
        }
    }

    if (srv->server_port > 0) {
        n_transport_close(&srv->transport);
    }

    srv->initialized = false;
}

/* ---- Slot management ---- */

i32 n_server_find_client_by_address(const n_server_t *srv, const n_address_t *addr) {
    for (u32 i = 0; i < srv->max_clients; i++) {
        if (srv->clients[i].state == N_CONN_DISCONNECTED) continue;
        if (srv->clients[i].address.ip == addr->ip &&
            srv->clients[i].address.port == addr->port) {
            return (i32)i;
        }
    }
    return -1;
}

i32 n_server_allocate_slot(n_server_t *srv) {
    for (u32 i = 0; i < srv->max_clients; i++) {
        if (srv->clients[i].state == N_CONN_DISCONNECTED) {
            return (i32)i;
        }
    }
    return -1;
}

void n_server_disconnect_client(n_server_t *srv, u32 slot) {
    if (slot >= srv->max_clients) return;
    n_client_slot_t *cl = &srv->clients[slot];
    if (cl->state == N_CONN_DISCONNECTED) return;

    N_DBG("disconnect_client: slot=%u loopback=%d", slot, cl->is_loopback);

    if (cl->is_loopback) {
        n_transport_close(&cl->transport);
    }

    slot_reset(cl);
    cl->client_id = (u8)slot;

    if (srv->client_count > 0) {
        srv->client_count--;
    }
}

int n_server_connect_loopback(n_server_t *srv) {
    i32 slot = n_server_allocate_slot(srv);
    if (slot < 0) return -1;

    n_client_slot_t *cl = &srv->clients[slot];
    slot_reset(cl);
    cl->client_id = (u8)slot;
    cl->state = N_CONN_CONNECTED;
    cl->is_loopback = true;
    cl->address.ip = 0;
    cl->address.port = 0;
    cl->last_packet_recv_time = n_platform_time();

    srv->client_count++;
    return slot;
}

/* ---- Send to client ---- */

void n_server_send_to_client(n_server_t *srv, u32 slot, const u8 *data, u32 len) {
    n_client_slot_t *cl = &srv->clients[slot];

    if (cl->is_loopback) {
        n_transport_send(&cl->transport, NULL, data, len);
    } else {
        n_transport_send(&srv->transport, &cl->address, data, len);
    }

    srv->stats.packets_sent++;
    srv->stats.bytes_sent += len;
}

/* ---- Build and send a packet to a client ---- */

static u32 build_packet(n_server_t *srv, u32 slot, u8 *buf, u32 max_len) {
    n_client_slot_t *cl = &srv->clients[slot];

    /* Write packet header */
    n_packet_header_t hdr;
    hdr.sequence = cl->outgoing_sequence++;
    hdr.ack = cl->incoming_sequence;
    hdr.ack_bitfield = cl->ack_bitfield;

    n_packet_header_write(buf, &hdr);

    /* Payload area starts after 8-byte header */
    n_bitwriter_t w;
    n_bitwriter_init(&w, buf + N_PACKET_HEADER_SIZE, max_len - N_PACKET_HEADER_SIZE);

    return N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
}

/* ---- Snapshot broadcast ---- */

void n_server_broadcast_snapshots(n_server_t *srv) {
    /* Store current snapshot in history ring buffer */
    u32 hist_idx = srv->tick % N_SNAPSHOT_HISTORY;
    srv->current_snapshot.tick = srv->tick;
    srv->snapshot_buffer.snapshots[hist_idx] = srv->current_snapshot;
    srv->snapshot_buffer.current_index = hist_idx;

    N_DBG("broadcast: tick=%u entities=%u", srv->tick, srv->current_snapshot.entity_count);

    for (u32 i = 0; i < srv->max_clients; i++) {
        n_client_slot_t *cl = &srv->clients[i];
        if (cl->state != N_CONN_CONNECTED) continue;

        /* Find baseline snapshot for this client */
        const n_snapshot_t *baseline = NULL;
        if (cl->last_acked_snapshot_tick > 0) {
            u32 base_idx = cl->last_acked_snapshot_tick % N_SNAPSHOT_HISTORY;
            n_snapshot_t *candidate = &srv->snapshot_buffer.snapshots[base_idx];
            if (candidate->tick == cl->last_acked_snapshot_tick) {
                /* Verify baseline hasn't been overwritten and isn't
                 * the current tick (self-reference).  Self-reference
                 * happens when a multi-tick batch inflates ack_tick
                 * to match srv->tick.  The client can't have received
                 * the current tick's snapshot yet, so delta-encoding
                 * against it causes a client-side drop and cascading
                 * failures.  Fall back to full snapshot instead. */
                u32 age = srv->tick - cl->last_acked_snapshot_tick;
                if (age > 0 && age < N_SNAPSHOT_HISTORY) {
                    baseline = candidate;
                }
            }
        }

        /* Delta encode */
        u8 delta_buf[N_TRANSPORT_MTU];
        u32 delta_len = n_snapshot_delta_encode(baseline, &srv->current_snapshot,
                                                delta_buf, sizeof(delta_buf));

        /* Build packet with snapshot message */
        u8 *pkt = srv->packet_buffer;
        n_packet_header_t hdr;
        hdr.sequence = cl->outgoing_sequence++;
        hdr.ack = cl->incoming_sequence;
        hdr.ack_bitfield = cl->ack_bitfield;
        n_packet_header_write(pkt, &hdr);

        n_bitwriter_t w;
        n_bitwriter_init(&w, pkt + N_PACKET_HEADER_SIZE,
                         N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);

        /* Snapshot message: header (base_tick + current_tick + cmd_ack = 12 bytes) + delta */
        u16 msg_payload_len = (u16)(12 + delta_len);
        n_msg_header_write(&w, N_MSG_SNAPSHOT, msg_payload_len);

        u32 base_tick = baseline ? baseline->tick : 0;
        n_write_u32(&w, base_tick);
        n_write_u32(&w, srv->tick);
        n_write_u32(&w, cl->last_input_tick);

        /* Write delta data raw */
        for (u32 b = 0; b < delta_len; b++) {
            n_write_u8(&w, delta_buf[b]);
        }

        /* Terminate with NOP */
        n_msg_header_write(&w, N_MSG_NOP, 0);

        u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
        n_server_send_to_client(srv, i, pkt, total);

        if (baseline) {
            srv->stats.snapshots_delta++;
        } else {
            srv->stats.snapshots_full++;
        }
    }
}

/* ---- Handle incoming packets ---- */

static void handle_connect_request(n_server_t *srv, const u8 *payload, u32 len,
                                    const n_address_t *from, n_transport_t *via,
                                    bool is_loopback) {
    if (len < 4) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);
    u32 client_challenge = n_read_u32(&r);

    /* Check if already connecting from this address */
    i32 existing = -1;
    if (!is_loopback) {
        existing = n_server_find_client_by_address(srv, from);
    }

    if (existing >= 0) {
        /* Resend challenge */
        n_client_slot_t *cl = &srv->clients[existing];
        u8 resp[N_TRANSPORT_MTU];
        n_packet_header_t hdr = {0};
        hdr.sequence = cl->outgoing_sequence++;
        n_packet_header_write(resp, &hdr);

        n_bitwriter_t w;
        n_bitwriter_init(&w, resp + N_PACKET_HEADER_SIZE,
                         N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
        n_msg_header_write(&w, N_MSG_CONNECT_CHALLENGE, 8);
        n_write_u32(&w, cl->server_challenge);
        n_write_u32(&w, client_challenge);
        n_msg_header_write(&w, N_MSG_NOP, 0);

        u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
        if (is_loopback) {
            n_transport_send(&cl->transport, NULL, resp, total);
        } else {
            n_transport_send(via, from, resp, total);
        }
        return;
    }

    /* Allocate new slot */
    i32 slot = n_server_allocate_slot(srv);
    if (slot < 0) {
        /* Server full: send rejection */
        u8 resp[N_TRANSPORT_MTU];
        n_packet_header_t hdr = {0};
        n_packet_header_write(resp, &hdr);

        n_bitwriter_t w;
        n_bitwriter_init(&w, resp + N_PACKET_HEADER_SIZE,
                         N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
        n_msg_header_write(&w, N_MSG_CONNECT_REJECTED, 1);
        n_write_u8(&w, 1); /* reason: full */
        n_msg_header_write(&w, N_MSG_NOP, 0);

        u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
        n_transport_send(via, from, resp, total);
        return;
    }

    n_client_slot_t *cl = &srv->clients[slot];
    slot_reset(cl);
    cl->client_id = (u8)slot;
    cl->state = N_CONN_CONNECTING;
    cl->address = *from;
    cl->client_challenge = client_challenge;
    cl->server_challenge = n_random_u32();
    cl->connect_start_time = n_platform_time();
    cl->last_packet_recv_time = n_platform_time();
    cl->is_loopback = is_loopback;

    /* Send challenge */
    u8 resp[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    n_packet_header_write(resp, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, resp + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_CONNECT_CHALLENGE, 8);
    n_write_u32(&w, cl->server_challenge);
    n_write_u32(&w, client_challenge);
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    if (is_loopback) {
        n_transport_send(&cl->transport, NULL, resp, total);
    } else {
        n_transport_send(via, from, resp, total);
    }
}

static void handle_connect_response(n_server_t *srv, u32 slot,
                                      const u8 *payload, u32 len) {
    n_client_slot_t *cl = &srv->clients[slot];
    if (cl->state != N_CONN_CONNECTING) return;
    if (len < 8) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);
    u32 server_challenge = n_read_u32(&r);
    u32 client_challenge = n_read_u32(&r);

    if (server_challenge != cl->server_challenge) return;
    if (client_challenge != cl->client_challenge) return;

    /* Accept the connection */
    cl->state = N_CONN_CONNECTED;
    cl->last_packet_recv_time = n_platform_time();
    srv->client_count++;

    /* Send CONNECT_ACCEPTED */
    u8 resp[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    hdr.ack = cl->incoming_sequence;
    hdr.ack_bitfield = cl->ack_bitfield;
    n_packet_header_write(resp, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, resp + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_CONNECT_ACCEPTED, 5);
    n_write_u8(&w, cl->client_id);
    n_write_u32(&w, srv->tick);
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_server_send_to_client(srv, slot, resp, total);
}

static void handle_input_message(n_server_t *srv, u32 slot,
                                  const u8 *payload, u32 len) {
    n_client_slot_t *cl = &srv->clients[slot];
    if (cl->state != N_CONN_CONNECTED) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);

    u32 input_count = n_read_bits(&r, 2) + 1; /* 1..3 stored as 0..2 */
    u32 start_tick = n_read_u32(&r);

    N_DBG("input: slot=%u count=%u start_tick=%u srv_tick=%u",
          slot, input_count, start_tick, srv->tick);

    for (u32 i = 0; i < input_count; i++) {
        n_input_t input;
        input.forward_move = (i8)n_read_u8(&r);
        input.side_move = (i8)n_read_u8(&r);
        input.yaw = n_read_u16(&r);
        input.pitch = n_read_u16(&r);
        input.buttons = n_read_u16(&r);
        input.weapon_select = n_read_u8(&r);

        if (n_bitreader_overflowed(&r)) break;

        u32 input_tick = start_tick + i;

        /* Discard if too old (already simulated) */
        if (input_tick < srv->tick && (srv->tick - input_tick) > N_INPUT_QUEUE_SIZE) {
            srv->stats.inputs_late++;
            continue;
        }

        /* Store in ring buffer */
        u32 idx = input_tick % N_INPUT_QUEUE_SIZE;

        /* Check for duplicate */
        if (input_tick <= cl->last_input_tick && cl->last_input_tick > 0) {
            srv->stats.inputs_duplicated++;
            continue;
        }

        cl->input_queue[idx] = input;
        if (input_tick > cl->last_input_tick) {
            cl->last_input_tick = input_tick;
        }
        cl->last_input = input;
        srv->stats.inputs_received++;
    }
}

static void handle_clock_sync_message(n_server_t *srv, u32 slot,
                                       const u8 *payload, u32 len) {
    n_client_slot_t *cl = &srv->clients[slot];
    if (cl->state != N_CONN_CONNECTED) return;
    if (len < 8) return;

    n_bitreader_t r;
    n_bitreader_init(&r, payload, len);
    f64 client_send_time = n_read_f64(&r);

    /* Send response with server time */
    u8 resp[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = cl->outgoing_sequence++;
    hdr.ack = cl->incoming_sequence;
    hdr.ack_bitfield = cl->ack_bitfield;
    n_packet_header_write(resp, &hdr);

    n_bitwriter_t w;
    n_bitwriter_init(&w, resp + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&w, N_MSG_CLOCK_SYNC, 16);
    n_write_f64(&w, client_send_time);
    n_write_f64(&w, n_platform_time());
    n_msg_header_write(&w, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&w);
    n_server_send_to_client(srv, slot, resp, total);
}

static void handle_disconnect_message(n_server_t *srv, u32 slot) {
    n_server_disconnect_client(srv, slot);
}

void n_server_process_packet(n_server_t *srv, u8 *data, u32 len,
                             const n_address_t *from, n_transport_t *via) {
    if (len < N_PACKET_HEADER_SIZE) {
        srv->stats.packets_dropped++;
        return;
    }

    n_packet_header_t hdr;
    n_packet_header_read(data, &hdr);

    bool is_loopback = (via != &srv->transport);

    /* Find which client slot this packet belongs to */
    i32 slot = -1;
    if (is_loopback) {
        /* For loopback, find by transport pointer */
        for (u32 i = 0; i < srv->max_clients; i++) {
            if (srv->clients[i].state == N_CONN_DISCONNECTED) continue;
            if (srv->clients[i].is_loopback &&
                srv->clients[i].transport.recv_queue == via->recv_queue) {
                slot = (i32)i;
                break;
            }
        }
    } else {
        slot = n_server_find_client_by_address(srv, from);
    }

    /* Parse messages */
    n_bitreader_t r;
    n_bitreader_init(&r, data + N_PACKET_HEADER_SIZE, len - N_PACKET_HEADER_SIZE);

    while (!n_bitreader_overflowed(&r)) {
        n_msg_header_t msg;
        if (!n_msg_header_read(&r, &msg)) break;
        if (msg.type == N_MSG_NOP) break;

        /* Save current position to extract payload */
        u32 payload_start_bit = r.bit_pos;

        /* For connection messages, we may not have a slot yet */
        if (msg.type == N_MSG_CONNECT_REQUEST) {
            u8 payload_buf[256];
            u32 payload_bytes = msg.length < 256 ? msg.length : 256;
            for (u32 b = 0; b < payload_bytes; b++) {
                payload_buf[b] = n_read_u8(&r);
            }
            handle_connect_request(srv, payload_buf, payload_bytes,
                                    from, via, is_loopback);
            continue;
        }

        if (msg.type == N_MSG_CONNECT_RESPONSE && slot >= 0) {
            u8 payload_buf[256];
            u32 payload_bytes = msg.length < 256 ? msg.length : 256;
            for (u32 b = 0; b < payload_bytes; b++) {
                payload_buf[b] = n_read_u8(&r);
            }
            handle_connect_response(srv, (u32)slot, payload_buf, payload_bytes);
            continue;
        }

        if (slot < 0) {
            /* Unknown sender, skip message */
            for (u32 b = 0; b < msg.length; b++) {
                n_read_u8(&r);
            }
            continue;
        }

        n_client_slot_t *cl = &srv->clients[slot];

        /* Update ack state */
        update_ack_bitfield(&cl->incoming_sequence, &cl->ack_bitfield, hdr.sequence);
        cl->last_packet_recv_time = n_platform_time();

        /* Track which snapshots the client has acked */
        /* The client acking our sequence N means they got the snapshot in that packet.
         * We track the association between outgoing sequence and snapshot tick
         * via a simple approach: the most recent snapshot sent was for tick == server tick
         * at the time. We approximate: acked sequence means they have our current snapshot. */
        /* For better accuracy, we store the tick in the snapshot message itself,
         * and the client's ack tells us implicitly. We'll use the fact that the
         * client echoes the snapshot tick when it sends input. */

        switch (msg.type) {
            case N_MSG_INPUT: {
                u8 payload_buf[256];
                u32 payload_bytes = msg.length < 256 ? msg.length : 256;
                for (u32 b = 0; b < payload_bytes; b++) {
                    payload_buf[b] = n_read_u8(&r);
                }
                handle_input_message(srv, (u32)slot, payload_buf, payload_bytes);

                /* Use the most recent input tick as implicit snapshot ack.
                 * The client sends inputs timestamped with server ticks it knows about,
                 * meaning it has received snapshots up to around that tick. */
                if (cl->last_input_tick > cl->last_acked_snapshot_tick) {
                    /* Client must have tick - interp_buffer_ticks as baseline.
                     * Conservative: ack = last_input_tick - a few ticks. */
                    u32 ack_tick = cl->last_input_tick > 4 ? cl->last_input_tick - 4 : 1;
                    if (ack_tick > cl->last_acked_snapshot_tick) {
                        cl->last_acked_snapshot_tick = ack_tick;
                    }
                }
                break;
            }

            case N_MSG_CLOCK_SYNC: {
                u8 payload_buf[64];
                u32 payload_bytes = msg.length < 64 ? msg.length : 64;
                for (u32 b = 0; b < payload_bytes; b++) {
                    payload_buf[b] = n_read_u8(&r);
                }
                handle_clock_sync_message(srv, (u32)slot, payload_buf, payload_bytes);
                break;
            }

            case N_MSG_DISCONNECT:
                handle_disconnect_message(srv, (u32)slot);
                break;

            default:
                /* Skip unknown message */
                for (u32 b = 0; b < msg.length; b++) {
                    n_read_u8(&r);
                }
                break;
        }

        QK_UNUSED(payload_start_bit);
    }

    srv->stats.packets_received++;
    srv->stats.bytes_received += len;
}

/* ---- Server tick ---- */

void n_server_tick(n_server_t *srv, f64 now) {
    if (!srv->initialized) return;

    srv->tick++;
    srv->tick_time = now;

    /* Receive packets from UDP */
    if (srv->server_port > 0) {
        u8 recv_buf[N_TRANSPORT_MTU];
        n_address_t from;
        i32 recv_len;

        while ((recv_len = n_transport_recv(&srv->transport, &from,
                                            recv_buf, sizeof(recv_buf))) > 0) {
            n_server_process_packet(srv, recv_buf, (u32)recv_len,
                                    &from, &srv->transport);
        }
    }

    /* Receive packets from loopback clients */
    for (u32 i = 0; i < srv->max_clients; i++) {
        n_client_slot_t *cl = &srv->clients[i];
        if (!cl->is_loopback) continue;
        if (cl->state == N_CONN_DISCONNECTED) continue;

        u8 recv_buf[N_TRANSPORT_MTU];
        n_address_t from = {0};
        i32 recv_len;

        /* Server reads from its side of the loopback */
        while ((recv_len = n_transport_recv(&cl->transport, &from,
                                            recv_buf, sizeof(recv_buf))) > 0) {
            n_server_process_packet(srv, recv_buf, (u32)recv_len,
                                    &cl->address, &cl->transport);
        }
    }

    /* Check for timeouts */
    for (u32 i = 0; i < srv->max_clients; i++) {
        n_client_slot_t *cl = &srv->clients[i];
        if (cl->state == N_CONN_DISCONNECTED) continue;
        if (cl->is_loopback) continue; /* loopback never times out */

        if (cl->state == N_CONN_CONNECTING) {
            if (now - cl->connect_start_time > N_CONNECT_TIMEOUT_SEC) {
                n_server_disconnect_client(srv, i);
                continue;
            }
        }

        if (now - cl->last_packet_recv_time > N_TIMEOUT_SEC) {
            n_server_disconnect_client(srv, i);
        }
    }

    /* Broadcast snapshots to all connected clients */
    n_server_broadcast_snapshots(srv);
}
