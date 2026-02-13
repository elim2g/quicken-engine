/*
 * QUICKEN Gameplay Module - Stub Implementation
 *
 * All functions return success/zero/no-op.
 * The gameplay agent replaces this with real code on feat/gameplay.
 */

#include "gameplay/qk_gameplay.h"

qk_result_t qk_game_init(const qk_game_config_t *config) {
    QK_UNUSED(config);
    return QK_SUCCESS;
}

void qk_game_tick(qk_phys_world_t *world, f32 dt) {
    QK_UNUSED(world); QK_UNUSED(dt);
}

void qk_game_shutdown(void) {}

qk_result_t qk_game_player_connect(u8 client_num, const char *name, qk_team_t team) {
    QK_UNUSED(client_num); QK_UNUSED(name); QK_UNUSED(team);
    return QK_SUCCESS;
}

void qk_game_player_disconnect(u8 client_num) {
    QK_UNUSED(client_num);
}

void qk_game_player_command(u8 client_num, const qk_usercmd_t *cmd) {
    QK_UNUSED(client_num); QK_UNUSED(cmd);
}

const qk_player_state_t *qk_game_get_player_state(u8 client_num) {
    QK_UNUSED(client_num);
    return NULL;
}

qk_player_state_t *qk_game_get_player_state_mut(u8 client_num) {
    QK_UNUSED(client_num);
    return NULL;
}

static qk_ca_state_t s_empty_ca = {0};

const qk_ca_state_t *qk_game_get_ca_state(void) {
    return &s_empty_ca;
}

qk_game_state_t *qk_game_get_state(void) {
    return NULL;
}

void qk_game_pack_entity(u8 entity_id, n_entity_state_t *out) {
    QK_UNUSED(entity_id);
    if (out) {
        n_entity_state_t empty = {0};
        *out = empty;
    }
}

u32 qk_game_get_entity_count(void) { return 0; }
