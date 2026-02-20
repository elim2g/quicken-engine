/*
 * QUICKEN Engine - Gameplay Public API
 *
 * Game init/tick, player management, entity packing, Clan Arena state.
 */

#ifndef QK_GAMEPLAY_H
#define QK_GAMEPLAY_H

#include "quicken.h"
#include "qk_types.h"
#include "netcode/n_types.h"

// Forward declaration
typedef struct qk_phys_world qk_phys_world_t;

// Game config
typedef struct {
    u8      max_players;            // 0 = default (16)
    u8      rounds_to_win;          // 0 = default (10)
    u32     round_time_limit_ms;    // 0 = default (120000)
    u32     countdown_time_ms;      // 0 = default (5000)
} qk_game_config_t;

// Clan Arena state (read-only for UI) -- named struct for forward declaration
typedef struct qk_ca_state {
    u8      state;              // ca_round_state_t enum value
    u32     state_timer_ms;
    u8      score_alpha;
    u8      score_beta;
    u8      round_number;
    u8      alive_alpha;
    u8      alive_beta;
} qk_ca_state_t;

// Opaque game state
typedef struct qk_game_state qk_game_state_t;

// Lifecycle
qk_result_t         qk_game_init(const qk_game_config_t *config);
void                qk_game_tick(qk_phys_world_t *world, f32 dt);
void                qk_game_shutdown(void);

// Load trigger volumes (teleporters + jump pads) from map data.
// Makes internal copies of the data. Call after qk_game_init.
void qk_game_load_triggers(const qk_teleporter_t *teleporters, u32 teleporter_count,
                            const qk_jump_pad_t *jump_pads, u32 jump_pad_count);

// Player management
qk_result_t         qk_game_player_connect(u8 client_num,
                                             const char *name, qk_team_t team);
void                qk_game_player_disconnect(u8 client_num);
void                qk_game_player_command(u8 client_num,
                                            const qk_usercmd_t *cmd);

// State queries
const qk_player_state_t *qk_game_get_player_state(u8 client_num);
qk_player_state_t       *qk_game_get_player_state_mut(u8 client_num);
const qk_ca_state_t     *qk_game_get_ca_state(void);
qk_game_state_t         *qk_game_get_state(void);

// Entity packing for netcode
void qk_game_pack_entity(u8 entity_id, n_entity_state_t *out);
u32  qk_game_get_entity_count(void);

// Diagnostics: raw f32 entity origin (before quantization)
bool qk_game_get_entity_origin(u8 entity_id, f32 *x, f32 *y, f32 *z);

// Explosion event (produced by projectile system, consumed for visuals)
typedef struct {
    f32     pos[3];
    f32     dir[3];     // normalized projectile travel direction at impact
    f32     radius;
} qk_explosion_event_t;

// Returns the number of explosions that occurred this tick.
// Fills out_events up to max_events.
u32 qk_game_get_explosions(qk_explosion_event_t *out_events, u32 max_events);

#endif /* QK_GAMEPLAY_H */
