/*
 * QUICKEN Engine - Clan Arena Mode
 *
 * Round state machine: warmup -> countdown -> playing -> round_end -> match_end.
 * Teams spawn with full health/armor/weapons. Rounds until one team eliminated.
 */

#include "g_internal.h"

// --- Default spawn points (hardcoded until map loader provides them) ---
static const qk_spawn_point_t s_alpha_spawns[] = {
    {{ 100.0f,  100.0f, 0.0f}, 0.0f},
    {{ 100.0f, -100.0f, 0.0f}, 0.0f},
    {{ 200.0f,  100.0f, 0.0f}, 0.0f},
    {{ 200.0f, -100.0f, 0.0f}, 0.0f},
};

static const qk_spawn_point_t s_beta_spawns[] = {
    {{-100.0f,  100.0f, 0.0f}, 180.0f},
    {{-100.0f, -100.0f, 0.0f}, 180.0f},
    {{-200.0f,  100.0f, 0.0f}, 180.0f},
    {{-200.0f, -100.0f, 0.0f}, 180.0f},
};

#define NUM_ALPHA_SPAWNS (sizeof(s_alpha_spawns) / sizeof(s_alpha_spawns[0]))
#define NUM_BETA_SPAWNS  (sizeof(s_beta_spawns) / sizeof(s_beta_spawns[0]))

// --- Init ---
void g_ca_init(qk_game_state_t *gs) {
    gs->ca.state = CA_STATE_WARMUP;
    gs->ca.state_timer_ms = 0;
    gs->ca.score_alpha = 0;
    gs->ca.score_beta = 0;
    gs->ca.round_number = 0;
    gs->ca.alive_alpha = 0;
    gs->ca.alive_beta = 0;
}

// --- Count alive players per team ---
void g_ca_count_alive(qk_game_state_t *gs) {
    u8 alive_a = 0, alive_b = 0;

    for (entity_t *e = g_entity_first(&gs->entities, ENTITY_PLAYER);
         e; e = g_entity_next(&gs->entities, e, ENTITY_PLAYER)) {
        qk_player_state_t *ps = &e->data.player;
        if (ps->alive_state != QK_PSTATE_ALIVE) continue;

        if (ps->team == QK_TEAM_ALPHA) alive_a++;
        else if (ps->team == QK_TEAM_BETA) alive_b++;
    }

    gs->ca.alive_alpha = alive_a;
    gs->ca.alive_beta = alive_b;
}

// --- Start Countdown ---
void g_ca_start_countdown(qk_game_state_t *gs) {
    gs->ca.state = CA_STATE_COUNTDOWN;
    gs->ca.state_timer_ms = gs->countdown_time_ms;
}

// --- Begin Round (transition from COUNTDOWN -> PLAYING) ---
void g_ca_begin_round(qk_game_state_t *gs) {
    gs->ca.round_number++;

    // destroy all projectiles
    entity_t *proj = g_entity_first(&gs->entities, ENTITY_PROJECTILE);
    while (proj) {
        entity_t *next = g_entity_next(&gs->entities, proj, ENTITY_PROJECTILE);
        g_entity_free(&gs->entities, proj);
        proj = next;
    }

    // spawn all players at their team spawn points
    u8 alpha_idx = 0, beta_idx = 0;

    for (entity_t *e = g_entity_first(&gs->entities, ENTITY_PLAYER);
         e; e = g_entity_next(&gs->entities, e, ENTITY_PLAYER)) {
        qk_player_state_t *ps = &e->data.player;

        if (ps->team == QK_TEAM_ALPHA) {
            u8 si = alpha_idx % NUM_ALPHA_SPAWNS;
            g_player_spawn_ca(e, s_alpha_spawns[si].origin, s_alpha_spawns[si].yaw);
            ps->team = QK_TEAM_ALPHA;
            alpha_idx++;
        } else if (ps->team == QK_TEAM_BETA) {
            u8 si = beta_idx % NUM_BETA_SPAWNS;
            g_player_spawn_ca(e, s_beta_spawns[si].origin, s_beta_spawns[si].yaw);
            ps->team = QK_TEAM_BETA;
            beta_idx++;
        }
    }

    g_ca_count_alive(gs);

    // push round start event
    game_event_t evt = {
        .type = GEVT_ROUND_START,
        .server_time = gs->server_time_ms,
        .data.round_start = { .round_number = gs->ca.round_number },
    };
    g_event_push(&gs->events, &evt);
}

// --- End Round (one team eliminated) ---
void g_ca_end_round(qk_game_state_t *gs) {
    u8 winner = 0;

    if (gs->ca.alive_alpha == 0 && gs->ca.alive_beta > 0) {
        gs->ca.score_beta++;
        winner = QK_TEAM_BETA;
    } else if (gs->ca.alive_beta == 0 && gs->ca.alive_alpha > 0) {
        gs->ca.score_alpha++;
        winner = QK_TEAM_ALPHA;
    }
    // both zero = draw round, no score change

    gs->ca.state = CA_STATE_ROUND_END;
    gs->ca.state_timer_ms = QK_CA_ROUND_END_MS;

    // push round end event
    game_event_t evt = {
        .type = GEVT_ROUND_END,
        .server_time = gs->server_time_ms,
        .data.round_end = {
            .winner_team = winner,
            .score_a = gs->ca.score_alpha,
            .score_b = gs->ca.score_beta,
        },
    };
    g_event_push(&gs->events, &evt);
}

// --- End Round on Timeout ---
void g_ca_end_round_timeout(qk_game_state_t *gs) {
    // sum health+armor for each team
    i32 total_alpha = 0, total_beta = 0;

    for (entity_t *e = g_entity_first(&gs->entities, ENTITY_PLAYER);
         e; e = g_entity_next(&gs->entities, e, ENTITY_PLAYER)) {
        qk_player_state_t *ps = &e->data.player;
        if (ps->alive_state != QK_PSTATE_ALIVE) continue;

        i32 total = (i32)ps->health + (i32)ps->armor;
        if (ps->team == QK_TEAM_ALPHA) total_alpha += total;
        else if (ps->team == QK_TEAM_BETA) total_beta += total;
    }

    u8 winner = 0;
    if (total_alpha > total_beta) {
        gs->ca.score_alpha++;
        winner = QK_TEAM_ALPHA;
    } else if (total_beta > total_alpha) {
        gs->ca.score_beta++;
        winner = QK_TEAM_BETA;
    }
    // equal = draw, no score change

    gs->ca.state = CA_STATE_ROUND_END;
    gs->ca.state_timer_ms = QK_CA_ROUND_END_MS;

    game_event_t evt = {
        .type = GEVT_ROUND_END,
        .server_time = gs->server_time_ms,
        .data.round_end = {
            .winner_team = winner,
            .score_a = gs->ca.score_alpha,
            .score_b = gs->ca.score_beta,
        },
    };
    g_event_push(&gs->events, &evt);
}

// --- CA Tick (called once per server tick) ---
void g_ca_tick(qk_game_state_t *gs, u32 dt_ms) {
    switch (gs->ca.state) {
    case CA_STATE_WARMUP:
        // allow free movement, no damage. wait for ready-up or admin force.
        break;

    case CA_STATE_COUNTDOWN:
        gs->ca.state_timer_ms -= min_u32(dt_ms, gs->ca.state_timer_ms);
        if (gs->ca.state_timer_ms == 0) {
            g_ca_begin_round(gs);
            gs->ca.state = CA_STATE_PLAYING;
            gs->ca.state_timer_ms = gs->round_time_limit_ms;
        }
        break;

    case CA_STATE_PLAYING:
        gs->ca.state_timer_ms -= min_u32(dt_ms, gs->ca.state_timer_ms);

        g_ca_count_alive(gs);

        if (gs->ca.alive_alpha == 0 || gs->ca.alive_beta == 0) {
            g_ca_end_round(gs);
        } else if (gs->ca.state_timer_ms == 0) {
            g_ca_end_round_timeout(gs);
        }
        break;

    case CA_STATE_ROUND_END:
        gs->ca.state_timer_ms -= min_u32(dt_ms, gs->ca.state_timer_ms);
        if (gs->ca.state_timer_ms == 0) {
            if (gs->ca.score_alpha >= gs->rounds_to_win ||
                gs->ca.score_beta >= gs->rounds_to_win) {
                gs->ca.state = CA_STATE_MATCH_END;

                game_event_t evt = {
                    .type = GEVT_MATCH_END,
                    .server_time = gs->server_time_ms,
                    .data.match_end = {
                        .winner_team = (gs->ca.score_alpha >= gs->rounds_to_win)
                            ? QK_TEAM_ALPHA : QK_TEAM_BETA,
                    },
                };
                g_event_push(&gs->events, &evt);
            } else {
                g_ca_start_countdown(gs);
            }
        }
        break;

    case CA_STATE_MATCH_END:
        // display final scores. wait for admin restart.
        break;

    default:
        break;
    }
}
