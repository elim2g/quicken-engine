/*
 * QUICKEN Engine - Demo Recording/Playback
 *
 * Records and plays back gameplay demos (.qkdm files).
 * Recording captures the network stream (snapshots, inputs, events, game state).
 * Playback feeds data back through the client interpolation pipeline.
 */

#ifndef QK_DEMO_H
#define QK_DEMO_H

#include "quicken.h"
#include "qk_types.h"
#include "netcode/n_types.h"

/* Forward declarations */
struct qk_ca_state;

/* Demo file magic and version */
#define QK_DEMO_MAGIC           0x4D444B51  /* 'QKDM' */
#define QK_DEMO_VERSION         1
#define QK_DEMO_MAP_NAME_LEN    28

/* Record types */
#define QK_DEMO_RECORD_SNAPSHOT     1
#define QK_DEMO_RECORD_USERCMD      2
#define QK_DEMO_RECORD_EVENT        3
#define QK_DEMO_RECORD_GAMESTATE    4
#define QK_DEMO_RECORD_END          255

/* Entity mask size (256 entities / 64 bits per u64) */
#define QK_DEMO_MASK_U64S       4

/* File header (48 bytes) */
typedef struct {
    u32  magic;
    u32  version;
    u32  tick_rate;
    u32  start_tick;
    u8   local_client_id;
    u8   pad[3];
    char map_name[QK_DEMO_MAP_NAME_LEN];
} qk_demo_header_t;

/* Record preamble (8 bytes) */
typedef struct {
    u8   type;
    u8   pad;
    u16  payload_len;
    u32  server_tick;
} qk_demo_record_t;

/* Lifecycle */
void qk_demo_init(void);
void qk_demo_shutdown(void);

/* Recording */
bool qk_demo_record_start(const char *name, u8 client_id,
                           u32 start_tick, const char *map_name);
void qk_demo_record_stop(void);
void qk_demo_record_snapshot(u32 tick, u32 entity_count,
                              const u64 *entity_mask,
                              const n_entity_state_t *entities);
void qk_demo_record_usercmd(u32 tick, const qk_usercmd_t *cmd);
void qk_demo_record_event(u32 tick, const void *event_blob, u16 event_size);
void qk_demo_record_gamestate(u32 tick, const struct qk_ca_state *ca_state);

/* Playback */
bool qk_demo_play_start(const char *name);
void qk_demo_play_stop(void);
bool qk_demo_play_tick(u32 current_tick);

/* State queries */
bool qk_demo_is_recording(void);
bool qk_demo_is_playing(void);
u8   qk_demo_get_pov_client_id(void);
u32  qk_demo_get_start_tick(void);

/* Last usercmd during playback (for camera angles) */
const qk_usercmd_t *qk_demo_get_last_usercmd(void);

/* Last game state during playback (for HUD) */
const struct qk_ca_state *qk_demo_get_last_ca_state(void);

#endif /* QK_DEMO_H */
