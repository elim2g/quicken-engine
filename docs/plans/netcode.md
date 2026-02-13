# QUICKEN Netcode Implementation Plan

## Vertical Slice Scope

This plan covers the first implementation pass of QUICKEN's netcode layer. The vertical slice delivers:

- Client/server architecture where clients ALWAYS connect to a server (even local play).
- Transport abstraction: loopback (function call) and UDP share the same interface.
- 128 Hz authoritative server tick.
- Delta-compressed snapshots from server to clients.
- Client-side interpolation buffer (~20ms configurable).
- Client input transmission every tick.
- Basic clock synchronization sufficient for interpolation timing.
- Connection lifecycle: connect, disconnect, timeout.
- Support for up to 24 simultaneous players, and a further 8 spectators totalling 32 simultaneous connections.

NOT in vertical slice: rollback, prediction, reconciliation, extrapolation, anti-cheat. The architecture is designed so these slot in cleanly later.

---

## 1. Transport Abstraction

### Design

Every connection between client and server goes through a `n_transport_t` interface. There are exactly two implementations:

1. **Loopback**: For local/singleplayer. Data is copied into a ring buffer via function call. Zero serialization overhead -- but data still goes through the same packet encode/decode path so the code path is identical.
2. **UDP**: Standard non-blocking UDP socket. Platform-abstracted (Winsock on Windows, BSD sockets on Linux).

The key insight: the transport only moves raw byte buffers. It has no knowledge of packet contents. All framing, sequencing, and reliability live above it.

### Interface

```c
/* include/netcode/n_transport.h */

#define N_TRANSPORT_MTU         1400    /* max bytes per packet (safe UDP) */
#define N_LOOPBACK_QUEUE_SIZE   64      /* packets in loopback ring buffer */
#define N_LOOPBACK_BUFFER_SIZE  (N_TRANSPORT_MTU * N_LOOPBACK_QUEUE_SIZE)

typedef enum {
    N_TRANSPORT_LOOPBACK,
    N_TRANSPORT_UDP
} n_transport_type_t;

typedef struct {
    u32 ip;         /* host byte order, 0 for loopback */
    u16 port;       /* host byte order, 0 for loopback */
} n_address_t;

/* A single queued packet in the loopback ring buffer */
typedef struct {
    u16 len;
    u8  data[N_TRANSPORT_MTU];
} n_loopback_packet_t;

/* Loopback channel: one direction of the pipe */
typedef struct {
    n_loopback_packet_t packets[N_LOOPBACK_QUEUE_SIZE];
    u32 head;   /* write position */
    u32 tail;   /* read position */
} n_loopback_queue_t;

typedef struct {
    n_transport_type_t type;

    /* UDP fields */
    int                socket_fd;   /* -1 if not UDP */

    /* Loopback fields */
    n_loopback_queue_t *send_queue; /* points to peer's recv queue */
    n_loopback_queue_t *recv_queue; /* our recv queue */
} n_transport_t;

/* Lifetime */
bool n_transport_open_udp(n_transport_t *t, u16 bind_port);
void n_transport_open_loopback(n_transport_t *client, n_transport_t *server);
void n_transport_close(n_transport_t *t);

/* I/O -- returns bytes sent/received, 0 if nothing, -1 on error */
i32  n_transport_send(n_transport_t *t, const n_address_t *to,
                      const void *data, u32 len);
i32  n_transport_recv(n_transport_t *t, n_address_t *from,
                      void *data, u32 max_len);
```

### Loopback Detail

`n_transport_open_loopback` creates two transports that are cross-wired: client's send_queue points to server's recv_queue and vice versa. Writes are a memcpy into the ring buffer. Reads are a memcpy out. No system calls. No threads. Data is available instantly on the next `n_transport_recv` call.

```
Client transport:
  send_queue -> server_recv_queue   (client writes here)
  recv_queue -> client_recv_queue   (client reads here)

Server transport:
  send_queue -> client_recv_queue   (server writes here)
  recv_queue -> server_recv_queue   (server reads here)
```

Both loopback queues are statically allocated alongside the transport structs (no heap allocation for loopback).

### UDP Detail

Non-blocking UDP socket. On Windows: `ioctlsocket(fd, FIONBIO, &one)`. On Linux: `fcntl(fd, F_SETFL, O_NONBLOCK)`. Platform differences are isolated inside `n_transport_open_udp`.

The server binds one UDP socket and sends/receives to all clients through it, discriminating by `n_address_t`. Clients bind their own socket (port 0 = OS-assigned).

### Platform Socket Abstraction

```c
/* src/netcode/n_platform.c -- compiled once, platform-selected via #ifdef */

#ifdef QUICKEN_PLATFORM_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET n_raw_socket_t;
    #define N_INVALID_SOCKET INVALID_SOCKET
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <fcntl.h>
    #include <unistd.h>
    typedef int n_raw_socket_t;
    #define N_INVALID_SOCKET (-1)
#endif

bool n_platform_init(void);     /* WSAStartup on Windows, noop on Linux */
void n_platform_shutdown(void); /* WSACleanup on Windows, noop on Linux */
```

---

## 2. Client/Server Architecture

### High-Level Model

```
  +---------+                           +---------+
  | Client  | <--- snapshots ---------- | Server  |
  |         | ---- inputs ------------> |         |
  +---------+                           +---------+
       |                                     |
       v                                     v
  interpolation                        authoritative
  buffer -> render                     simulation
```

**Server** owns:
- The authoritative game state (entity positions, health, weapons, etc.)
- The tick counter (monotonically increasing `u32`)
- Per-client connection state, input queues, and snapshot history
- Game rules (scoring, rounds) -- transported reliably, not implemented in netcode

**Client** owns:
- Its local connection to the server (transport + protocol state)
- An interpolation buffer of recent snapshots
- Interpolated entity states for rendering
- Its own input sampling and transmission

For the vertical slice, the client does NOT own a predicted game state. It renders purely from interpolated server snapshots. The architecture reserves a slot for prediction state that will be filled later.

### Local Play Flow

```
main() {
    n_server_t server;
    n_client_t client;

    n_server_init(&server, &server_config);  /* creates server state */
    n_client_init(&client, &client_config);  /* creates client state */
    n_client_connect_local(&client, &server); /* loopback transport */

    while (running) {
        /* Server tick (128 Hz, fixed timestep) */
        while (server_accumulator >= SERVER_TICK_INTERVAL) {
            n_server_tick(&server);
            server_accumulator -= SERVER_TICK_INTERVAL;
        }

        /* Client tick (128 Hz, processes received snapshots) */
        while (client_accumulator >= CLIENT_TICK_INTERVAL) {
            n_client_tick(&client);
            client_accumulator -= CLIENT_TICK_INTERVAL;
        }

        /* Client generates interpolated state for rendering */
        n_client_interpolate(&client, render_time);

        /* Render at uncapped framerate */
        renderer_draw(client.interp_state);
    }
}
```

### Networked Play Flow

Identical to above except:
- `n_client_connect_remote(&client, "192.168.1.5", 27960)` instead of `connect_local`.
- Server runs in its own process (dedicated server) or in the host's process (listen server).
- For listen server, one client slot is loopback, others are UDP.

---

## 3. Connection Lifecycle

### States

```c
typedef enum {
    N_CONN_DISCONNECTED,    /* no connection */
    N_CONN_CONNECTING,      /* challenge/response handshake */
    N_CONN_CONNECTED,       /* fully connected, exchanging data */
    N_CONN_DISCONNECTING    /* graceful disconnect in progress */
} n_conn_state_t;
```

### Connection Handshake (3-way, like Q3)

```
Client -> Server:   CONNECT_REQUEST  { client_challenge: u32 }
Server -> Client:   CONNECT_CHALLENGE { server_challenge: u32, client_challenge: u32 }
Client -> Server:   CONNECT_RESPONSE { server_challenge: u32, client_challenge: u32 }
Server -> Client:   CONNECT_ACCEPTED { client_id: u8, server_tick: u32 }
```

- `client_challenge` and `server_challenge` are random u32 values. They prevent spoofed connects.
- On `CONNECT_ACCEPTED`, the server assigns a `client_id` (0..15) and the client begins normal operation.
- If the server is full, it responds with `CONNECT_REJECTED { reason: u8 }`.
- The client retransmits `CONNECT_REQUEST` every 500ms until accepted, rejected, or timed out (10s).

For loopback connections, the handshake is still executed (same code path) but completes within a single frame since packets are instantly available.

### Connection Slot (Server-Side)

```c
#define N_MAX_CLIENTS           16
#define N_TIMEOUT_SECONDS       30
#define N_SNAPSHOT_HISTORY      64  /* must be power of 2 */
#define N_INPUT_QUEUE_SIZE      64  /* must be power of 2 */

typedef struct {
    n_conn_state_t  state;
    n_address_t     address;
    n_transport_t   transport;  /* for loopback; UDP clients share server socket */
    u8              client_id;

    /* Protocol state */
    u16             outgoing_sequence;
    u16             incoming_sequence;
    u32             ack_bitfield;       /* bitmask of received packets */

    /* Timing */
    f64             last_packet_recv_time;
    f64             connect_start_time;
    u32             challenge;

    /* Snapshot delta state */
    u32             last_acked_snapshot_tick;  /* baseline for delta */
    n_snapshot_t    snapshot_history[N_SNAPSHOT_HISTORY];

    /* Input queue -- server stores recent inputs from this client */
    n_input_t       input_queue[N_INPUT_QUEUE_SIZE];
    u32             input_queue_head;
    u32             input_queue_tail;
    u32             last_input_tick;

    /* Clock sync */
    f64             clock_offset;       /* estimated offset: client_time - server_time */
    i32             rtt_ms;             /* round-trip time in ms */
} n_client_slot_t;
```

### Timeout

- Server checks `last_packet_recv_time` every tick. If `current_time - last_packet_recv_time > N_TIMEOUT_SECONDS`, the slot is freed.
- Client checks the same for the server. If no packets received for `N_TIMEOUT_SECONDS`, it disconnects.

### Disconnect

Either side can send a `DISCONNECT` message (reliable). The sender transitions to `N_CONN_DISCONNECTING` and retransmits the disconnect message for 1 second to ensure delivery, then transitions to `N_CONN_DISCONNECTED`.

---

## 4. Packet Protocol

### Packet Header

Every packet starts with an 8-byte header:

```
Byte  0-1:  sequence       (u16, wraps)
Byte  2-3:  ack            (u16, last received remote sequence)
Byte  4-7:  ack_bitfield   (u32, bit N = received ack-N-1)
```

Total: 8 bytes. This is a standard bitfield ack scheme (like ENet / Gaffer on Games). It acknowledges up to 33 packets (ack + 32 trailing bits) with no extra overhead.

**Sequence wrap handling**: Sequences are compared with `(i16)(a - b) > 0` to handle 16-bit wraparound correctly.

```c
static inline bool n_sequence_more_recent(u16 a, u16 b) {
    return (i16)(a - b) > 0;
}
```

### Message Framing

After the header, the packet contains one or more messages:

```
Bits 0-3:   message_type   (4 bits, 0..15)
Bits 4-15:  payload_length (12 bits, 0..4095 bytes)
Bytes:      payload        (payload_length bytes)
```

Messages are packed end-to-end. Receiver reads type + length, then advances by length. A NOP (type 0) with length 0 terminates the message stream.

### Message Types

```c
typedef enum {
    N_MSG_NOP               = 0,    /* end of messages / padding */
    N_MSG_INPUT             = 1,    /* client -> server: player input */
    N_MSG_SNAPSHOT          = 2,    /* server -> client: delta snapshot */
    N_MSG_COMMAND           = 3,    /* reliable ordered command (both dirs) */
    N_MSG_CLOCK_SYNC        = 4,    /* bidirectional clock probe */
    N_MSG_DISCONNECT        = 5,    /* graceful disconnect */
    N_MSG_CONNECT_REQUEST   = 6,    /* connection handshake messages */
    N_MSG_CONNECT_CHALLENGE = 7,
    N_MSG_CONNECT_RESPONSE  = 8,
    N_MSG_CONNECT_ACCEPTED  = 9,
    N_MSG_CONNECT_REJECTED  = 10,
    N_MSG_COUNT
} n_msg_type_t;
```

### Reliable Channel (for N_MSG_COMMAND)

Commands (chat, player info, game state changes like "round start") require reliable ordered delivery. This is implemented as a simple stop-and-wait protocol layered on top:

```c
typedef struct {
    u16     reliable_sequence;          /* monotonic per-channel sequence */
    u16     reliable_ack;               /* last received reliable seq from peer */
    u8      unacked_buffer[4096];       /* retransmit buffer for unacked msg */
    u16     unacked_len;
    u16     unacked_sequence;
    f64     last_send_time;
} n_reliable_channel_t;
```

- Sender writes `[reliable_sequence: u16][payload]` inside the COMMAND message.
- Receiver echoes `reliable_ack` in every packet header... **No** -- to keep the packet header minimal, the reliable ack is piggy-backed in the COMMAND message itself: if the receiver has nothing to send reliably, it sends `[N_MSG_COMMAND][len=2][reliable_ack: u16]` with empty payload.
- Actually, simpler: the reliable_ack is included as the first 2 bytes of every COMMAND message payload. Non-COMMAND packets don't carry it.
- If the sender doesn't receive an ack within 200ms, it retransmits the current unacked message.
- Only one unacked reliable message in flight at a time (stop-and-wait). This is sufficient for the low-bandwidth reliable stream (a few messages per second at most).

### Packet Budget

At 128 ticks/second with 16 clients:
- Server -> each client: 1 packet per tick = 128 packets/sec.
- Target < 50 KB/s per client = ~390 bytes/packet average.
- With 8-byte header + ~200 byte delta snapshot + overhead, this is comfortable.
- Client -> server: 1 packet per tick = 128 packets/sec.
- Input packet: 8-byte header + 2-byte message header + input payload (~16 bytes) = ~26 bytes.
- At 128 Hz that's ~3.3 KB/s upload per client.

---

## 5. Server-Side: Snapshots, Delta Compression, Broadcast

### Entity State

The unit of synchronization is the entity. Every entity has a compact state representation for networking:

```c
#define N_MAX_ENTITIES          256
#define N_ENTITY_TYPE_BITS      4
#define N_ENTITY_ID_BITS        8

typedef struct {
    u8      entity_id;              /* 0..255 */
    u8      entity_type;            /* player, projectile, item, etc. */
    u8      flags;                  /* bitfield: alive, on_ground, etc. */

    /* Position: quantized to 0.125 unit precision (3 fractional bits) */
    /* Range: +/-4096 units = 13 bits integer + 3 bits frac = 16 bits signed per axis */
    i16     pos_x;                  /* fixed-point 13.3 */
    i16     pos_y;
    i16     pos_z;

    /* Velocity: quantized to 1 unit/sec precision */
    /* Range: +/-4096 units/sec = 13 bits signed */
    i16     vel_x;
    i16     vel_y;
    i16     vel_z;

    /* Orientation: quantized to 360/65536 degree precision */
    u16     yaw;                    /* 0..65535 maps to 0..360 deg */
    u16     pitch;

    /* Player-specific (valid when entity_type == PLAYER) */
    u8      health;                 /* 0..255 */
    u8      armor;                  /* 0..255 */
    u8      weapon;                 /* weapon id */
    u8      ammo;                   /* current weapon ammo, clamped 0..255 */
} n_entity_state_t;
/* Size: 22 bytes per entity, tightly packed */
```

**Quantization rationale:**
- Position at 0.125 unit precision over +/-4096 range covers any Arena FPS map with sub-unit accuracy. 16-bit signed per axis.
- Angles at 16-bit give 0.0055 degree precision, plenty for FPS.
- Health/armor capped at 255 (Quake-era values fit).

### Snapshot

```c
typedef struct {
    u32                 tick;
    u32                 entity_count;                    /* how many valid entities */
    u64                 entity_mask[N_MAX_ENTITIES / 64]; /* bitmask: which IDs exist */
    n_entity_state_t    entities[N_MAX_ENTITIES];         /* indexed by entity_id */
} n_snapshot_t;
```

The server stores the last `N_SNAPSHOT_HISTORY` (64) snapshots in a circular buffer. This is ~64 * (8 + 256*22 + 32) = ~360 KB per client slot. Since entity arrays are indexed by ID, there is no need for a lookup table.

**Correction**: The snapshot_history should be shared (one copy for the whole server), not per-client. Each client only needs to track which snapshot tick it last acknowledged as its delta baseline.

```c
/* Server-wide snapshot ring buffer */
typedef struct {
    n_snapshot_t    snapshots[N_SNAPSHOT_HISTORY];
    u32             current_index;
} n_snapshot_buffer_t;

/* Per-client: just the baseline tick */
/* (already in n_client_slot_t as last_acked_snapshot_tick) */
```

This brings memory to ~360 KB total for snapshot history (shared), which is fine.

### Snapshot Generation

Every server tick:

1. The server captures the authoritative game state into `n_snapshot_t` at the current tick.
2. For each connected client, the server delta-encodes the new snapshot against that client's last-acknowledged baseline snapshot.
3. The delta is bit-packed into the packet and sent.

```c
void n_server_generate_snapshot(n_server_t *server);
void n_server_broadcast_snapshots(n_server_t *server);
```

### Delta Compression

Delta compression compares two snapshots field-by-field and only sends changed fields.

**Per-entity delta encoding:**

```
For each entity_id (0..255):
  1 bit:  entity_present_changed  (did this entity appear/disappear?)
  If changed:
    1 bit: present_now (1 = spawned, 0 = despawned)
    If spawned: full entity state (22 bytes = 176 bits)
  If not changed (entity exists in both):
    1 bit: entity_changed (any field different?)
    If changed:
      field_bitmask: 12 bits (one per field below)
        [0] pos_x      [1] pos_y       [2] pos_z
        [3] vel_x      [4] vel_y       [5] vel_z
        [6] yaw        [7] pitch
        [8] flags      [9] health      [10] armor
        [11] weapon_ammo (weapon + ammo packed together)
      For each set bit in field_bitmask:
        write the new value at its natural bit width
```

**Typical delta size estimate:**
- 16-player game, most players move every tick.
- 16 entities present, ~12 change per tick (some idle).
- Per changed entity: 1 + 1 + 12 + ~64 bits of changed fields = ~78 bits = ~10 bytes.
- 256 entities scanned: 256 bits for presence-unchanged + 12 * 78 bits changed = ~1192 bits = ~149 bytes.
- Well within the 390-byte budget.

**Optimization: only iterate entities that exist.** Use `entity_mask` to skip empty slots entirely. The delta encoder writes the mask diff first, then per-entity deltas for entities that exist in either snapshot.

Revised delta format:

```
[entity_mask_delta]:
  For each u64 word of entity_mask (4 words for 256 entities):
    1 bit: word_changed
    If changed: 64 bits (new mask word)

[per-entity deltas]:
  Iterate entity IDs that exist in EITHER baseline or current snapshot.
  For each such ID:
    If only in current (spawned):  1 bit (1) + full state
    If only in baseline (removed): 1 bit (1) -- presence change already encoded in mask
    If in both:
      1 bit: changed
      If changed: field_bitmask (12 bits) + changed field values
```

### Bitpacker

```c
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
u32  n_bitwriter_bytes_written(const n_bitwriter_t *w);

void n_bitreader_init(n_bitreader_t *r, const u8 *buffer, u32 num_bytes);
u32  n_read_bits(n_bitreader_t *r, u32 num_bits);
bool n_read_bool(n_bitreader_t *r);
bool n_bitreader_overflowed(const n_bitreader_t *r);
```

All multi-bit writes are little-endian bit order. The bitpacker operates on the packet payload area (after the 8-byte packet header and 2-byte message header).

---

## 6. Client-Side: Snapshot Reception, Interpolation Buffer, Entity Interpolation

### Interpolation Strategy

The client renders the world at a time slightly in the past ("render time"), interpolating between the two snapshots that bracket that time. This smooths out jitter from network variance.

```
render_time = client_time - interp_delay

interp_delay = configurable, default 20ms (~2.5 server ticks at 128Hz)
```

The client maintains a buffer of received snapshots sorted by tick. To render, it finds the two snapshots `A` and `B` such that `A.tick <= render_tick < B.tick`, and lerps each entity's state between A and B.

```c
#define N_INTERP_BUFFER_SIZE    32  /* snapshot ring buffer on client */

typedef struct {
    n_snapshot_t    snapshots[N_INTERP_BUFFER_SIZE];
    u32             count;              /* number of valid snapshots */
    u32             newest_index;       /* index of most recent snapshot */
} n_interp_buffer_t;

typedef struct {
    /* Position: float for smooth rendering */
    f32     pos_x, pos_y, pos_z;
    f32     vel_x, vel_y, vel_z;
    f32     yaw, pitch;
    u8      entity_type;
    u8      flags;
    u8      health;
    u8      armor;
    u8      weapon;
    u8      ammo;
    bool    active;
} n_interp_entity_t;

typedef struct {
    n_interp_entity_t entities[N_MAX_ENTITIES];
} n_interp_state_t;
```

### Interpolation Implementation

```c
/* Called every render frame (not every tick) */
void n_client_interpolate(n_client_t *client, f64 render_time);
```

Algorithm:
1. Convert `render_time` to a tick number: `render_tick = render_time * 128.0`.
2. Find snapshots A and B in the interp buffer where `A.tick <= render_tick < B.tick`.
3. Compute `t = (render_tick - A.tick) / (B.tick - A.tick)`, clamped to [0, 1].
4. For each entity present in both A and B: lerp position, velocity, angles by `t`. Copy discrete fields (health, weapon, etc.) from B if `t >= 0.5`, else from A.
5. For entities present in B but not A (spawned): snap to B's state if `render_tick >= B.tick - 0.5`, else invisible.
6. For entities present in A but not B (despawned): keep rendering A's state until `render_tick >= B.tick`, then mark inactive.

**Angle interpolation:** Use shortest-arc interpolation on the 16-bit quantized angles. Convert to float, find the shortest path around the circle, lerp.

### Snapshot Reassembly from Delta

The client maintains a "baseline" snapshot -- the last fully reconstructed snapshot that the server is also using as a delta base. When the client receives a delta snapshot:

1. Read the `base_tick` from the message. Look up the corresponding snapshot in the interp buffer.
2. Apply the delta to produce a new full snapshot.
3. Store the new snapshot in the interp buffer.
4. The client's ack of this packet tells the server "I have tick X" which the server then uses as the new delta baseline.

```c
typedef struct {
    u32 base_tick;          /* tick of baseline snapshot, 0 = full snapshot */
    u32 current_tick;       /* tick of this snapshot */
    /* followed by bitpacked delta data */
} n_snapshot_msg_header_t;
```

If `base_tick == 0`, this is a full (non-delta) snapshot. The server sends a full snapshot on first connect and whenever the client's baseline is too old (fallen out of the server's history).

---

## 7. Input Handling

### Input Packet

The client samples input and sends it to the server every client tick (128 Hz).

```c
typedef struct {
    u32     tick;               /* server tick this input is for */
    i8      forward_move;       /* -127 to 127 */
    i8      side_move;          /* -127 to 127 */
    u16     yaw;                /* quantized view angle */
    u16     pitch;              /* quantized view angle */
    u16     buttons;            /* bitmask: fire, jump, crouch, etc. */
} n_input_t;
/* Wire size: 10 bytes payload */
```

**Button bitfield:**

```c
#define N_BUTTON_FIRE       (1 << 0)
#define N_BUTTON_JUMP       (1 << 1)
#define N_BUTTON_CROUCH     (1 << 2)
#define N_BUTTON_USE        (1 << 3)
#define N_BUTTON_WEAPON1    (1 << 4)
#define N_BUTTON_WEAPON2    (1 << 5)
#define N_BUTTON_WEAPON3    (1 << 6)
#define N_BUTTON_WEAPON4    (1 << 7)
#define N_BUTTON_WEAPON5    (1 << 8)
#define N_BUTTON_WEAPON6    (1 << 9)
#define N_BUTTON_WEAPON7    (1 << 10)
#define N_BUTTON_WEAPON8    (1 << 11)
/* bits 12-15 reserved */
```

### Input Redundancy

To handle packet loss, each input packet includes the last 3 inputs (current + 2 previous). This way, if one packet is lost, the next one covers the gap.

```
INPUT message payload:
  [input_count: 2 bits]       (1..3, how many inputs in this message)
  [start_tick: 32 bits]       (tick of the oldest input)
  For each input (oldest first):
    [forward_move: 8 bits]
    [side_move: 8 bits]
    [yaw: 16 bits]
    [pitch: 16 bits]
    [buttons: 16 bits]
Total per input: 8 bytes. With 3 inputs + overhead: 2 + 32 + 3*64 = 226 bits = ~29 bytes.
```

The server stores received inputs and discards duplicates (by tick number). If an input arrives for a tick the server already simulated, it's discarded (the server does not re-simulate in the vertical slice -- that's rollback territory).

### Server Input Processing

```c
/* Called during n_server_tick */
void n_server_process_inputs(n_server_t *server);
```

For each client slot, the server dequeues the input for the current server tick (if available). If no input is available (packet loss or late arrival), the server repeats the client's last known input. This prevents stalls but causes slight misprediction that future rollback will fix.

---

## 8. Clock Synchronization

### Goal

The client needs to know the server's current tick so it can:
1. Timestamp its inputs with the correct server tick.
2. Calculate `render_time` for interpolation.

We do NOT need sub-millisecond accuracy for the vertical slice. We need accuracy within ~2ms, enough for correct interpolation.

### Protocol

Uses a simple ping-pong scheme, similar to NTP but simplified:

```
Client -> Server:  CLOCK_SYNC { client_send_time: f64 }
Server -> Client:  CLOCK_SYNC { client_send_time: f64, server_time: f64 }
```

On receipt of the response:
```
rtt = current_client_time - client_send_time
one_way_latency = rtt / 2  (assumption: symmetric)
server_time_now = server_time + one_way_latency
clock_offset = server_time_now - current_client_time
```

The client maintains a rolling average of `clock_offset` over the last 8 samples, discarding outliers (samples where RTT > 2x the median RTT). Clock sync probes are sent every 1 second during gameplay and every 100ms during the initial connection phase (first 2 seconds) to converge quickly.

```c
#define N_CLOCK_SYNC_SAMPLES    16
#define N_CLOCK_SYNC_INTERVAL   1.0     /* seconds, during gameplay */
#define N_CLOCK_SYNC_FAST       0.1     /* seconds, during initial sync */

typedef struct {
    f64     offset_samples[N_CLOCK_SYNC_SAMPLES];
    f64     rtt_samples[N_CLOCK_SYNC_SAMPLES];
    u32     sample_count;
    u32     sample_index;
    f64     smoothed_offset;    /* filtered clock offset */
    f64     smoothed_rtt;       /* filtered RTT */
    f64     last_sync_time;
    bool    converged;          /* true after sufficient samples */
} n_clock_state_t;
```

### Client Tick Derivation

```c
f64 n_client_server_time(const n_client_t *client) {
    return client_local_time() + client->clock.smoothed_offset;
}

u32 n_client_server_tick(const n_client_t *client) {
    return (u32)(n_client_server_time(client) * 128.0);
}

f64 n_client_render_time(const n_client_t *client) {
    return n_client_server_time(client) - client->interp_delay;
}
```

`interp_delay` defaults to `0.020` (20ms) and is configurable. The client can adaptively increase it if it detects too much jitter (snapshots arriving late), but for the vertical slice, a fixed value is fine.

---

## 9. Spectator Architecture Considerations

The vertical slice does not implement spectating, but the architecture must support it cleanly. Key design decisions:

### Spectator as a Client Variant

A spectator is a connected client that:
- Does NOT have a player entity in the game world.
- Still receives snapshots (possibly filtered to the spectated player's POV).
- Sends minimal input (camera controls, spectate-next/prev commands).
- Has its own interpolation buffer.

This is achieved by adding a `role` field to the client slot:

```c
typedef enum {
    N_CLIENT_ROLE_PLAYER,
    N_CLIENT_ROLE_SPECTATOR
} n_client_role_t;
```

The server checks `role` before creating a player entity and before accepting gameplay inputs. Spectators receive the same snapshot stream as players (for simplicity in the vertical slice architecture). A future optimization can filter snapshots to only include entities visible from the spectated player's POV.

### POV-Synced Spectating

For spectating synced to a specific player's POV (seeing exactly what they see, at the same time), the spectator's interpolation must be tied to the spectated player's timing. This means:

- The server tags which player the spectator is following.
- The spectator client uses the same `render_time` calculation but could apply a different interp delay.
- No special snapshot format needed -- the same entity data suffices. The rendering layer handles first-person POV switching.

The only netcode requirement is: a reliable command to say "spectate player X" and the server including the `spectated_player_id` in the client slot. All of this works within the existing reliable command channel.

### Data Hiding for Anti-Cheat

A critical future consideration: don't send entity positions that the spectated player can't see (wallhack prevention for competitive spectating). This requires server-side visibility (PVS) checks before snapshot generation. The architecture supports this because snapshot generation is already per-client; adding a PVS filter is a matter of zeroing out entities not visible to the client's (or spectated player's) position.

---

## 10. Data Structures and Interfaces

### Core Server Structure

```c
typedef struct {
    /* Transport */
    n_transport_t       transport;          /* UDP socket (shared for all clients) */

    /* Client slots */
    n_client_slot_t     clients[N_MAX_CLIENTS];
    u32                 client_count;       /* number of connected clients */

    /* Game state */
    u32                 tick;               /* current server tick */
    f64                 tick_time;          /* time of current tick */
    f64                 tick_interval;      /* 1.0 / 128.0 */

    /* Snapshot buffer (shared) */
    n_snapshot_buffer_t snapshot_buffer;

    /* Temporary packet buffer */
    u8                  packet_buffer[N_TRANSPORT_MTU];
} n_server_t;
```

### Core Client Structure

```c
typedef struct {
    /* Transport */
    n_transport_t       transport;

    /* Connection state */
    n_conn_state_t      conn_state;
    u8                  client_id;          /* assigned by server */
    n_address_t         server_address;

    /* Protocol */
    u16                 outgoing_sequence;
    u16                 incoming_sequence;
    u32                 ack_bitfield;

    /* Reliable channel */
    n_reliable_channel_t reliable;

    /* Clock sync */
    n_clock_state_t     clock;

    /* Interpolation */
    n_interp_buffer_t   interp_buffer;
    n_interp_state_t    interp_state;       /* current interpolated state for renderer */
    f64                 interp_delay;       /* configurable, default 0.020 */

    /* Input */
    n_input_t           input_history[N_INPUT_QUEUE_SIZE]; /* ring buffer of sent inputs */
    u32                 input_history_head;
    u32                 input_tick;         /* next tick to generate input for */

    /* Timing */
    f64                 last_packet_recv_time;
    f64                 connect_start_time;
    u32                 challenge;

    /* Future: prediction state will go here */
    /* n_prediction_state_t prediction; */

    /* Temporary packet buffer */
    u8                  packet_buffer[N_TRANSPORT_MTU];
} n_client_t;
```

### Public API (include/netcode/netcode.h)

This is the interface consumed by main.c and other engine modules:

```c
#ifndef QUICKEN_NETCODE_H
#define QUICKEN_NETCODE_H

#include "quicken.h"

/* Configuration */
typedef struct {
    u16     server_port;        /* 0 = don't bind (client-only) */
    u32     max_clients;        /* up to N_MAX_CLIENTS */
    f64     tick_rate;          /* server tick rate in Hz (128.0) */
} qk_net_server_config_t;

typedef struct {
    f64     interp_delay;       /* interpolation delay in seconds (0.020) */
} qk_net_client_config_t;

/* Result codes */
typedef enum {
    QK_NET_OK = 0,
    QK_NET_ERROR_SOCKET,
    QK_NET_ERROR_FULL,
    QK_NET_ERROR_TIMEOUT,
    QK_NET_ERROR_REJECTED,
    QK_NET_ERROR_INVALID
} qk_net_result_t;

/* Server API */
qk_net_result_t qk_net_server_init(const qk_net_server_config_t *config);
void            qk_net_server_tick(void);
void            qk_net_server_shutdown(void);
u32             qk_net_server_get_tick(void);
u32             qk_net_server_client_count(void);

/* Called by the game module to push authoritative state into the snapshot */
void            qk_net_server_set_entity(u8 entity_id, const n_entity_state_t *state);
void            qk_net_server_remove_entity(u8 entity_id);

/* Called by the game module to read client inputs for the current tick */
bool            qk_net_server_get_input(u8 client_id, n_input_t *out_input);

/* Client API */
qk_net_result_t qk_net_client_init(const qk_net_client_config_t *config);
qk_net_result_t qk_net_client_connect_remote(const char *address, u16 port);
qk_net_result_t qk_net_client_connect_local(void);  /* loopback to local server */
void            qk_net_client_disconnect(void);
void            qk_net_client_tick(void);
void            qk_net_client_interpolate(f64 render_time);
void            qk_net_client_shutdown(void);

/* Called by the input module to push local input */
void            qk_net_client_send_input(const n_input_t *input);

/* Called by the renderer to get interpolated state */
const n_interp_state_t *qk_net_client_get_interp_state(void);

/* Connection info */
n_conn_state_t  qk_net_client_get_state(void);
i32             qk_net_client_get_rtt(void);
u8              qk_net_client_get_id(void);

#endif /* QUICKEN_NETCODE_H */
```

### Interfaces Required FROM Other Modules

| Module | Interface | Used By Netcode For |
|--------|-----------|---------------------|
| Physics | `qk_physics_step(const n_input_t *input, f64 dt)` | Future: rollback replay. Not used in vertical slice. |
| Physics | `qk_physics_get_entity_state(u8 id, n_entity_state_t *out)` | Server snapshot generation reads physics state. |
| Physics | `qk_physics_set_entity_state(u8 id, const n_entity_state_t *state)` | Future: rollback state restore. |
| Core | `qk_time_now(void)` -> `f64` | All timing (monotonic seconds since init). |
| Core | `qk_input_sample(n_input_t *out)` | Client reads local input to send to server. |

### Interfaces PROVIDED TO Other Modules

| Interface | Consumer | Purpose |
|-----------|----------|---------|
| `qk_net_server_get_input(client_id, &input)` | Game logic / Physics | Server reads client inputs to step simulation. |
| `qk_net_client_get_interp_state()` | Renderer | Renderer reads interpolated entity states. |
| `qk_net_client_get_rtt()` | HUD / Debug | Display ping. |
| `qk_net_server_set_entity(id, &state)` | Game logic / Physics | Game pushes authoritative state for snapshot broadcast. |

---

## 11. File Layout

```
src/netcode/
    n_transport.c       # Transport abstraction (loopback + UDP)
    n_platform.c        # Platform socket wrappers (Winsock / BSD)
    n_protocol.c        # Packet header, sequencing, ack bitfield
    n_channel.c         # Reliable ordered message channel
    n_server.c          # Server tick loop, client slot management, snapshot broadcast
    n_client.c          # Client tick loop, snapshot reception, interpolation
    n_snapshot.c        # Snapshot capture, delta encode/decode
    n_compress.c        # Bitpacker (write/read bits)
    n_clock.c           # Clock synchronization

include/netcode/
    netcode.h           # Public API (qk_net_* functions)
    n_transport.h       # Transport types and functions
    n_protocol.h        # Packet/message types, header structs
    n_channel.h         # Reliable channel types
    n_snapshot.h        # Snapshot, entity state types
    n_compress.h        # Bitpacker types and functions
    n_clock.h           # Clock sync types
    n_types.h           # Shared netcode types (n_input_t, n_address_t, constants)
```

---

## 12. Build System Changes

Add a `quicken-netcode` static library to `premake5.lua`:

```lua
project "quicken-netcode"
    kind "StaticLib"
    language "C"
    cdialect "C11"

    targetdir ("build/lib/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/netcode")

    files {
        "src/netcode/**.c",
        "include/netcode/**.h"
    }

    includedirs {
        "include"
    }

    -- Precise floating-point for determinism (same as physics)
    filter "toolset:gcc or toolset:clang"
        buildoptions { "-Wall", "-Wextra", "-march=native", "-std=c11" }
    filter "toolset:msc"
        buildoptions { "/W4", "/arch:AVX2", "/fp:precise" }

    -- Platform-specific socket libraries
    filter "system:windows"
        links { "ws2_32" }
    filter "system:linux"
        links { "pthread" }
    filter {}
```

The main executable links: `quicken-physics`, `quicken-renderer`, `quicken-netcode`.

---

## 13. Implementation Order

The implementation should proceed in this order, each step producing testable, working code:

### Phase 1: Foundation (transport + bitpacker)
1. `n_platform.c` -- Socket abstraction (open, close, send, recv, non-blocking).
2. `n_transport.c` -- Loopback ring buffer + UDP transport. Test: send/recv bytes through both.
3. `n_compress.c` -- Bitpacker (write_bits, read_bits). Test: round-trip bit patterns.

### Phase 2: Protocol Layer
4. `n_protocol.c` -- Packet header encode/decode, sequence comparison, message framing.
5. `n_channel.c` -- Reliable channel (send, retransmit, ack). Test: reliable delivery over lossy loopback (simulate drops).

### Phase 3: Server Core
6. `n_server.c` -- Server init, tick loop, client slot management. Accept connections via handshake.
7. `n_snapshot.c` -- Snapshot capture + delta encode. Test: encode/decode round-trip, verify compressed size.

### Phase 4: Client Core
8. `n_client.c` -- Client init, connect (local + remote), receive snapshots, populate interp buffer.
9. `n_clock.c` -- Clock sync probes. Test: measure offset stability.
10. Client interpolation logic in `n_client.c`. Test: visual smoothness with artificial latency.

### Phase 5: Input Loop
11. Client input sampling + transmission (redundant inputs).
12. Server input reception + dequeue for simulation.
13. End-to-end test: client sends inputs, server steps, server sends snapshots, client interpolates.

### Phase 6: Integration
14. Wire netcode into `main.c` game loop.
15. premake5.lua changes.
16. End-to-end local play test.
17. End-to-end networked play test (two processes, one machine).

---

## 14. Configurable Timing Constants

All timing values are grouped in one place for easy tuning:

```c
/* include/netcode/n_types.h */

/* Tick rate */
#define N_TICK_RATE             128                         /* Hz */
#define N_TICK_INTERVAL         (1.0 / (f64)N_TICK_RATE)   /* ~7.8125ms */

/* Interpolation */
#define N_INTERP_DELAY_DEFAULT  0.020   /* 20ms = ~2.56 ticks */
#define N_INTERP_DELAY_MIN      0.0078  /* 1 tick minimum */
#define N_INTERP_DELAY_MAX      0.100   /* 100ms maximum */

/* Connection */
#define N_CONNECT_RETRY_MS      500
#define N_CONNECT_TIMEOUT_MS    10000
#define N_TIMEOUT_MS            30000
#define N_DISCONNECT_LINGER_MS  1000

/* Clock sync */
#define N_CLOCK_SYNC_INTERVAL   1.0     /* seconds between probes */
#define N_CLOCK_SYNC_FAST       0.1     /* seconds between probes during init */
#define N_CLOCK_SYNC_SAMPLES    16
#define N_CLOCK_CONVERGE_COUNT  4       /* min samples before "converged" */

/* Reliable channel */
#define N_RELIABLE_RETRANSMIT_MS 200
#define N_RELIABLE_MAX_PAYLOAD   4096

/* Input */
#define N_INPUT_REDUNDANCY      3       /* inputs per packet */

/* Bandwidth (informational, not enforced in vertical slice) */
#define N_TARGET_BANDWIDTH      51200   /* 50 KB/s per client */
```

---

## 15. Memory Budget

| Allocation | Size | Count | Total |
|-----------|------|-------|-------|
| `n_server_t` | ~100 bytes base | 1 | ~100 B |
| `n_client_slot_t` | ~6.5 KB | 16 | ~104 KB |
| `n_snapshot_buffer_t` (shared) | ~360 KB | 1 | ~360 KB |
| `n_client_t` | ~200 KB (interp buffer + state) | 1 | ~200 KB |
| Packet buffers | 1400 B | 2 (server+client) | ~3 KB |
| Loopback queues | ~88 KB per pair | 1 (local play) | ~88 KB |
| **Total** | | | **~755 KB** |

All allocations are either static or allocated once at init (no per-frame heap allocation). The snapshot buffer and loopback queues are the largest consumers but are well within budget for a game engine.

---

## 16. Error Handling Strategy

The netcode layer does not crash on bad data. All received packets are validated:

1. **Packet too short**: Drop silently.
2. **Message type out of range**: Drop entire packet.
3. **Message length exceeds remaining packet**: Drop entire packet.
4. **Bitreader overflow** (delta decode reads past end): Drop snapshot, request full resend.
5. **Sequence from the past**: Ignore (already processed).
6. **Unknown client address**: Ignore unless it's a CONNECT_REQUEST.
7. **Delta baseline not found** (client's acked tick fell out of server history): Server sends a full (non-delta) snapshot.

All error conditions increment debug counters (packet drops, bad sequences, etc.) for diagnostics.

```c
typedef struct {
    u64     packets_sent;
    u64     packets_received;
    u64     packets_dropped;        /* failed validation */
    u64     bytes_sent;
    u64     bytes_received;
    u64     snapshots_delta;        /* delta-encoded snapshots sent */
    u64     snapshots_full;         /* full snapshots sent (baseline lost) */
    u64     inputs_received;
    u64     inputs_duplicated;      /* redundant inputs discarded */
    u64     inputs_late;            /* inputs for already-simulated ticks */
} n_stats_t;
```

---

## 17. Future Extension Points

The architecture explicitly reserves these extension points for post-vertical-slice work:

| Feature | Extension Point |
|---------|----------------|
| Client prediction | `n_client_t.prediction` field + `n_predict_step()` function that calls physics. |
| Rollback | `n_rollback.c` -- saves `n_snapshot_t` per predicted tick, replays inputs on misprediction via `qk_physics_step`. |
| Server-side rewind (lag compensation) | Server stores N ticks of entity history. Hit detection rewinds to the shooter's perceived time. Uses `n_snapshot_buffer_t`. |
| Spectator POV | `n_client_role_t` field on client slot. Reliable command to set spectate target. |
| PVS filtering | Per-client entity visibility mask applied during `n_server_broadcast_snapshots`. |
| Adaptive send rate | Monitor bandwidth via `n_stats_t`, reduce snapshot rate for clients with poor connections. |
| Packet encryption | Wrap `n_transport_send/recv` with an encrypt/decrypt layer. Key exchange during handshake. |
| Demo recording | Tap the snapshot stream (pre-delta) into a file. Playback feeds snapshots into the interp buffer. |
