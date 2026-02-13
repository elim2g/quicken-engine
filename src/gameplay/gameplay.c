/*
 * QUICKEN Engine - Gameplay Module Main
 *
 * qk_game_init/tick/shutdown. Wires all gameplay subsystems together.
 * Owns the global game state.
 */

#include "g_internal.h"
#include "netcode/n_types.h"

/* ---- Global Game State ---- */
static qk_game_state_t s_gs;

/* ---- Lifecycle ---- */

qk_result_t qk_game_init(const qk_game_config_t *config) {
    memset(&s_gs, 0, sizeof(s_gs));

    g_entity_pool_init(&s_gs.entities);

    /* apply config with defaults */
    s_gs.max_players = (config && config->max_players > 0) ?
        config->max_players : QK_MAX_PLAYERS;
    s_gs.rounds_to_win = (config && config->rounds_to_win > 0) ?
        config->rounds_to_win : QK_CA_ROUNDS_TO_WIN;
    s_gs.round_time_limit_ms = (config && config->round_time_limit_ms > 0) ?
        config->round_time_limit_ms : QK_CA_ROUND_TIME_MS;
    s_gs.countdown_time_ms = (config && config->countdown_time_ms > 0) ?
        config->countdown_time_ms : QK_CA_COUNTDOWN_MS;

    s_gs.server_time_ms = 0;
    s_gs.num_clients = 0;

    /* initialize player entity mapping */
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        s_gs.player_entity[i] = -1;
    }

    g_ca_init(&s_gs);
    g_event_clear(&s_gs.events);

    return QK_SUCCESS;
}

void qk_game_tick(qk_phys_world_t *world, f32 dt) {
    QK_UNUSED(world);

    u32 dt_ms = (u32)(dt * 1000.0f + 0.5f);
    s_gs.server_time_ms += dt_ms;

    /* clear events from previous tick */
    g_event_clear(&s_gs.events);

    /* 1. CA mode tick (state machine transitions) */
    g_ca_tick(&s_gs, dt_ms);

    /* 2. Process player commands (weapon tick, view angles) */
    g_process_commands(&s_gs, dt_ms);

    /* 3. Projectile tick (movement + collision) */
    g_projectile_tick(&s_gs, dt);
}

void qk_game_shutdown(void) {
    memset(&s_gs, 0, sizeof(s_gs));
}

/* ---- Player Management ---- */

qk_result_t qk_game_player_connect(u8 client_num, const char *name, qk_team_t team) {
    if (client_num >= QK_MAX_PLAYERS) return QK_ERROR_INVALID_PARAM;
    if (s_gs.player_entity[client_num] >= 0) return QK_ERROR_FULL;

    entity_t *ent = g_entity_alloc(&s_gs.entities, ENTITY_PLAYER);
    if (!ent) return QK_ERROR_FULL;

    s_gs.player_entity[client_num] = (i32)(ent - s_gs.entities.entities);

    qk_player_state_t *ps = &ent->data.player;
    memset(ps, 0, sizeof(*ps));
    ps->client_num = client_num;
    ps->team = team;
    ps->alive_state = QK_PSTATE_SPECTATING;
    ps->weapon = QK_WEAPON_NONE;
    ps->mins = QK_PLAYER_MINS;
    ps->maxs = QK_PLAYER_MAXS;
    ps->max_speed = QK_PM_MAX_SPEED;
    ps->gravity = QK_PM_GRAVITY;

    s_gs.num_clients++;

    QK_UNUSED(name);
    return QK_SUCCESS;
}

void qk_game_player_disconnect(u8 client_num) {
    if (client_num >= QK_MAX_PLAYERS) return;
    i32 idx = s_gs.player_entity[client_num];
    if (idx < 0) return;

    g_entity_free(&s_gs.entities, &s_gs.entities.entities[idx]);
    s_gs.player_entity[client_num] = -1;

    if (s_gs.num_clients > 0) s_gs.num_clients--;
}

void qk_game_player_command(u8 client_num, const qk_usercmd_t *cmd) {
    if (client_num >= QK_MAX_PLAYERS || !cmd) return;
    i32 idx = s_gs.player_entity[client_num];
    if (idx < 0) return;

    entity_t *ent = &s_gs.entities.entities[idx];
    ent->data.player.last_cmd = *cmd;
}

/* ---- State Queries ---- */

const qk_player_state_t *qk_game_get_player_state(u8 client_num) {
    if (client_num >= QK_MAX_PLAYERS) return NULL;
    i32 idx = s_gs.player_entity[client_num];
    if (idx < 0) return NULL;
    return &s_gs.entities.entities[idx].data.player;
}

qk_player_state_t *qk_game_get_player_state_mut(u8 client_num) {
    if (client_num >= QK_MAX_PLAYERS) return NULL;
    i32 idx = s_gs.player_entity[client_num];
    if (idx < 0) return NULL;
    return &s_gs.entities.entities[idx].data.player;
}

const qk_ca_state_t *qk_game_get_ca_state(void) {
    return &s_gs.ca;
}

qk_game_state_t *qk_game_get_state(void) {
    return &s_gs;
}

/* ---- Entity Packing (for netcode) ---- */

void qk_game_pack_entity(u8 entity_id, n_entity_state_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    entity_t *ent = &s_gs.entities.entities[entity_id];
    if (!ent->active || ent->type == ENTITY_NONE) return;

    if (ent->type == ENTITY_PLAYER) {
        qk_player_state_t *ps = &ent->data.player;
        out->entity_type = (u8)ENTITY_PLAYER;
        out->pos_x = (i16)(ps->origin.x * 8.0f);
        out->pos_y = (i16)(ps->origin.y * 8.0f);
        out->pos_z = (i16)(ps->origin.z * 8.0f);
        out->vel_x = (i16)ps->velocity.x;
        out->vel_y = (i16)ps->velocity.y;
        out->vel_z = (i16)ps->velocity.z;
        out->yaw = (u16)(ps->yaw * (65535.0f / 360.0f));
        out->pitch = (u16)(ps->pitch * (65535.0f / 360.0f));
        out->health = (ps->health > 0) ? (u8)((ps->health > 255) ? 255 : ps->health) : 0;
        out->armor = (ps->armor > 0) ? (u8)((ps->armor > 255) ? 255 : ps->armor) : 0;
        out->weapon = (u8)ps->weapon;
        out->ammo = (ps->weapon < QK_WEAPON_COUNT) ?
            (u8)((ps->ammo[ps->weapon] > 255) ? 255 : ps->ammo[ps->weapon]) : 0;
    } else if (ent->type == ENTITY_PROJECTILE) {
        projectile_t *p = &ent->data.projectile;
        out->entity_type = (u8)ENTITY_PROJECTILE;
        out->pos_x = (i16)(p->origin.x * 8.0f);
        out->pos_y = (i16)(p->origin.y * 8.0f);
        out->pos_z = (i16)(p->origin.z * 8.0f);
        out->vel_x = (i16)p->velocity.x;
        out->vel_y = (i16)p->velocity.y;
        out->vel_z = (i16)p->velocity.z;
        out->weapon = (u8)p->weapon;
    }
}

u32 qk_game_get_entity_count(void) {
    return s_gs.entities.high_water;
}

/* ---- Process Commands (called during tick) ---- */

void g_process_commands(qk_game_state_t *gs, u32 tick_dt_ms) {
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        i32 ent_idx = gs->player_entity[i];
        if (ent_idx < 0) continue;

        entity_t *ent = &gs->entities.entities[ent_idx];
        qk_player_state_t *ps = &ent->data.player;

        if (ps->alive_state != QK_PSTATE_ALIVE) continue;

        qk_usercmd_t *cmd = &ps->last_cmd;

        /* update view angles */
        ps->pitch = cmd->pitch;
        ps->yaw = cmd->yaw;

        /* weapon switch request */
        if (cmd->weapon_select != 0 &&
            (qk_weapon_id_t)cmd->weapon_select != ps->weapon &&
            cmd->weapon_select < QK_WEAPON_COUNT &&
            ps->ammo[cmd->weapon_select] > 0) {
            g_weapon_switch(ent, (qk_weapon_id_t)cmd->weapon_select);
        }

        /* weapon tick handles firing */
        g_weapon_tick(gs, ent, tick_dt_ms);
    }
}
