/*
 * QUICKEN Engine - Gameplay Module Main
 *
 * qk_game_init/tick/shutdown. Wires all gameplay subsystems together.
 * Owns the global game state.
 */

#include "g_internal.h"
#include "netcode/n_types.h"
#include "physics/qk_physics.h"
#include "core/qk_demo.h"

// --- Global Game State ---
static qk_game_state_t s_gs;

// --- Lifecycle ---

qk_result_t qk_game_init(const qk_game_config_t *config) {
    memset(&s_gs, 0, sizeof(s_gs));

    g_entity_pool_init(&s_gs.entities);

    // apply config with defaults
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

    // initialize player entity mapping
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        s_gs.player_entity[i] = -1;
    }

    g_ca_init(&s_gs);
    g_event_clear(&s_gs.events);

    return QK_SUCCESS;
}

void qk_game_tick(qk_phys_world_t *world, f32 dt) {
    u32 dt_ms = (u32)(dt * 1000.0f + 0.5f);
    s_gs.server_time_ms += dt_ms;

    // clear events from previous tick
    g_event_clear(&s_gs.events);

    // 1. CA mode tick (state machine transitions)
    g_ca_tick(&s_gs, dt_ms);

    // 2. Process player commands (weapon tick, view angles)
    g_process_commands(&s_gs, dt_ms);

    // 3. Physics movement for all alive players
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        i32 ent_idx = s_gs.player_entity[i];
        if (ent_idx < 0) continue;

        entity_t *ent = &s_gs.entities.entities[ent_idx];
        qk_player_state_t *ps = &ent->data.player;

        if (ps->alive_state != QK_PSTATE_ALIVE) continue;

        qk_physics_move(ps, &ps->last_cmd, world);
    }

    // 4. Trigger checks (teleporters + jump pads, after physics)
    g_triggers_tick(&s_gs);

    // 5. Projectile tick (movement + collision against world and players)
    g_projectile_tick(&s_gs, dt, world);

    // 6. Demo recording hooks
    if (qk_demo_is_recording()) {
        u32 tick = s_gs.server_time_ms / QK_TICK_DT_MS_NOM;
        qk_demo_record_gamestate(tick, &s_gs.ca);
        for (u32 i = 0; i < s_gs.events.count; i++) {
            qk_demo_record_event(tick, &s_gs.events.events[i],
                                 (u16)sizeof(game_event_t));
        }
    }
}

void qk_game_load_triggers(const qk_teleporter_t *teleporters, u32 teleporter_count,
                            const qk_jump_pad_t *jump_pads, u32 jump_pad_count) {
    g_triggers_load(teleporters, teleporter_count, jump_pads, jump_pad_count);
}

void qk_game_shutdown(void) {
    g_triggers_clear();
    memset(&s_gs, 0, sizeof(s_gs));
}

// --- Player Management ---

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

// --- State Queries ---

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

// --- Entity Packing (for netcode) ---

void qk_game_pack_entity(u8 entity_id, n_entity_state_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    entity_t *ent = &s_gs.entities.entities[entity_id];
    if (!ent->active || ent->type == ENTITY_NONE) return;

    if (ent->type == ENTITY_PLAYER) {
        qk_player_state_t *ps = &ent->data.player;
        out->entity_type = (u8)ENTITY_PLAYER;
        out->pos_x = (i16)(ps->origin.x * 2.0f);
        out->pos_y = (i16)(ps->origin.y * 2.0f);
        out->pos_z = (i16)(ps->origin.z * 2.0f);
        out->vel_x = (i16)ps->velocity.x;
        out->vel_y = (i16)ps->velocity.y;
        out->vel_z = (i16)ps->velocity.z;
        out->yaw = (u16)(ps->yaw * (65535.0f / 360.0f));
        out->pitch = (u16)(ps->pitch * (65535.0f / 360.0f));
        // Determine firing flag
        u8 firing = 0;
        if (ps->weapon > QK_WEAPON_NONE && ps->weapon < QK_WEAPON_COUNT &&
            ps->pending_weapon == QK_WEAPON_NONE) {
            const g_weapon_def_t *wdef = &g_weapon_defs[ps->weapon];
            if (wdef->fire_mode == FIRE_BEAM) {
                // Beam weapons: flag is set continuously while holding attack,
                // weapon is ready or actively cycling, and player has ammo
                if ((ps->last_cmd.buttons & QK_BUTTON_ATTACK) &&
                    ps->ammo[ps->weapon] > 0 && ps->switch_time == 0) {
                    firing = QK_ENT_FLAG_FIRING;
                }
            } else {
                // Hitscan/projectile: flag held for 3 ticks after firing so
                // client interpolation reliably catches the rising edge.
                if (ps->weapon_time >= wdef->fire_interval_ms - 2 * QK_TICK_DT_MS_NOM &&
                    ps->weapon_time > 0 && wdef->fire_interval_ms > 0) {
                    firing = QK_ENT_FLAG_FIRING;
                }
            }
        }

        out->flags = (ps->on_ground  ? QK_ENT_FLAG_ON_GROUND  : 0)
                   | (ps->jump_held  ? QK_ENT_FLAG_JUMP_HELD  : 0)
                   | ((ps->teleport_bit & 1) ? QK_ENT_FLAG_TELEPORTED : 0)
                   | firing;
        out->health  = (ps->health > 0) ? (u8)((ps->health > 255) ? 255 : ps->health) : 0;
        out->armor   = (ps->armor > 0)  ? (u8)((ps->armor > 255)  ? 255 : ps->armor)  : 0;
        out->weapon  = (u8)ps->weapon;
        out->ammo    = (ps->weapon < QK_WEAPON_COUNT)
            ? (u8)((ps->ammo[ps->weapon] > 255) ? 255 : ps->ammo[ps->weapon]) : 0;
    } else if (ent->type == ENTITY_PROJECTILE) {
        projectile_t *p = &ent->data.projectile;
        out->entity_type = (u8)ENTITY_PROJECTILE;
        out->pos_x = (i16)(p->origin.x * 2.0f);
        out->pos_y = (i16)(p->origin.y * 2.0f);
        out->pos_z = (i16)(p->origin.z * 2.0f);
        out->vel_x = (i16)p->velocity.x;
        out->vel_y = (i16)p->velocity.y;
        out->vel_z = (i16)p->velocity.z;
        out->weapon = (u8)p->weapon;
    }
}

u32 qk_game_get_entity_count(void) {
    return s_gs.entities.high_water;
}

bool qk_game_get_entity_origin(u8 entity_id, f32 *x, f32 *y, f32 *z) {
    entity_t *ent = &s_gs.entities.entities[entity_id];
    if (!ent->active) return false;
    if (ent->type == ENTITY_PLAYER) {
        *x = ent->data.player.origin.x;
        *y = ent->data.player.origin.y;
        *z = ent->data.player.origin.z;
    } else if (ent->type == ENTITY_PROJECTILE) {
        *x = ent->data.projectile.origin.x;
        *y = ent->data.projectile.origin.y;
        *z = ent->data.projectile.origin.z;
    } else {
        return false;
    }
    return true;
}

// --- Explosion Event Query ---

u32 qk_game_get_explosions(qk_explosion_event_t *out_events, u32 max_events) {
    u32 count = 0;
    for (u32 i = 0; i < s_gs.events.count && count < max_events; i++) {
        if (s_gs.events.events[i].type == GEVT_EXPLOSION) {
            out_events[count].pos[0] = s_gs.events.events[i].data.explosion.pos[0];
            out_events[count].pos[1] = s_gs.events.events[i].data.explosion.pos[1];
            out_events[count].pos[2] = s_gs.events.events[i].data.explosion.pos[2];
            out_events[count].dir[0] = s_gs.events.events[i].data.explosion.dir[0];
            out_events[count].dir[1] = s_gs.events.events[i].data.explosion.dir[1];
            out_events[count].dir[2] = s_gs.events.events[i].data.explosion.dir[2];
            out_events[count].radius = s_gs.events.events[i].data.explosion.radius;
            count++;
        }
    }
    return count;
}

// --- Process Commands (called during tick) ---

void g_process_commands(qk_game_state_t *gs, u32 tick_dt_ms) {
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        i32 ent_idx = gs->player_entity[i];
        if (ent_idx < 0) continue;

        entity_t *ent = &gs->entities.entities[ent_idx];
        qk_player_state_t *ps = &ent->data.player;

        if (ps->alive_state != QK_PSTATE_ALIVE) continue;

        qk_usercmd_t *cmd = &ps->last_cmd;

        // update view angles
        ps->pitch = cmd->pitch;
        ps->yaw = cmd->yaw;

        // weapon switch request
        if (cmd->weapon_select != 0 &&
            (qk_weapon_id_t)cmd->weapon_select != ps->weapon &&
            cmd->weapon_select < QK_WEAPON_COUNT &&
            ps->ammo[cmd->weapon_select] > 0) {
            g_weapon_switch(ent, (qk_weapon_id_t)cmd->weapon_select);
        }

        // weapon tick handles firing
        g_weapon_tick(gs, ent, tick_dt_ms);
    }
}
