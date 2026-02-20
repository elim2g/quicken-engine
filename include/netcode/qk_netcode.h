/*
 * QUICKEN Engine - Netcode Public API
 *
 * Client/server lifecycle, tick, interpolation.
 * Compiled with precise floating-point for deterministic state sync.
 */

#ifndef QK_NETCODE_H
#define QK_NETCODE_H

#include "quicken.h"
#include "qk_types.h"
#include "netcode/n_types.h"

// Server config
typedef struct {
    u16     server_port;        // 0 = don't bind
    u32     max_clients;        // up to 16
    f64     tick_rate;          // 0 = default (128.0)
} qk_net_server_config_t;

// Client config
typedef struct {
    f64     interp_delay;       // 0 = default (0.020)
} qk_net_client_config_t;

// Connection state
typedef enum {
    QK_CONN_DISCONNECTED,
    QK_CONN_CONNECTING,
    QK_CONN_CONNECTED,
    QK_CONN_DISCONNECTING
} qk_conn_state_t;

// Interpolated entity (what the renderer sees)
typedef struct {
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
} qk_interp_entity_t;

#define QK_NET_MAX_ENTITIES     256

typedef struct {
    qk_interp_entity_t entities[QK_NET_MAX_ENTITIES];
} qk_interp_state_t;

// Interpolation diagnostics (for AI trace ingestion)
typedef struct {
    u32     snap_a_tick;
    u32     snap_b_tick;
    f32     t;
    f64     render_tick;
    u32     interp_count;
    bool    valid;              // false if no interp pair found
    bool    fallback;           // true if using two-newest fallback
} qk_interp_diag_t;

// Server API
qk_result_t qk_net_server_init(const qk_net_server_config_t *config);
void        qk_net_server_tick(void);
void        qk_net_server_shutdown(void);
u32         qk_net_server_get_tick(void);
u32         qk_net_server_client_count(void);

void        qk_net_server_set_entity(u8 entity_id,
                                      const n_entity_state_t *state);
void        qk_net_server_remove_entity(u8 entity_id);
bool        qk_net_server_get_input(u8 client_id, qk_usercmd_t *out_cmd);

// Per-client server queries (for detecting remote joins/disconnects)
qk_conn_state_t qk_net_server_get_client_state(u8 client_id);
bool             qk_net_server_is_client_map_ready(u8 client_id);

// Client API
qk_result_t qk_net_client_init(const qk_net_client_config_t *config);
qk_result_t qk_net_client_connect_remote(const char *address, u16 port);
qk_result_t qk_net_client_connect_local(void);
void        qk_net_client_disconnect(void);
void        qk_net_client_tick(void);
void        qk_net_client_interpolate(f64 render_time);
void        qk_net_client_shutdown(void);

void        qk_net_client_send_input(const qk_usercmd_t *cmd);
const qk_interp_state_t *qk_net_client_get_interp_state(void);
const qk_interp_diag_t  *qk_net_client_get_interp_diag(void);

qk_conn_state_t qk_net_client_get_state(void);
i32             qk_net_client_get_rtt(void);
u8              qk_net_client_get_id(void);

// Client prediction support
u32             qk_net_client_get_input_sequence(void);
u32             qk_net_client_get_server_cmd_ack(void);
bool            qk_net_client_get_server_player_state(qk_player_state_t *out);

// Map-load handshake: client notifies server after loading a map.
// Server withholds snapshots until the handshake completes.
void            qk_net_client_notify_map_loaded(const char *map_name);
bool            qk_net_client_is_map_ready(void);

// Server-side: set the current map name (for handshake validation).
// The map name is included in CONNECT_ACCEPTED so remote clients
// know which map to load.
void            qk_net_server_set_map(const char *map_name);

// Client-side: get the map name received from the server in CONNECT_ACCEPTED.
// Returns NULL if not connected or no map name was provided.
const char     *qk_net_client_get_server_map(void);

// Demo playback: inject a snapshot directly into the interp buffer
void            qk_net_client_inject_demo_snapshot(u32 tick, u32 entity_count,
                                                    const u64 *entity_mask,
                                                    const n_entity_state_t *entities);

#endif /* QK_NETCODE_H */
