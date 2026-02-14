/*
 * QUICKEN Engine - Netcode Internal Header
 *
 * All internal types, constants, and function declarations for the netcode module.
 * NOT part of the public API. Only included by netcode .c files.
 */

#ifndef N_INTERNAL_H
#define N_INTERNAL_H

#include "quicken.h"
#include "qk_types.h"
#include "netcode/qk_netcode.h"
#include "netcode/n_types.h"

#include <string.h>

/* ---- Constants ---- */

#define N_TRANSPORT_MTU         1400
#define N_LOOPBACK_QUEUE_SIZE   64
#define N_MAX_CLIENTS           16
#define N_MAX_ENTITIES          256
#define N_SNAPSHOT_HISTORY      64
#define N_INPUT_QUEUE_SIZE      64
#define N_INTERP_BUFFER_SIZE    32

/* Timing */
#define N_TICK_RATE             128
#define N_TICK_INTERVAL         (1.0 / 128.0)
#define N_INTERP_DELAY_DEFAULT  0.020
#define N_INTERP_DELAY_MIN      0.0078
#define N_INTERP_DELAY_MAX      0.100

/* Connection */
#define N_CONNECT_RETRY_SEC     0.5
#define N_CONNECT_TIMEOUT_SEC   10.0
#define N_TIMEOUT_SEC           30.0
#define N_DISCONNECT_LINGER_SEC 1.0

/* Clock sync */
#define N_CLOCK_SYNC_INTERVAL   1.0
#define N_CLOCK_SYNC_FAST       0.1
#define N_CLOCK_SYNC_SAMPLES    16
#define N_CLOCK_CONVERGE_COUNT  4

/* Reliable channel */
#define N_RELIABLE_RETRANSMIT_SEC 0.2
#define N_RELIABLE_MAX_PAYLOAD  4096

/* Input redundancy */
#define N_INPUT_REDUNDANCY      3

/* Entity field count for delta bitmask */
#define N_ENTITY_FIELD_COUNT    12

/* ---- Message types ---- */

typedef enum {
    N_MSG_NOP               = 0,
    N_MSG_INPUT             = 1,
    N_MSG_SNAPSHOT          = 2,
    N_MSG_COMMAND           = 3,
    N_MSG_CLOCK_SYNC        = 4,
    N_MSG_DISCONNECT        = 5,
    N_MSG_CONNECT_REQUEST   = 6,
    N_MSG_CONNECT_CHALLENGE = 7,
    N_MSG_CONNECT_RESPONSE  = 8,
    N_MSG_CONNECT_ACCEPTED  = 9,
    N_MSG_CONNECT_REJECTED  = 10,
    N_MSG_COUNT
} n_msg_type_t;

/* ---- Connection state (internal mirror of qk_conn_state_t) ---- */

typedef enum {
    N_CONN_DISCONNECTED     = QK_CONN_DISCONNECTED,
    N_CONN_CONNECTING       = QK_CONN_CONNECTING,
    N_CONN_CONNECTED        = QK_CONN_CONNECTED,
    N_CONN_DISCONNECTING    = QK_CONN_DISCONNECTING
} n_conn_state_t;

/* ---- Transport ---- */

typedef enum {
    N_TRANSPORT_LOOPBACK,
    N_TRANSPORT_UDP
} n_transport_type_t;

typedef struct {
    u32 ip;
    u16 port;
} n_address_t;

typedef struct {
    u16 len;
    u8  data[N_TRANSPORT_MTU];
} n_loopback_packet_t;

typedef struct {
    n_loopback_packet_t packets[N_LOOPBACK_QUEUE_SIZE];
    u32 head;
    u32 tail;
} n_loopback_queue_t;

typedef struct {
    n_transport_type_t type;
    int                socket_fd;
    n_loopback_queue_t *send_queue;
    n_loopback_queue_t *recv_queue;
} n_transport_t;

/* Transport API */
bool n_transport_open_udp(n_transport_t *t, u16 bind_port);
void n_transport_open_loopback(n_transport_t *client, n_transport_t *server,
                               n_loopback_queue_t *q1, n_loopback_queue_t *q2);
void n_transport_close(n_transport_t *t);
i32  n_transport_send(n_transport_t *t, const n_address_t *to,
                      const void *data, u32 len);
i32  n_transport_recv(n_transport_t *t, n_address_t *from,
                      void *data, u32 max_len);

/* ---- Platform ---- */

bool n_platform_init(void);
void n_platform_shutdown(void);
f64  n_platform_time(void);

/* ---- Bitpacker ---- */

typedef struct {
    u8     *buffer;
    u32     bit_pos;
    u32     max_bits;
} n_bitwriter_t;

typedef struct {
    const u8 *buffer;
    u32       bit_pos;
    u32       max_bits;
} n_bitreader_t;

void n_bitwriter_init(n_bitwriter_t *w, u8 *buffer, u32 max_bytes);
void n_write_bits(n_bitwriter_t *w, u32 value, u32 num_bits);
void n_write_bool(n_bitwriter_t *w, bool value);
void n_write_u8(n_bitwriter_t *w, u8 value);
void n_write_u16(n_bitwriter_t *w, u16 value);
void n_write_u32(n_bitwriter_t *w, u32 value);
void n_write_i16(n_bitwriter_t *w, i16 value);
void n_write_f64(n_bitwriter_t *w, f64 value);
u32  n_bitwriter_bytes_written(const n_bitwriter_t *w);

void n_bitreader_init(n_bitreader_t *r, const u8 *buffer, u32 num_bytes);
u32  n_read_bits(n_bitreader_t *r, u32 num_bits);
bool n_read_bool(n_bitreader_t *r);
u8   n_read_u8(n_bitreader_t *r);
u16  n_read_u16(n_bitreader_t *r);
u32  n_read_u32(n_bitreader_t *r);
i16  n_read_i16(n_bitreader_t *r);
f64  n_read_f64(n_bitreader_t *r);
bool n_bitreader_overflowed(const n_bitreader_t *r);

/* ---- Protocol ---- */

#define N_PACKET_HEADER_SIZE 8

typedef struct {
    u16 sequence;
    u16 ack;
    u32 ack_bitfield;
} n_packet_header_t;

typedef struct {
    u8   type;       /* n_msg_type_t (4 bits) */
    u16  length;     /* payload length (12 bits) */
} n_msg_header_t;

static inline bool n_sequence_more_recent(u16 a, u16 b) {
    return (i16)(a - b) > 0;
}

void n_packet_header_write(u8 *buf, const n_packet_header_t *hdr);
void n_packet_header_read(const u8 *buf, n_packet_header_t *hdr);
void n_msg_header_write(n_bitwriter_t *w, u8 type, u16 length);
bool n_msg_header_read(n_bitreader_t *r, n_msg_header_t *hdr);

/* ---- Reliable channel ---- */

typedef struct {
    u16     reliable_sequence;
    u16     reliable_ack;
    u8      unacked_buffer[N_RELIABLE_MAX_PAYLOAD];
    u16     unacked_len;
    u16     unacked_sequence;
    bool    has_unacked;
    f64     last_send_time;
} n_reliable_channel_t;

void n_reliable_init(n_reliable_channel_t *ch);
void n_reliable_send(n_reliable_channel_t *ch, const u8 *data, u16 len);
bool n_reliable_needs_retransmit(const n_reliable_channel_t *ch, f64 now);
void n_reliable_on_ack(n_reliable_channel_t *ch, u16 ack_seq);
void n_reliable_write_to_packet(n_reliable_channel_t *ch, n_bitwriter_t *w, f64 now);
bool n_reliable_read_from_packet(n_reliable_channel_t *ch, n_bitreader_t *r,
                                  u16 *out_len);

/* ---- Snapshot ---- */

typedef struct {
    u32                 tick;
    u32                 entity_count;
    u64                 entity_mask[N_MAX_ENTITIES / 64];
    n_entity_state_t    entities[N_MAX_ENTITIES];
} n_snapshot_t;

typedef struct {
    n_snapshot_t    snapshots[N_SNAPSHOT_HISTORY];
    u32             current_index;
} n_snapshot_buffer_t;

void n_snapshot_init(n_snapshot_t *snap);
void n_snapshot_set_entity(n_snapshot_t *snap, u8 id, const n_entity_state_t *state);
void n_snapshot_remove_entity(n_snapshot_t *snap, u8 id);
bool n_snapshot_has_entity(const n_snapshot_t *snap, u8 id);

u32 n_snapshot_delta_encode(const n_snapshot_t *baseline, const n_snapshot_t *current,
                            u8 *out_buf, u32 max_bytes);
bool n_snapshot_delta_decode(const n_snapshot_t *baseline, n_snapshot_t *out,
                             const u8 *data, u32 data_len,
                             u32 current_tick);

/* ---- Clock sync ---- */

typedef struct {
    f64     offset_samples[N_CLOCK_SYNC_SAMPLES];
    f64     rtt_samples[N_CLOCK_SYNC_SAMPLES];
    u32     sample_count;
    u32     sample_index;
    f64     smoothed_offset;
    f64     smoothed_rtt;
    f64     last_sync_time;
    bool    converged;
} n_clock_state_t;

void n_clock_init(n_clock_state_t *clk);
void n_clock_add_sample(n_clock_state_t *clk, f64 rtt, f64 offset);
bool n_clock_should_sync(const n_clock_state_t *clk, f64 now);
void n_clock_mark_sent(n_clock_state_t *clk, f64 now);

/* ---- Stats ---- */

typedef struct {
    u64     packets_sent;
    u64     packets_received;
    u64     packets_dropped;
    u64     bytes_sent;
    u64     bytes_received;
    u64     snapshots_delta;
    u64     snapshots_full;
    u64     inputs_received;
    u64     inputs_duplicated;
    u64     inputs_late;
} n_stats_t;

/* ---- Server client slot ---- */

typedef struct {
    n_conn_state_t  state;
    n_address_t     address;
    n_transport_t   transport;
    u8              client_id;
    bool            is_loopback;

    /* Protocol state */
    u16             outgoing_sequence;
    u16             incoming_sequence;
    u32             ack_bitfield;

    /* Timing */
    f64             last_packet_recv_time;
    f64             connect_start_time;
    u32             client_challenge;
    u32             server_challenge;

    /* Snapshot delta state */
    u32             last_acked_snapshot_tick;

    /* Input queue */
    n_input_t       input_queue[N_INPUT_QUEUE_SIZE];
    u32             input_queue_head;
    u32             input_queue_tail;
    u32             last_input_tick;
    n_input_t       last_input;

    /* Reliable channel */
    n_reliable_channel_t reliable;

    /* Clock sync (server-side) */
    f64             clock_offset;
    i32             rtt_ms;

    /* Disconnect linger */
    f64             disconnect_start_time;
} n_client_slot_t;

/* ---- Server ---- */

typedef struct {
    n_transport_t       transport;
    n_client_slot_t     clients[N_MAX_CLIENTS];
    u32                 client_count;
    u32                 tick;
    f64                 tick_time;
    f64                 tick_interval;
    n_snapshot_buffer_t snapshot_buffer;
    n_snapshot_t        current_snapshot;
    u8                  packet_buffer[N_TRANSPORT_MTU];
    n_stats_t           stats;
    bool                initialized;
    u16                 server_port;
    u32                 max_clients;

    /* Loopback queues (one pair per possible loopback client) */
    n_loopback_queue_t  loopback_queues[N_MAX_CLIENTS][2];
} n_server_t;

/* Server API */
void n_server_init(n_server_t *srv, u16 port, u32 max_clients, f64 tick_rate);
void n_server_tick(n_server_t *srv, f64 now);
void n_server_shutdown(n_server_t *srv);
void n_server_process_packet(n_server_t *srv, u8 *data, u32 len,
                             const n_address_t *from, n_transport_t *via);
i32  n_server_find_client_by_address(const n_server_t *srv, const n_address_t *addr);
i32  n_server_allocate_slot(n_server_t *srv);
void n_server_disconnect_client(n_server_t *srv, u32 slot);
void n_server_send_to_client(n_server_t *srv, u32 slot, const u8 *data, u32 len);
void n_server_broadcast_snapshots(n_server_t *srv);
void n_server_handle_connect_request(n_server_t *srv, u32 slot_or_new,
                                     const u8 *payload, u32 payload_len,
                                     const n_address_t *from, n_transport_t *via);
int  n_server_connect_loopback(n_server_t *srv);

/* ---- Client ---- */

typedef struct {
    n_transport_t       transport;
    n_conn_state_t      conn_state;
    u8                  client_id;
    n_address_t         server_address;

    /* Protocol */
    u16                 outgoing_sequence;
    u16                 incoming_sequence;
    u32                 ack_bitfield;

    /* Reliable channel */
    n_reliable_channel_t reliable;

    /* Clock sync */
    n_clock_state_t     clock;
    f64                 clock_sync_send_time;

    /* Interpolation */
    n_snapshot_t        interp_snapshots[N_INTERP_BUFFER_SIZE];
    u32                 interp_count;
    u32                 interp_write;
    n_snapshot_t        baseline_snapshot;
    bool                has_baseline;
    qk_interp_state_t  interp_state;
    f64                 interp_delay;

    /* Input */
    n_input_t           input_history[N_INPUT_QUEUE_SIZE];
    u32                 input_history_head;
    u32                 input_tick;

    /* Prediction reconciliation */
    u32                 last_server_cmd_ack;

    /* Timing */
    f64                 last_packet_recv_time;
    f64                 connect_start_time;
    f64                 last_connect_retry_time;
    u32                 client_challenge;
    u32                 server_challenge;

    /* Packet buffer */
    u8                  packet_buffer[N_TRANSPORT_MTU];

    n_stats_t           stats;
    bool                initialized;
    bool                is_loopback;

    /* Reference to server for loopback */
    n_server_t          *loopback_server;
} n_client_t;

/* Client API */
void n_client_init(n_client_t *cl, f64 interp_delay);
void n_client_tick(n_client_t *cl, f64 now);
void n_client_shutdown(n_client_t *cl);
void n_client_connect_remote(n_client_t *cl, const char *address, u16 port);
void n_client_connect_local(n_client_t *cl, n_server_t *srv);
void n_client_disconnect(n_client_t *cl);
void n_client_interpolate(n_client_t *cl, f64 render_time);
void n_client_send_input(n_client_t *cl, const n_input_t *input, f64 now);
void n_client_process_packet(n_client_t *cl, const u8 *data, u32 len, f64 now);

/* ---- Simple PRNG for challenge generation ---- */
u32 n_random_u32(void);

#endif /* N_INTERNAL_H */
