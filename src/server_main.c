/*
 * QUICKEN Engine - Dedicated Server Entry Point
 *
 * Headless mode: no window, no renderer, no SDL3.
 * Runs the authoritative game simulation + netcode only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "quicken.h"
#include "qk_types.h"
#include "qk_arena.h"
#include "core/qk_platform.h"
#include "core/qk_map.h"
#include "physics/qk_physics.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"
#include "core/qk_prof.h"
#include "core/qk_cpuid.h"
#include "core/qk_simd_dispatch.h"

// --- Shutdown signal ---

static volatile int s_running = 1;

#ifdef QK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static BOOL WINAPI console_handler(DWORD type) {
    QK_UNUSED(type);
    s_running = 0;
    return TRUE;
}
#else
static void signal_handler(int sig) {
    QK_UNUSED(sig);
    s_running = 0;
}
#endif

// --- Remote player tracking ---

static bool s_client_ready[QK_MAX_PLAYERS];

static void detect_remote_players(void) {
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        bool ready = qk_net_server_is_client_map_ready(i);
        if (ready && !s_client_ready[i]) {
            printf("Player %u connected.\n", (u32)i);
            qk_game_player_connect(i, "Player", QK_TEAM_ALPHA);

            qk_player_state_t *ps = qk_game_get_player_state_mut(i);
            if (ps) {
                ps->alive_state = QK_PSTATE_SPECTATING;
            }
        } else if (!ready && s_client_ready[i]) {
            printf("Player %u disconnected.\n", (u32)i);
            qk_game_player_disconnect(i);
        }
        s_client_ready[i] = ready;
    }
}

// --- Server tick ---

static void server_tick(qk_phys_world_t *phys_world) {
    // Read inputs from all connected clients
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        qk_usercmd_t cmd;
        if (qk_net_server_get_input(i, &cmd)) {
            qk_game_player_command(i, &cmd);
        }
    }

    // Run gameplay tick
    qk_game_tick(phys_world, QK_TICK_DT);

    // Pack entity states for netcode
    for (u32 i = 0; i < qk_game_get_entity_count(); i++) {
        n_entity_state_t net_state;
        qk_game_pack_entity((u8)i, &net_state);
        if (net_state.entity_type == 0) {
            qk_net_server_remove_entity((u8)i);
        } else {
            qk_net_server_set_entity((u8)i, &net_state);
        }
    }

    // Netcode broadcasts snapshots
    qk_net_server_tick();
}

// --- Main ---

int main(int argc, char *argv[]) {
    /* Detect CPU features before anything else */
    qk_cpuid_detect();

    printf("QUICKEN Dedicated Server v%d.%d.%d\n",
           QUICKEN_VERSION_MAJOR, QUICKEN_VERSION_MINOR, QUICKEN_VERSION_PATCH);
#ifdef QUICKEN_DEBUG
    printf("Build: Debug\n");
#else
    printf("Build: Release\n");
#endif
    qk_cpuid_print();
    printf("SIMD tier: %s\n", qk_simd_tier_name(qk_simd_get_tier()));
    printf("\n");

    // --- Parse arguments ---
    const char *map_name = NULL;
    u16 port = 27960;
    u32 max_clients = QK_MAX_PLAYERS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-map") == 0 && i + 1 < argc) {
            map_name = argv[++i];
        } else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc) {
            port = (u16)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-maxclients") == 0 && i + 1 < argc) {
            max_clients = (u32)atoi(argv[++i]);
            if (max_clients > QK_MAX_PLAYERS) max_clients = QK_MAX_PLAYERS;
            if (max_clients == 0) max_clients = 1;
        }
    }

    if (!map_name) {
        fprintf(stderr, "Usage: quicken-server -map <name> [-port %u] [-maxclients %u]\n",
                27960, QK_MAX_PLAYERS);
        return 1;
    }

    // --- Install signal handler ---
#ifdef QK_PLATFORM_WINDOWS
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    // --- Load map ---
    char path[512];
    bool found = false;

    snprintf(path, sizeof(path), "assets/maps/%s", map_name);
    { FILE *f = fopen(path, "rb"); if (f) { fclose(f); found = true; } }

    if (!found) {
        const char *exts[] = { ".bsp", ".map" };
        for (int e = 0; e < 2 && !found; e++) {
            snprintf(path, sizeof(path), "assets/maps/%s%s", map_name, exts[e]);
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); found = true; }
        }
    }

    if (!found) {
        snprintf(path, sizeof(path), "%s", map_name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); found = true; }
    }

    if (!found) {
        fprintf(stderr, "FATAL: Map not found: %s\n", map_name);
        return 1;
    }

    qk_map_data_t map_data = {0};
    qk_result_t res = qk_map_load(path, &map_data);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to load map '%s' (%d)\n", path, res);
        return 1;
    }
    printf("Map loaded: %s\n", path);

    // --- Init physics ---
    qk_phys_world_t *phys_world = NULL;
    if (map_data.collision.brush_count > 0)
        phys_world = qk_physics_world_create(&map_data.collision);
    if (!phys_world)
        phys_world = qk_physics_world_create_test_room();
    printf("Physics world: OK (%u brushes)\n", map_data.collision.brush_count);

    // --- Init gameplay ---
    qk_game_config_t gc = {0};
    res = qk_game_init(&gc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init gameplay (%d)\n", res);
        return 1;
    }

    qk_game_load_triggers(map_data.teleporters, map_data.teleporter_count,
                           map_data.jump_pads, map_data.jump_pad_count);
    printf("Gameplay: OK\n");

    // --- Init netcode ---
    qk_net_server_config_t nsc = {
        .server_port = port,
        .max_clients = max_clients,
        .tick_rate = (f64)QK_TICK_RATE,
    };

    res = qk_net_server_init(&nsc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init server on port %u (%d)\n", (u32)port, res);
        goto shutdown;
    }
    qk_net_server_set_map(path);
    printf("Server listening on port %u (max %u clients)\n", (u32)port, max_clients);

    QK_PROF_INIT();

    // --- Tick loop ---
    printf("\nServer running. Press Ctrl+C to stop.\n\n");

    memset(s_client_ready, 0, sizeof(s_client_ready));
    f64 prev_time = qk_platform_time_now();
    f32 accumulator = 0.0f;

    while (s_running) {
        QK_PROF_FRAME_BEGIN();

        f64 now = qk_platform_time_now();
        f32 dt = (f32)(now - prev_time);
        prev_time = now;

        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.0f) dt = 0.0f;

        accumulator += dt;

        QK_PROF_ZONE_BEGIN("server_tick");
        while (accumulator >= QK_TICK_DT) {
            detect_remote_players();
            server_tick(phys_world);
            accumulator -= QK_TICK_DT;
        }
        QK_PROF_ZONE_END("server_tick");

        QK_PROF_FRAME_END();

        /* Sleep to avoid burning CPU. Target slightly under tick interval
         * so we don't overshoot and miss ticks. */
        f32 sleep_ms = (QK_TICK_DT - accumulator) * 1000.0f - 1.0f;
        if (sleep_ms > 1.0f) {
            qk_platform_sleep((u32)sleep_ms);
        }
    }

    printf("\nShutting down...\n");

    // --- Shutdown ---
shutdown:
    QK_PROF_SHUTDOWN();
    qk_net_server_shutdown();
    qk_game_shutdown();
    qk_physics_world_destroy(phys_world);
    qk_map_free(&map_data);

    printf("Clean shutdown.\n");
    return 0;
}
