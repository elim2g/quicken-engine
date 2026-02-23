/*
 * QUICKEN Engine - Client
 *
 * Connects to a server (local or remote), receives snapshots,
 * manages interpolation buffer, sends inputs, handles clock sync.
 */

#include "n_internal.h"
#include "core/qk_demo.h"
#include "core/qk_prof.h"
#include <math.h>
#include <stdio.h>

#ifdef QUICKEN_DEBUG
#define N_DBG(fmt, ...) fprintf(stderr, "[NET-CL] " fmt "\n", ##__VA_ARGS__)
#else
#define N_DBG(fmt, ...) ((void)0)
#endif

#ifdef QK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

// --- Helpers ---

#if 0 // TODO: enable when client interpolation/prediction lands
static f64 client_server_time(const n_client_t *client) {
    return n_platform_time() + client->clock.smoothed_offset;
}

static u32 client_server_tick(const n_client_t *client) {
    f64 server_time = client_server_time(client);
    if (server_time < 0.0) return 0;
    return (u32)(server_time * (f64)N_TICK_RATE);
}

static f64 client_render_time(const n_client_t *client) {
    return client_server_time(client) - client->interp_delay;
}
#endif

// --- Lifecycle ---

void n_client_init(n_client_t *client, f64 interp_delay) {
    memset(client, 0, sizeof(*client));

    client->conn_state = N_CONN_DISCONNECTED;
    client->transport.socket_fd = -1;
    n_reliable_init(&client->reliable);
    n_clock_init(&client->clock);

    if (interp_delay > 0.0) {
        if (interp_delay < N_INTERP_DELAY_MIN) interp_delay = N_INTERP_DELAY_MIN;
        if (interp_delay > N_INTERP_DELAY_MAX) interp_delay = N_INTERP_DELAY_MAX;
        client->interp_delay = interp_delay;
    } else {
        client->interp_delay = N_INTERP_DELAY_DEFAULT;
    }

    client->initialized = true;
}

void n_client_shutdown(n_client_t *client) {
    if (!client->initialized) return;
    if (client->conn_state != N_CONN_DISCONNECTED) {
        n_client_disconnect(client);
    }
    client->initialized = false;
}

// --- Connection ---

// Helper to reset a slot for loopback without losing the client_id
static void slot_reset_for_loopback(n_client_slot_t *slot, u8 id) {
    memset(slot, 0, sizeof(*slot));
    slot->state = N_CONN_DISCONNECTED;
    slot->client_id = id;
    n_reliable_init(&slot->reliable);
}

void n_client_connect_remote(n_client_t *client, const char *address, u16 port) {
    if (client->conn_state != N_CONN_DISCONNECTED) return;

    n_platform_init();

    // Parse IP address
    struct in_addr addr;
    if (inet_pton(AF_INET, address, &addr) != 1) return;

    client->server_address.ip = ntohl(addr.s_addr);
    client->server_address.port = port;

    // Open a UDP socket with OS-assigned port
    if (!n_transport_open_udp(&client->transport, 0)) {
        return;
    }

    client->is_loopback = false;
    client->conn_state = N_CONN_CONNECTING;
    client->client_challenge = n_random_u32();
    client->connect_start_time = n_platform_time();
    client->last_connect_retry_time = 0.0;

    // Send initial connect request
    n_client_tick(client, n_platform_time());
}

void n_client_connect_local(n_client_t *client, n_server_t *srv) {
    if (client->conn_state != N_CONN_DISCONNECTED) return;

    client->loopback_server = srv;
    client->is_loopback = true;

    // Allocate a loopback slot on the server
    i32 slot = n_server_allocate_slot(srv);
    if (slot < 0) {
        N_DBG("connect_local: %s", "no free slot on server");
        return;
    }

    n_client_slot_t *server_slot = &srv->clients[slot];
    slot_reset_for_loopback(server_slot, (u8)slot);

    // Set up cross-wired loopback transports
    n_transport_open_loopback(&client->transport, &server_slot->transport,
                              &srv->loopback_queues[slot][0],
                              &srv->loopback_queues[slot][1]);

    // Loopback skips the handshake entirely -- client and server are in
    // the same process, so challenge/response is pointless. Go straight
    // to CONNECTED on both sides.
    f64 now = n_platform_time();

    server_slot->state = N_CONN_CONNECTED;
    server_slot->is_loopback = true;
    server_slot->map_ready = true;  // loopback: same process, map is shared
    server_slot->last_packet_recv_time = now;
    srv->client_count++;

    client->conn_state = N_CONN_CONNECTED;
    client->client_id = (u8)slot;
    client->input_tick = srv->tick;
    client->map_ready = true;   // loopback: same process, map is shared

    // Zero clock offset for loopback (zero latency, same clock)
    client->clock.smoothed_offset = (f64)srv->tick * N_TICK_INTERVAL - now;
    client->clock.converged = true;

    N_DBG("connect_local: connected as client_id=%u, server_tick=%u", (u32)slot, srv->tick);
}

void n_client_disconnect(n_client_t *client) {
    if (client->conn_state == N_CONN_DISCONNECTED) return;

    N_DBG("disconnect: state=%d loopback=%d", client->conn_state, client->is_loopback);

    // Send disconnect message
    if (client->conn_state == N_CONN_CONNECTED) {
        u8 pkt[N_TRANSPORT_MTU];
        n_packet_header_t hdr = {0};
        hdr.sequence = client->outgoing_sequence++;
        hdr.ack = client->incoming_sequence;
        hdr.ack_bitfield = client->ack_bitfield;
        n_packet_header_write(pkt, &hdr);

        n_bitwriter_t writer;
        n_bitwriter_init(&writer, pkt + N_PACKET_HEADER_SIZE,
                         N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
        n_msg_header_write(&writer, N_MSG_DISCONNECT, 0);
        n_msg_header_write(&writer, N_MSG_NOP, 0);

        u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&writer);
        n_transport_send(&client->transport, &client->server_address, pkt, total);
    }

    n_transport_close(&client->transport);
    client->conn_state = N_CONN_DISCONNECTED;
    client->has_baseline = false;
    client->interp_count = 0;
    client->interp_write = 0;
    client->map_ready = false;
    n_clock_init(&client->clock);
}

// --- Send connect request ---

static void send_connect_request(n_client_t *client) {
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = client->outgoing_sequence++;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t writer;
    n_bitwriter_init(&writer, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&writer, N_MSG_CONNECT_REQUEST, 4);
    n_write_u32(&writer, client->client_challenge);
    n_msg_header_write(&writer, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&writer);
    n_transport_send(&client->transport, &client->server_address, pkt, total);
    client->stats.packets_sent++;
}

static void send_connect_response(n_client_t *client) {
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = client->outgoing_sequence++;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t writer;
    n_bitwriter_init(&writer, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&writer, N_MSG_CONNECT_RESPONSE, 8);
    n_write_u32(&writer, client->server_challenge);
    n_write_u32(&writer, client->client_challenge);
    n_msg_header_write(&writer, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&writer);
    n_transport_send(&client->transport, &client->server_address, pkt, total);
    client->stats.packets_sent++;
}

// --- Send clock sync probe ---

static void send_clock_sync(n_client_t *client) {
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = client->outgoing_sequence++;
    hdr.ack = client->incoming_sequence;
    hdr.ack_bitfield = client->ack_bitfield;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t writer;
    n_bitwriter_init(&writer, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);
    n_msg_header_write(&writer, N_MSG_CLOCK_SYNC, 8);

    f64 now = n_platform_time();
    client->clock_sync_send_time = now;
    n_write_f64(&writer, now);
    n_msg_header_write(&writer, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&writer);
    n_transport_send(&client->transport, &client->server_address, pkt, total);
    n_clock_mark_sent(&client->clock, now);
    client->stats.packets_sent++;
}

// --- Packet processing ---

static void update_client_ack_bitfield(n_client_t *client, u16 remote_seq) {
    if (client->incoming_sequence == 0 && client->ack_bitfield == 0) {
        client->incoming_sequence = remote_seq;
        return;
    }

    if (n_sequence_more_recent(remote_seq, client->incoming_sequence)) {
        u16 diff = remote_seq - client->incoming_sequence;
        if (diff <= 32) {
            client->ack_bitfield = (client->ack_bitfield << diff) | (1u << (diff - 1));
        } else {
            client->ack_bitfield = 0;
        }
        client->incoming_sequence = remote_seq;
    } else {
        u16 diff = client->incoming_sequence - remote_seq;
        if (diff > 0 && diff <= 32) {
            client->ack_bitfield |= (1u << (diff - 1));
        }
    }
}

static void handle_snapshot_message(n_client_t *client, const u8 *payload, u32 len) {
    if (len < 12) return;

    n_bitreader_t reader;
    n_bitreader_init(&reader, payload, len);

    u32 base_tick = n_read_u32(&reader);
    u32 current_tick = n_read_u32(&reader);
    u32 cmd_ack = n_read_u32(&reader);

    if (n_bitreader_overflowed(&reader)) return;

    client->last_server_cmd_ack = cmd_ack;

    N_DBG("snapshot: tick=%u base=%u cmd_ack=%u bytes=%u interp_count=%u",
          current_tick, base_tick, cmd_ack, len, client->interp_count);

    // Extract remaining delta data
    u32 delta_bytes = len - 12;
    u8 delta_buf[N_TRANSPORT_MTU];
    for (u32 i = 0; i < delta_bytes; i++) {
        delta_buf[i] = n_read_u8(&reader);
    }

    // Find baseline
    const n_snapshot_t *baseline = NULL;
    if (base_tick != 0 && client->has_baseline) {
        // Search interp buffer for matching tick
        for (u32 i = 0; i < client->interp_count; i++) {
            u32 idx = (client->interp_write - 1 - i + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
            if (client->interp_snapshots[idx].tick == base_tick) {
                baseline = &client->interp_snapshots[idx];
                break;
            }
        }

        // Also check baseline_snapshot
        if (!baseline && client->baseline_snapshot.tick == base_tick) {
            baseline = &client->baseline_snapshot;
        }

        if (!baseline) {
            // Can't find baseline, skip this snapshot.
            // Server will eventually send a full snapshot.
            N_DBG("snapshot: missing baseline tick=%u, dropping", base_tick);
            return;
        }
    }

    // Decode snapshot
    u32 write_idx = client->interp_write;
    n_snapshot_t *dest = &client->interp_snapshots[write_idx];

    if (!n_snapshot_delta_decode(baseline, dest, delta_buf, delta_bytes, current_tick)) {
        return;
    }

    // Teleport handling: do NOT flush the interp buffer here.
    // Flushing destroys the delta-decode baseline chain, causing most
    // subsequent snapshots to be dropped (base_tick mismatch).
    // Instead, teleport is handled per-entity in n_client_interpolate()
    // via XOR comparison of the toggle bit between snap_a and snap_b.
    // When the bit differs, that entity snaps to destination without
    // lerp.  Other entities (projectiles, etc.) interpolate normally.
    client->interp_write = (write_idx + 1) % N_INTERP_BUFFER_SIZE;
    if (client->interp_count < N_INTERP_BUFFER_SIZE) {
        client->interp_count++;
    }

    // Update baseline: the most recent fully decoded snapshot
    client->baseline_snapshot = *dest;
    client->has_baseline = true;

    // Demo recording hook
    if (qk_demo_is_recording()) {
        qk_demo_record_snapshot(dest->tick, dest->entity_count,
                                dest->entity_mask, dest->entities);
    }
}

static void handle_connect_challenge(n_client_t *client, const u8 *payload, u32 len) {
    if (client->conn_state != N_CONN_CONNECTING) return;
    if (len < 8) return;

    n_bitreader_t reader;
    n_bitreader_init(&reader, payload, len);
    u32 server_challenge = n_read_u32(&reader);
    u32 client_challenge = n_read_u32(&reader);

    if (client_challenge != client->client_challenge) return;

    client->server_challenge = server_challenge;
    send_connect_response(client);
}

static void handle_connect_accepted(n_client_t *client, const u8 *payload, u32 len) {
    if (client->conn_state != N_CONN_CONNECTING) return;
    if (len < 5) return;

    n_bitreader_t reader;
    n_bitreader_init(&reader, payload, len);
    client->client_id = n_read_u8(&reader);
    u32 server_tick = n_read_u32(&reader);

    client->conn_state = N_CONN_CONNECTED;
    client->input_tick = server_tick;

    // Read map name (if present in payload)
    client->server_map_name[0] = '\0';
    if (len >= 6) {
        u8 map_name_len = n_read_u8(&reader);
        if (map_name_len > 0 && map_name_len < sizeof(client->server_map_name)) {
            for (u32 mi = 0; mi < map_name_len && !n_bitreader_overflowed(&reader); mi++) {
                client->server_map_name[mi] = (char)n_read_u8(&reader);
            }
            client->server_map_name[map_name_len] = '\0';
            N_DBG("connect_accepted: map='%s'", client->server_map_name);
        }
    }

    // Initialize clock offset estimate from server tick
    f64 now = n_platform_time();
    f64 server_time = (f64)server_tick * N_TICK_INTERVAL;
    client->clock.smoothed_offset = server_time - now;
}

static void handle_connect_rejected(n_client_t *client) {
    client->conn_state = N_CONN_DISCONNECTED;
    n_transport_close(&client->transport);
}

static void handle_clock_sync_response(n_client_t *client, const u8 *payload, u32 len) {
    if (len < 16) return;

    n_bitreader_t reader;
    n_bitreader_init(&reader, payload, len);
    f64 client_send_time = n_read_f64(&reader);
    f64 server_time = n_read_f64(&reader);

    f64 now = n_platform_time();
    f64 rtt = now - client_send_time;
    if (rtt < 0.0) return;

    f64 one_way = rtt * 0.5;
    f64 server_time_now = server_time + one_way;
    f64 offset = server_time_now - now;

    n_clock_add_sample(&client->clock, rtt, offset);
}

void n_client_process_packet(n_client_t *client, const u8 *data, u32 len, f64 now) {
    if (len < N_PACKET_HEADER_SIZE) {
        client->stats.packets_dropped++;
        return;
    }

    n_packet_header_t hdr;
    n_packet_header_read(data, &hdr);

    update_client_ack_bitfield(client, hdr.sequence);
    client->last_packet_recv_time = now;

    // Parse messages
    n_bitreader_t reader;
    n_bitreader_init(&reader, data + N_PACKET_HEADER_SIZE, len - N_PACKET_HEADER_SIZE);

    while (!n_bitreader_overflowed(&reader)) {
        n_msg_header_t msg;
        if (!n_msg_header_read(&reader, &msg)) break;
        if (msg.type == N_MSG_NOP) break;

        // Extract payload bytes
        u8 payload_buf[N_TRANSPORT_MTU];
        u32 payload_bytes = msg.length;
        if (payload_bytes > sizeof(payload_buf)) payload_bytes = sizeof(payload_buf);
        for (u32 b = 0; b < payload_bytes; b++) {
            payload_buf[b] = n_read_u8(&reader);
        }
        // Skip any excess
        for (u32 b = payload_bytes; b < msg.length; b++) {
            n_read_u8(&reader);
        }

        switch (msg.type) {
            case N_MSG_SNAPSHOT:
                handle_snapshot_message(client, payload_buf, payload_bytes);
                break;
            case N_MSG_CONNECT_CHALLENGE:
                handle_connect_challenge(client, payload_buf, payload_bytes);
                break;
            case N_MSG_CONNECT_ACCEPTED:
                handle_connect_accepted(client, payload_buf, payload_bytes);
                break;
            case N_MSG_CONNECT_REJECTED:
                handle_connect_rejected(client);
                break;
            case N_MSG_CLOCK_SYNC:
                handle_clock_sync_response(client, payload_buf, payload_bytes);
                break;
            case N_MSG_MAP_CONFIRMED: {
                if (payload_bytes >= 4) {
                    n_bitreader_t map_reader;
                    n_bitreader_init(&map_reader, payload_buf, payload_bytes);
                    u32 server_tick = n_read_u32(&map_reader);
                    client->map_ready = true;
                    client->input_tick = server_tick;
                    // Reset interp buffer for clean start
                    client->interp_count = 0;
                    client->interp_write = 0;
                    client->has_baseline = false;
                    N_DBG("map_confirmed: server_tick=%u", server_tick);
                }
                break;
            }
            case N_MSG_DISCONNECT:
                client->conn_state = N_CONN_DISCONNECTED;
                break;
            default:
                break;
        }
    }

    client->stats.packets_received++;
    client->stats.bytes_received += len;
    QK_PROF_COUNTER("cl_packets_recv", 1);
    QK_PROF_COUNTER("cl_bytes_recv", len);
}

// --- Client tick ---

void n_client_tick(n_client_t *client, f64 now) {
    if (!client->initialized) return;

    // Receive packets
    u8 recv_buf[N_TRANSPORT_MTU];
    n_address_t from;
    i32 recv_len;

    while ((recv_len = n_transport_recv(&client->transport, &from,
                                        recv_buf, sizeof(recv_buf))) > 0) {
        n_client_process_packet(client, recv_buf, (u32)recv_len, now);
    }

    // State-dependent logic
    switch (client->conn_state) {
        case N_CONN_CONNECTING: {
            // Retry connect request
            if (now - client->last_connect_retry_time >= N_CONNECT_RETRY_SEC) {
                send_connect_request(client);
                client->last_connect_retry_time = now;
            }
            // Timeout
            if (now - client->connect_start_time >= N_CONNECT_TIMEOUT_SEC) {
                client->conn_state = N_CONN_DISCONNECTED;
                n_transport_close(&client->transport);
            }
            break;
        }

        case N_CONN_CONNECTED: {
            // Clock sync
            if (n_clock_should_sync(&client->clock, now)) {
                send_clock_sync(client);
            }

            // Timeout check (not for loopback)
            if (!client->is_loopback &&
                now - client->last_packet_recv_time > N_TIMEOUT_SEC) {
                client->conn_state = N_CONN_DISCONNECTED;
                n_transport_close(&client->transport);
            }
            break;
        }

        default:
            break;
    }
}

// --- Send input ---

void n_client_send_input(n_client_t *client, const n_input_t *input, f64 now) {
    if (client->conn_state != N_CONN_CONNECTED) return;

    // Store in history
    u32 idx = client->input_history_head % N_INPUT_QUEUE_SIZE;
    client->input_history[idx] = *input;
    client->input_history_head++;

    // Build input packet with redundancy
    u8 pkt[N_TRANSPORT_MTU];
    n_packet_header_t hdr = {0};
    hdr.sequence = client->outgoing_sequence++;
    hdr.ack = client->incoming_sequence;
    hdr.ack_bitfield = client->ack_bitfield;
    n_packet_header_write(pkt, &hdr);

    n_bitwriter_t writer;
    n_bitwriter_init(&writer, pkt + N_PACKET_HEADER_SIZE,
                     N_TRANSPORT_MTU - N_PACKET_HEADER_SIZE);

    // Determine how many redundant inputs to send
    u32 available = client->input_history_head;
    u32 count = available < N_INPUT_REDUNDANCY ? available : N_INPUT_REDUNDANCY;
    u32 start_idx = client->input_history_head - count;
    u32 start_tick = client->input_tick > (count - 1) ? client->input_tick - (count - 1) : 0;

    // Input message payload size: 2 bits (count) + 32 bits (start_tick) + count * 9 bytes
    u16 payload_len = (u16)(1 + 4 + count * 9); // approximate byte size
    n_msg_header_write(&writer, N_MSG_INPUT, payload_len);

    n_write_bits(&writer, count - 1, 2); // 0=1 input, 1=2 inputs, 2=3 inputs
    n_write_u32(&writer, start_tick);

    for (u32 i = 0; i < count; i++) {
        u32 hist_idx = (start_idx + i) % N_INPUT_QUEUE_SIZE;
        const n_input_t *inp = &client->input_history[hist_idx];
        n_write_u8(&writer, (u8)inp->forward_move);
        n_write_u8(&writer, (u8)inp->side_move);
        n_write_u16(&writer, inp->yaw);
        n_write_u16(&writer, inp->pitch);
        n_write_u16(&writer, inp->buttons);
        n_write_u8(&writer, inp->weapon_select);
    }

    // Terminate
    n_msg_header_write(&writer, N_MSG_NOP, 0);

    u32 total = N_PACKET_HEADER_SIZE + n_bitwriter_bytes_written(&writer);
    n_transport_send(&client->transport, &client->server_address, pkt, total);

    client->input_tick++;
    client->stats.packets_sent++;
    client->stats.bytes_sent += total;

    QK_UNUSED(now);
}

// --- Interpolation ---

// Find two snapshots that bracket the given render tick
static bool find_interp_pair(const n_client_t *client, f64 render_tick,
                              const n_snapshot_t **out_a, const n_snapshot_t **out_b) {
    if (client->interp_count < 2) return false;

    // Find newest and oldest ticks, then find the pair that brackets render_tick
    const n_snapshot_t *best_a = NULL;
    const n_snapshot_t *best_b = NULL;

    for (u32 i = 0; i < client->interp_count; i++) {
        u32 idx = (client->interp_write - 1 - i + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
        const n_snapshot_t *snap = &client->interp_snapshots[idx];

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

    // Fallback: if render_tick is beyond all snapshots, use the two most recent
    if (!best_b && client->interp_count >= 2) {
        const n_snapshot_t *newest = NULL;
        const n_snapshot_t *second = NULL;
        for (u32 i = 0; i < client->interp_count; i++) {
            u32 idx = (client->interp_write - 1 - i + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
            const n_snapshot_t *snap = &client->interp_snapshots[idx];
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

// Lerp a single f32 value
static f32 lerpf(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

// Lerp 16-bit quantized angle (shortest arc) to float degrees
static f32 lerp_angle(u16 angle_a, u16 angle_b, f32 t) {
    f32 degrees_a = (f32)angle_a * (360.0f / 65536.0f);
    f32 degrees_b = (f32)angle_b * (360.0f / 65536.0f);

    f32 diff = degrees_b - degrees_a;
    if (diff > 180.0f) diff -= 360.0f;
    if (diff < -180.0f) diff += 360.0f;

    return degrees_a + diff * t;
}

// Dequantize position from 15.1 fixed-point
static f32 dequant_pos(i16 quantized) {
    return (f32)quantized * 0.5f;
}

// Dequantize velocity (1 unit/sec precision)
static f32 dequant_vel(i16 quantized) {
    return (f32)quantized;
}

void n_client_interpolate(n_client_t *client, f64 render_time) {
    if (client->conn_state != N_CONN_CONNECTED) return;

    f64 render_tick = render_time * (f64)N_TICK_RATE;

    const n_snapshot_t *snap_a = NULL;
    const n_snapshot_t *snap_b = NULL;

    if (!find_interp_pair(client, render_tick, &snap_a, &snap_b)) {
        N_DBG("interp: no pair for render_tick=%.2f, interp_count=%u (fallback to latest)",
              render_tick, client->interp_count);
        client->interp_diag.valid = false;
        client->interp_diag.render_tick = render_tick;
        client->interp_diag.interp_count = client->interp_count;
        // Not enough data to interpolate. If we have at least one snapshot,
        // use it directly.
        if (client->interp_count > 0) {
            u32 idx = (client->interp_write - 1 + N_INTERP_BUFFER_SIZE) % N_INTERP_BUFFER_SIZE;
            const n_snapshot_t *snap = &client->interp_snapshots[idx];
            for (u32 id = 0; id < N_MAX_ENTITIES; id++) {
                qk_interp_entity_t *interp_entity = &client->interp_state.entities[id];
                if (n_snapshot_has_entity(snap, (u8)id)) {
                    const n_entity_state_t *entity = &snap->entities[id];
                    interp_entity->pos_x = dequant_pos(entity->pos_x);
                    interp_entity->pos_y = dequant_pos(entity->pos_y);
                    interp_entity->pos_z = dequant_pos(entity->pos_z);
                    interp_entity->vel_x = dequant_vel(entity->vel_x);
                    interp_entity->vel_y = dequant_vel(entity->vel_y);
                    interp_entity->vel_z = dequant_vel(entity->vel_z);
                    interp_entity->yaw = (f32)entity->yaw * (360.0f / 65536.0f);
                    interp_entity->pitch = (f32)entity->pitch * (360.0f / 65536.0f);
                    interp_entity->entity_type = entity->entity_type;
                    interp_entity->flags = entity->flags;
                    interp_entity->health = entity->health;
                    interp_entity->armor = entity->armor;
                    interp_entity->weapon = entity->weapon;
                    interp_entity->ammo = entity->ammo;
                    interp_entity->active = true;
                } else {
                    interp_entity->active = false;
                }
            }
        }
        return;
    }

    // Compute interpolation factor
    f32 t = 0.0f;
    if (snap_b->tick != snap_a->tick) {
        t = (f32)(render_tick - (f64)snap_a->tick) /
            (f32)(snap_b->tick - snap_a->tick);
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Fill interpolation diagnostics
    client->interp_diag.valid = true;
    client->interp_diag.fallback = (snap_a->tick > (u32)render_tick);
    client->interp_diag.snap_a_tick = snap_a->tick;
    client->interp_diag.snap_b_tick = snap_b->tick;
    client->interp_diag.t = t;
    client->interp_diag.render_tick = render_tick;
    client->interp_diag.interp_count = client->interp_count;

    for (u32 id = 0; id < N_MAX_ENTITIES; id++) {
        qk_interp_entity_t *interp_entity = &client->interp_state.entities[id];
        bool in_a = n_snapshot_has_entity(snap_a, (u8)id);
        bool in_b = n_snapshot_has_entity(snap_b, (u8)id);

        if (in_a && in_b) {
            const n_entity_state_t *entity_a = &snap_a->entities[id];
            const n_entity_state_t *entity_b = &snap_b->entities[id];

            // Detect teleport: toggle bit differs between snapshots.
            // Robust against dropped packets -- any two snapshots spanning
            // a teleport will have different flag values.
            bool teleported = ((entity_a->flags ^ entity_b->flags) & QK_ENT_FLAG_TELEPORTED) != 0;

            if (teleported) {
                // Snap to destination -- no lerp
                interp_entity->pos_x = dequant_pos(entity_b->pos_x);
                interp_entity->pos_y = dequant_pos(entity_b->pos_y);
                interp_entity->pos_z = dequant_pos(entity_b->pos_z);
                interp_entity->vel_x = dequant_vel(entity_b->vel_x);
                interp_entity->vel_y = dequant_vel(entity_b->vel_y);
                interp_entity->vel_z = dequant_vel(entity_b->vel_z);
                interp_entity->yaw = (f32)entity_b->yaw * (360.0f / 65536.0f);
                interp_entity->pitch = (f32)entity_b->pitch * (360.0f / 65536.0f);
            } else {
                // Normal interpolation
                interp_entity->pos_x = lerpf(dequant_pos(entity_a->pos_x), dequant_pos(entity_b->pos_x), t);
                interp_entity->pos_y = lerpf(dequant_pos(entity_a->pos_y), dequant_pos(entity_b->pos_y), t);
                interp_entity->pos_z = lerpf(dequant_pos(entity_a->pos_z), dequant_pos(entity_b->pos_z), t);
                interp_entity->vel_x = lerpf(dequant_vel(entity_a->vel_x), dequant_vel(entity_b->vel_x), t);
                interp_entity->vel_y = lerpf(dequant_vel(entity_a->vel_y), dequant_vel(entity_b->vel_y), t);
                interp_entity->vel_z = lerpf(dequant_vel(entity_a->vel_z), dequant_vel(entity_b->vel_z), t);
                interp_entity->yaw = lerp_angle(entity_a->yaw, entity_b->yaw, t);
                interp_entity->pitch = lerp_angle(entity_a->pitch, entity_b->pitch, t);
            }

            // Discrete fields: always use the newer snapshot (B).
            // Delaying until t >= 0.5 causes flags/weapon to appear
            // stale for up to half a tick interval.
            const n_entity_state_t *src = entity_b;
            interp_entity->entity_type = src->entity_type;
            interp_entity->flags = src->flags;
            interp_entity->health = src->health;
            interp_entity->armor = src->armor;
            interp_entity->weapon = src->weapon;
            interp_entity->ammo = src->ammo;
            interp_entity->active = true;

        } else if (in_b && !in_a) {
            // Entity spawned: show immediately from snap_b.
            // The old threshold (render_tick >= snap_b->tick - 0.5)
            // delayed new entities for many frames, making rockets
            // and other projectiles invisible at spawn.
            const n_entity_state_t *entity_b = &snap_b->entities[id];
            interp_entity->pos_x = dequant_pos(entity_b->pos_x);
            interp_entity->pos_y = dequant_pos(entity_b->pos_y);
            interp_entity->pos_z = dequant_pos(entity_b->pos_z);
            interp_entity->vel_x = dequant_vel(entity_b->vel_x);
            interp_entity->vel_y = dequant_vel(entity_b->vel_y);
            interp_entity->vel_z = dequant_vel(entity_b->vel_z);
            interp_entity->yaw = (f32)entity_b->yaw * (360.0f / 65536.0f);
            interp_entity->pitch = (f32)entity_b->pitch * (360.0f / 65536.0f);
            interp_entity->entity_type = entity_b->entity_type;
            interp_entity->flags = entity_b->flags;
            interp_entity->health = entity_b->health;
            interp_entity->armor = entity_b->armor;
            interp_entity->weapon = entity_b->weapon;
            interp_entity->ammo = entity_b->ammo;
            interp_entity->active = true;

        } else if (in_a && !in_b) {
            // Entity despawned: keep showing until past B
            if (render_tick < (f64)snap_b->tick) {
                const n_entity_state_t *entity_a = &snap_a->entities[id];
                interp_entity->pos_x = dequant_pos(entity_a->pos_x);
                interp_entity->pos_y = dequant_pos(entity_a->pos_y);
                interp_entity->pos_z = dequant_pos(entity_a->pos_z);
                interp_entity->vel_x = dequant_vel(entity_a->vel_x);
                interp_entity->vel_y = dequant_vel(entity_a->vel_y);
                interp_entity->vel_z = dequant_vel(entity_a->vel_z);
                interp_entity->yaw = (f32)entity_a->yaw * (360.0f / 65536.0f);
                interp_entity->pitch = (f32)entity_a->pitch * (360.0f / 65536.0f);
                interp_entity->entity_type = entity_a->entity_type;
                interp_entity->flags = entity_a->flags;
                interp_entity->health = entity_a->health;
                interp_entity->armor = entity_a->armor;
                interp_entity->weapon = entity_a->weapon;
                interp_entity->ammo = entity_a->ammo;
                interp_entity->active = true;
            } else {
                interp_entity->active = false;
            }

        } else {
            interp_entity->active = false;
        }
    }
}
