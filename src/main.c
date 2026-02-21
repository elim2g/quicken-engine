/*
 * QUICKEN Engine - Main Entry Point
 *
 * Full game loop: window, renderer, physics, gameplay, netcode (loopback),
 * input, HUD. This is the client executable.
 *
 * See docs/plans/INTEGRATION.md Section 3.3 for the loop design.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "quicken.h"
#include "qk_arena.h"
#include "qk_types.h"
#include "qk_math.h"
#include "core/qk_platform.h"
#include "core/qk_window.h"
#include "core/qk_input.h"
#include "core/qk_map.h"
#include "physics/qk_physics.h"
#include "renderer/qk_renderer.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"
#include "ui/qk_ui.h"
#include "core/qk_cvar.h"
#include "core/qk_perf.h"
#include "core/qk_demo.h"
#include "ui/qk_console.h"

#include "client/cl_camera.h"
#include "client/cl_fx.h"
#include "client/cl_predict.h"
#include "client/cl_diag.h"
#include "client/cl_map.h"
#include "client/cl_testroom.h"

// --- File-static state for demo system access ---
static u8 s_local_client_id;
static const char *s_map_path;
static f64 s_demo_play_start_time;

// --- Deferred map change ---
static char s_pending_map[256];
static char s_loaded_map_path[512];

// --- Remote client tracking (for mid-session join detection) ---
static bool s_client_map_ready[QK_MAX_PLAYERS];

// --- Connection mode ---
typedef enum {
    CONN_MODE_LOCAL,        // loopback: hosting + playing locally
    CONN_MODE_CONNECTING,   // UDP handshake in progress
    CONN_MODE_LOADING_MAP,  // connected, loading server's map
    CONN_MODE_HANDSHAKING,  // map loaded, waiting for server confirmation
    CONN_MODE_REMOTE        // fully connected to remote server
} conn_mode_t;

static conn_mode_t s_conn_mode = CONN_MODE_LOCAL;
static f64         s_connect_start_time;

// --- Cached cvar pointers (set during init, read each frame) ---

static qk_cvar_t *s_cvar_fov;
static qk_cvar_t *s_cvar_r_width;
static qk_cvar_t *s_cvar_r_height;
static qk_cvar_t *s_cvar_r_vsync;
static qk_cvar_t *s_cvar_r_windowwidth;
static qk_cvar_t *s_cvar_r_windowheight;
static qk_cvar_t *s_cvar_r_fullscreen;
static qk_cvar_t *s_cvar_r_perflog;
static qk_cvar_t *s_cvar_r_ambient;
static qk_cvar_t *s_cvar_r_bloom_strength;

// Window pointer for cvar callbacks
static qk_window_t *s_window;

// --- perflog callback ---

static void cb_perflog_changed(qk_cvar_t *cvar) {
    qk_perf_set_enabled(cvar->value.b);
}

static void cb_ambient_changed(qk_cvar_t *cvar) {
    qk_renderer_set_ambient(cvar->value.f);
}

static void cb_bloom_strength_changed(qk_cvar_t *cvar) {
    qk_renderer_set_bloom_strength(cvar->value.f);
}

// --- vid_restart callback ---

static void cb_render_cvar_changed(qk_cvar_t *cvar) {
    QK_UNUSED(cvar);
    qk_console_printf("Run 'vid_restart' to apply.");
}

// --- Window size callback (immediate, windowed mode only) ---

static void cb_window_size_changed(qk_cvar_t *cvar) {
    QK_UNUSED(cvar);
    if (!s_window || !s_cvar_r_windowwidth || !s_cvar_r_windowheight) return;
    if (qk_window_is_fullscreen(s_window)) return;

    u32 w = (u32)s_cvar_r_windowwidth->value.i;
    u32 h = (u32)s_cvar_r_windowheight->value.i;
    qk_window_set_size(s_window, w, h);
    qk_renderer_handle_window_resize(w, h);
}

// --- Fullscreen callback ---

static void cb_fullscreen_changed(qk_cvar_t *cvar) {
    if (!s_window) return;
    qk_window_set_fullscreen(s_window, cvar->value.b);

    u32 w, h;
    qk_window_get_size(s_window, &w, &h);
    if (w > 0 && h > 0) {
        qk_renderer_handle_window_resize(w, h);
    }
}

static void cmd_vid_restart(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);

    if (s_cvar_r_width && s_cvar_r_height) {
        qk_renderer_set_render_resolution(
            (u32)s_cvar_r_width->value.i,
            (u32)s_cvar_r_height->value.i);
    }
    if (s_cvar_r_vsync) {
        qk_renderer_set_vsync(s_cvar_r_vsync->value.b);
    }
    qk_console_print("Video restarted.");
}

// --- Server Tick ---

static void server_tick(qk_phys_world_t *phys_world) {
    // 0. Detect remote client connects and disconnects
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        bool was_ready = s_client_map_ready[i];
        bool is_ready = qk_net_server_is_client_map_ready(i);

        if (is_ready && !was_ready) {
            qk_game_player_connect(i, "Remote", QK_TEAM_ALPHA);
            s_client_map_ready[i] = true;
        } else if (!is_ready && was_ready) {
            qk_game_player_disconnect(i);
            s_client_map_ready[i] = false;
        }
    }

    // 1. Read inputs from all connected clients
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        qk_usercmd_t cmd;
        if (qk_net_server_get_input(i, &cmd)) {
            qk_game_player_command(i, &cmd);
        }
    }

    // 2. Run gameplay tick (mode logic, weapons, physics, combat)
    qk_game_tick(phys_world, QK_TICK_DT);

    // 2b. Read explosion events from gameplay -> VFX
    {
        qk_explosion_event_t expl_events[32];
        u32 expl_count = qk_game_get_explosions(expl_events, 32);
        if (expl_count > 0) {
            cl_fx_add_explosions(expl_events, expl_count,
                                  qk_platform_time_now());
        }
    }

    // 3. Pack entity states for netcode snapshot
    for (u32 i = 0; i < qk_game_get_entity_count(); i++) {
        n_entity_state_t net_state;
        qk_game_pack_entity((u8)i, &net_state);
        if (net_state.entity_type == 0) {
            qk_net_server_remove_entity((u8)i);
        } else {
            qk_net_server_set_entity((u8)i, &net_state);
        }
    }

    // 4. Netcode broadcasts snapshots to all clients
    qk_net_server_tick();
}

// --- Map Console Command ---

static void cmd_map(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: map <name>");
        return;
    }
    if (s_conn_mode != CONN_MODE_LOCAL) {
        qk_console_print("Cannot change map while connected to a remote server. Disconnect first.");
        return;
    }
    snprintf(s_pending_map, sizeof(s_pending_map), "%s", argv[1]);
}

// --- Demo Console Commands ---

static void cmd_demo_record(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: demo_record <name>");
        return;
    }
    u32 start_tick = qk_net_server_get_tick();
    if (qk_demo_record_start(argv[1], s_local_client_id,
                              start_tick, s_map_path)) {
        qk_console_printf("Recording demo '%s'...", argv[1]);
    } else {
        qk_console_printf("Failed to start recording '%s'.", argv[1]);
    }
}

static void cmd_demo_stop(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);
    if (qk_demo_is_recording()) {
        qk_demo_record_stop();
        qk_console_print("Demo recording stopped.");
    } else if (qk_demo_is_playing()) {
        qk_demo_play_stop();
        qk_console_print("Demo playback stopped.");
    } else {
        qk_console_print("Not recording or playing.");
    }
}

static void cmd_demo_play(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: demo_play <name>");
        return;
    }
    if (qk_demo_play_start(argv[1])) {
        s_demo_play_start_time = qk_platform_time_now();
        qk_console_printf("Playing demo '%s'...", argv[1]);
    } else {
        qk_console_printf("Failed to play demo '%s'.", argv[1]);
    }
}

// --- Loopback Restoration Helpers ---

static void restore_loopback_netcode(void) {
    qk_net_server_config_t nsc = {
        .server_port = 0,
        .max_clients = QK_MAX_PLAYERS,
        .tick_rate = (f64)QK_TICK_RATE,
    };
    qk_net_server_init(&nsc);

    qk_net_client_config_t ncc = {
        .interp_delay = 0.0,
    };
    qk_net_client_init(&ncc);
    qk_net_client_connect_local();

    s_conn_mode = CONN_MODE_LOCAL;
}

static void restore_local_gameplay(void) {
    qk_game_shutdown();

    qk_game_config_t gc = {0};
    qk_game_init(&gc);

    u8 lid = qk_net_client_get_id();
    s_local_client_id = lid;
    memset(s_client_map_ready, 0, sizeof(s_client_map_ready));
    qk_game_player_connect(lid, "Player", QK_TEAM_ALPHA);
    s_client_map_ready[lid] = true;

    qk_player_state_t *ps = qk_game_get_player_state_mut(lid);
    if (ps) {
        ps->alive_state = QK_PSTATE_ALIVE;
        ps->health = QK_CA_SPAWN_HEALTH;
        ps->armor = QK_CA_SPAWN_ARMOR;
        ps->weapon = QK_WEAPON_ROCKET;
        ps->ammo[QK_WEAPON_ROCKET] = 50;
        ps->ammo[QK_WEAPON_RAIL] = 25;
        ps->ammo[QK_WEAPON_LG] = 150;
        qk_physics_player_init(ps, (vec3_t){0.0f, 0.0f, 24.0f});
    }

    cl_predict_reset();
}

// --- Connect / Disconnect Console Commands ---

static void cmd_connect(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: connect <ip>:<port>");
        return;
    }
    if (s_conn_mode != CONN_MODE_LOCAL) {
        qk_console_print("Already connecting or connected to a remote server. Disconnect first.");
        return;
    }

    char addr_buf[256];
    snprintf(addr_buf, sizeof(addr_buf), "%s", argv[1]);
    char *colon = strrchr(addr_buf, ':');
    if (!colon) {
        qk_console_print("Invalid address. Use <ip>:<port> (e.g. 127.0.0.1:27960)");
        return;
    }
    *colon = '\0';
    u16 port = (u16)atoi(colon + 1);
    if (port == 0) {
        qk_console_print("Invalid port number.");
        return;
    }

    qk_console_printf("Connecting to %s:%u...", addr_buf, (u32)port);

    qk_net_client_disconnect();
    qk_net_client_shutdown();
    qk_net_server_shutdown();

    qk_net_client_config_t ncc = {
        .interp_delay = 0.020,
    };
    qk_net_client_init(&ncc);

    qk_result_t res = qk_net_client_connect_remote(addr_buf, port);
    if (res != QK_SUCCESS) {
        qk_console_printf("Failed to connect: error %d", res);
        qk_net_client_shutdown();
        restore_loopback_netcode();
        return;
    }

    s_conn_mode = CONN_MODE_CONNECTING;
    s_connect_start_time = qk_platform_time_now();
}

static void cmd_disconnect(i32 argc, const char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);

    if (s_conn_mode == CONN_MODE_LOCAL) {
        qk_console_print("Not connected to a remote server.");
        return;
    }

    qk_console_print("Disconnecting...");

    qk_net_client_disconnect();
    qk_net_client_shutdown();
    restore_loopback_netcode();
    restore_local_gameplay();

    qk_console_print("Disconnected. Returned to local server.");
}

// --- Helper: set up player after map load ---

static void setup_player_for_map(u8 client_id, const qk_map_data_t *map) {
    memset(s_client_map_ready, 0, sizeof(s_client_map_ready));
    qk_game_player_connect(client_id, "Player", QK_TEAM_ALPHA);
    s_client_map_ready[client_id] = true;

    qk_player_state_t *ps = qk_game_get_player_state_mut(client_id);
    if (ps) {
        ps->alive_state = QK_PSTATE_ALIVE;
        ps->health = QK_CA_SPAWN_HEALTH;
        ps->armor = QK_CA_SPAWN_ARMOR;
        ps->weapon = QK_WEAPON_ROCKET;
        ps->ammo[QK_WEAPON_ROCKET] = 50;
        ps->ammo[QK_WEAPON_RAIL] = 25;
        ps->ammo[QK_WEAPON_LG] = 150;

        vec3_t spawn = {0.0f, 0.0f, 24.0f};
        if (map && map->spawn_count > 0) {
            spawn = map->spawn_points[0].origin;
            ps->yaw = map->spawn_points[0].yaw;
        }
        qk_physics_player_init(ps, spawn);
    }
}

// --- Main ---

int main(int argc, char *argv[]) {
    QK_UNUSED(argc);

    printf("QUICKEN Engine v%d.%d.%d\n",
           QUICKEN_VERSION_MAJOR, QUICKEN_VERSION_MINOR, QUICKEN_VERSION_PATCH);
#ifdef QUICKEN_DEBUG
    printf("Build: Debug\n");
#else
    printf("Build: Release\n");
#endif
    printf("\n");

    // --- Parse arguments ---
    const char *map_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        }
    }
    s_map_path = map_path;

    // --- Create window ---
    qk_window_t *window = NULL;
    qk_window_config_t wc = {
        .width = 1280,
        .height = 720,
        .title = "QUICKEN Engine",
    };

    qk_result_t res = qk_window_create(&wc, &window);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to create window (%d)\n", res);
        return 1;
    }
    s_window = window;

    u32 win_w, win_h;
    qk_window_get_size(window, &win_w, &win_h);

    // --- Init renderer ---
    qk_renderer_config_t rc = {
        .sdl_window = qk_window_get_native_handle(window),
        .render_width = 1920,
        .render_height = 1080,
        .window_width = win_w,
        .window_height = win_h,
        .aspect_fit = true,
        .vsync = false,
    };

    res = qk_renderer_init(&rc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init renderer (%d)\n", res);
        qk_window_destroy(window);
        return 1;
    }

    // --- Init cvar + console ---
    qk_cvar_init();
    qk_console_init();

    // Register game cvars
    qk_cvar_register_float("sensitivity", 0.022f, 0.001f, 10.0f,
                            QK_CVAR_ARCHIVE, NULL);
    s_cvar_fov = qk_cvar_register_float("fov", 90.0f, 60.0f, 140.0f,
                                          QK_CVAR_ARCHIVE, NULL);
    s_cvar_r_vsync = qk_cvar_register_bool("r_vsync", false,
                                             QK_CVAR_ARCHIVE,
                                             cb_render_cvar_changed);
    s_cvar_r_width = qk_cvar_register_int("r_width", 1920, 640, 7680,
                                            QK_CVAR_ARCHIVE,
                                            cb_render_cvar_changed);
    s_cvar_r_height = qk_cvar_register_int("r_height", 1080, 480, 4320,
                                             QK_CVAR_ARCHIVE,
                                             cb_render_cvar_changed);
    qk_cvar_register_string("name", "Player", QK_CVAR_ARCHIVE, NULL);

    s_cvar_r_windowwidth = qk_cvar_register_int("r_windowwidth", 1280, 320, 7680,
                                                  QK_CVAR_ARCHIVE,
                                                  cb_window_size_changed);
    s_cvar_r_windowheight = qk_cvar_register_int("r_windowheight", 720, 240, 4320,
                                                   QK_CVAR_ARCHIVE,
                                                   cb_window_size_changed);
    s_cvar_r_fullscreen = qk_cvar_register_bool("r_fullscreen", false,
                                                  QK_CVAR_ARCHIVE,
                                                  cb_fullscreen_changed);
    s_cvar_r_perflog = qk_cvar_register_bool("r_perflog", false, 0,
                                               cb_perflog_changed);
    s_cvar_r_ambient = qk_cvar_register_float("r_ambient", 0.0125f, 0.0f, 2.0f,
                                                QK_CVAR_ARCHIVE,
                                                cb_ambient_changed);
    s_cvar_r_bloom_strength = qk_cvar_register_float("r_bloom_strength", 0.3f,
                                                       0.0f, 2.0f,
                                                       QK_CVAR_ARCHIVE,
                                                       cb_bloom_strength_changed);

    qk_perf_init();
    qk_demo_init();
    cl_diag_init();
    cl_fx_init();
    cl_predict_init();

    qk_console_register_cmd("map", cmd_map,
                             "Load a map by name (e.g. map asylum)");
    qk_console_register_cmd("vid_restart", cmd_vid_restart,
                             "Apply render setting changes");
    qk_console_register_cmd("demo_record", cmd_demo_record,
                             "Start recording a demo");
    qk_console_register_cmd("demo_stop", cmd_demo_stop,
                             "Stop recording or playing a demo");
    qk_console_register_cmd("demo_play", cmd_demo_play,
                             "Play back a recorded demo");
    qk_console_register_cmd("diag", cl_diag_cmd,
                             "Diagnostic trace: diag start|stop");
    qk_console_register_cmd("connect", cmd_connect,
                             "Connect to a remote server (connect <ip>:<port>)");
    qk_console_register_cmd("disconnect", cmd_disconnect,
                             "Disconnect from remote server");

    // Manually fire off callbacks since the renderer is already init
    cb_ambient_changed(s_cvar_r_ambient);
    cb_bloom_strength_changed(s_cvar_r_bloom_strength);

    // --- Initial world (test room as baseline) ---
    qk_map_data_t map_data = {0};
    bool map_loaded = false;

    qk_phys_world_t *phys_world = qk_physics_world_create_test_room();
    qk_texture_id_t grid_tex = cl_testroom_create_texture();
    cl_testroom_upload_geometry(grid_tex);

    // --- Init gameplay ---
    qk_game_config_t gc = {0};
    res = qk_game_init(&gc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init gameplay (%d)\n", res);
        goto shutdown;
    }

    // --- Init netcode (server + client, loopback) ---
    qk_net_server_config_t nsc = {
        .server_port = 0,
        .max_clients = QK_MAX_PLAYERS,
        .tick_rate = (f64)QK_TICK_RATE,
    };

    res = qk_net_server_init(&nsc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init netcode server (%d)\n", res);
        goto shutdown;
    }

    qk_net_client_config_t ncc = {
        .interp_delay = 0.0,
    };

    res = qk_net_client_init(&ncc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init netcode client (%d)\n", res);
        goto shutdown;
    }

    res = qk_net_client_connect_local();
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed loopback connect (%d)\n", res);
        goto shutdown;
    }

    u8 local_client_id = qk_net_client_get_id();
    s_local_client_id = local_client_id;
    printf("Connected as client %u (loopback)\n", local_client_id);

    // --- Connect local player to game ---
    res = qk_game_player_connect(local_client_id, "Player", QK_TEAM_ALPHA);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to connect player (%d)\n", res);
        goto shutdown;
    }
    s_client_map_ready[local_client_id] = true;

    // Spawn the player at default position (map load will respawn)
    {
        qk_player_state_t *ps = qk_game_get_player_state_mut(local_client_id);
        if (ps) {
            ps->alive_state = QK_PSTATE_ALIVE;
            ps->health = QK_CA_SPAWN_HEALTH;
            ps->armor = QK_CA_SPAWN_ARMOR;
            ps->weapon = QK_WEAPON_ROCKET;
            ps->ammo[QK_WEAPON_ROCKET] = 50;
            ps->ammo[QK_WEAPON_RAIL] = 25;
            ps->ammo[QK_WEAPON_LG] = 150;
            qk_physics_player_init(ps, (vec3_t){0.0f, 0.0f, 24.0f});
        }
    }

    // --- Queue command-line map for deferred loading (same path as console) ---
    if (map_path) {
        snprintf(s_pending_map, sizeof(s_pending_map), "%s", map_path);
    }

    // --- Main loop ---
    printf("Entering main loop...\n");

    f64 prev_time = qk_platform_time_now();
    f32 server_accumulator = 0.0f;
    qk_input_state_t input_state;
    bool running = true;
    u32 frame_count = 0;
    f64 fps_timer = 0.0;
    u32 fps_display = 0;

    while (running) {
        qk_perf_begin_frame();

        f64 now = qk_platform_time_now();
        f32 real_dt = (f32)(now - prev_time);
        prev_time = now;

        // Clamp dt to avoid spiral of death
        if (real_dt > 0.1f) real_dt = 0.1f;
        if (real_dt < 0.0f) real_dt = 0.0f;

        // FPS counter
        fps_timer += (f64)real_dt;
        frame_count++;
        if (fps_timer >= 1.0) {
            fps_display = frame_count;
            frame_count = 0;
            fps_timer -= 1.0;
        }

        // --- 1. Poll input FIRST for minimum latency ---
        qk_input_poll(&input_state);
        if (input_state.quit_requested) {
            running = false;
            break;
        }

        // Sync local_client_id from file-static (console commands may change it)
        local_client_id = s_local_client_id;

        // --- Handle deferred map change (local mode only) ---
        if (s_pending_map[0] != '\0' && s_conn_mode == CONN_MODE_LOCAL) {
            char path[512];
            if (!cl_map_resolve(s_pending_map, path, sizeof(path))) {
                qk_console_printf("Map not found: %s", s_pending_map);
            } else {
                qk_map_data_t new_map = {0};
                res = qk_map_load(path, &new_map);
                if (res != QK_SUCCESS) {
                    qk_console_printf("Failed to load map '%s' (%d)", s_pending_map, res);
                } else {
                    // === CLEAN SLATE: tear down everything ===
                    qk_net_client_disconnect();
                    qk_net_client_shutdown();
                    qk_net_server_shutdown();
                    qk_game_shutdown();
                    qk_renderer_free_world();
                    qk_physics_world_destroy(phys_world);
                    phys_world = NULL;
                    qk_map_free(&map_data);

                    // === REBUILD: fresh state from scratch ===
                    map_data = new_map;
                    map_loaded = true;
                    strncpy(s_loaded_map_path, path, sizeof(s_loaded_map_path) - 1);
                    s_loaded_map_path[sizeof(s_loaded_map_path) - 1] = '\0';
                    s_map_path = s_loaded_map_path;

                    phys_world = cl_map_build_world(&map_data, grid_tex);

                    // Game state (clean init)
                    qk_game_config_t gc2 = {0};
                    qk_game_init(&gc2);

                    // Netcode (full reinit)
                    qk_net_server_config_t nsc2 = {
                        .server_port = 0,
                        .max_clients = QK_MAX_PLAYERS,
                        .tick_rate = (f64)QK_TICK_RATE,
                    };
                    qk_net_server_init(&nsc2);
                    qk_net_server_set_map(path);

                    qk_net_client_config_t ncc2 = {
                        .interp_delay = 0.0,
                    };
                    qk_net_client_init(&ncc2);
                    qk_net_client_connect_local();

                    local_client_id = qk_net_client_get_id();
                    s_local_client_id = local_client_id;

                    // Player (connect + spawn)
                    setup_player_for_map(local_client_id, &map_data);

                    // Triggers
                    qk_game_load_triggers(map_data.teleporters, map_data.teleporter_count,
                                           map_data.jump_pads, map_data.jump_pad_count);

                    // Reset client state
                    cl_predict_reset();
                    cl_fx_reset();
                    server_accumulator = 0.0f;

                    qk_net_client_notify_map_loaded(path);

                    /* Reset time reference so the next frame's dt
                     * doesn't include the map loading duration. */
                    prev_time = qk_platform_time_now();

                    qk_console_printf("Loaded: %s", path);
                }
            }
            s_pending_map[0] = '\0';
        }

        // --- Remote connection state machine ---
        if (s_conn_mode == CONN_MODE_CONNECTING) {
            qk_net_client_tick();
            qk_conn_state_t cs = qk_net_client_get_state();
            if (cs == QK_CONN_CONNECTED) {
                const char *server_map = qk_net_client_get_server_map();
                if (server_map && server_map[0] != '\0') {
                    qk_console_printf("Connected! Server map: %s", server_map);
                    s_conn_mode = CONN_MODE_LOADING_MAP;

                    char rpath[512];
                    if (!cl_map_resolve(server_map, rpath, sizeof(rpath))) {
                        qk_console_printf("Map not found locally: %s", server_map);
                        qk_console_print("Disconnecting...");
                        qk_net_client_disconnect();
                        qk_net_client_shutdown();
                        restore_loopback_netcode();
                    } else {
                        qk_map_data_t new_map = {0};
                        res = qk_map_load(rpath, &new_map);
                        if (res != QK_SUCCESS) {
                            qk_console_printf("Failed to load map '%s' (%d)", server_map, res);
                            qk_net_client_disconnect();
                            qk_net_client_shutdown();
                            restore_loopback_netcode();
                        } else {
                            // Tear down old game state (but NOT netcode)
                            qk_game_shutdown();
                            qk_renderer_free_world();
                            qk_physics_world_destroy(phys_world);
                            phys_world = NULL;
                            qk_map_free(&map_data);

                            // Rebuild world
                            map_data = new_map;
                            map_loaded = true;
                            strncpy(s_loaded_map_path, rpath, sizeof(s_loaded_map_path) - 1);
                            s_loaded_map_path[sizeof(s_loaded_map_path) - 1] = '\0';
                            s_map_path = s_loaded_map_path;

                            phys_world = cl_map_build_world(&map_data, grid_tex);

                            // Game state
                            qk_game_config_t gc2 = {0};
                            qk_game_init(&gc2);

                            local_client_id = qk_net_client_get_id();
                            s_local_client_id = local_client_id;
                            setup_player_for_map(local_client_id, &map_data);

                            qk_game_load_triggers(map_data.teleporters, map_data.teleporter_count,
                                                   map_data.jump_pads, map_data.jump_pad_count);

                            // Reset client state
                            cl_predict_reset();
                            cl_fx_reset();

                            qk_net_client_notify_map_loaded(rpath);
                            s_conn_mode = CONN_MODE_HANDSHAKING;
                            s_connect_start_time = qk_platform_time_now();
                            prev_time = s_connect_start_time;
                            qk_console_printf("Loaded: %s (waiting for server confirmation...)", rpath);
                        }
                    }
                }
            } else if (cs == QK_CONN_DISCONNECTED) {
                qk_console_print("Connection failed.");
                qk_net_client_shutdown();
                restore_loopback_netcode();
            } else if (now - s_connect_start_time > 10.0) {
                qk_console_print("Connection timed out.");
                qk_net_client_disconnect();
                qk_net_client_shutdown();
                restore_loopback_netcode();
            }
        }

        if (s_conn_mode == CONN_MODE_HANDSHAKING) {
            qk_net_client_tick();
            if (qk_net_client_is_map_ready()) {
                qk_console_print("Server confirmed map. Entering game.");
                s_conn_mode = CONN_MODE_REMOTE;
            } else if (qk_net_client_get_state() == QK_CONN_DISCONNECTED) {
                qk_console_print("Lost connection during handshake.");
                qk_net_client_shutdown();
                restore_loopback_netcode();
            } else if (now - s_connect_start_time > 10.0) {
                qk_console_print("Handshake timed out.");
                qk_net_client_disconnect();
                qk_net_client_shutdown();
                restore_loopback_netcode();
            }
        }

        // --- Game/Demo branch ---
        f32 cam_x = 0, cam_y = 0, cam_z = 24;
        f32 cam_pitch = 0, cam_yaw = 0;

        if (qk_demo_is_playing()) {
            // --- Demo playback path ---
            f64 elapsed = now - s_demo_play_start_time;
            u32 playback_tick = qk_demo_get_start_tick() +
                                (u32)(elapsed * (f64)QK_TICK_RATE);

            if (!qk_demo_play_tick(playback_tick)) {
                qk_demo_play_stop();
                qk_console_print("Demo playback ended.");
            }

            f64 render_time = (f64)playback_tick / (f64)QK_TICK_RATE - QK_TICK_DT_F64;
            qk_net_client_interpolate(render_time);

            u8 pov_id = qk_demo_get_pov_client_id();
            const qk_interp_state_t *pov = qk_net_client_get_interp_state();
            if (pov && pov->entities[pov_id].active) {
                cam_x = pov->entities[pov_id].pos_x;
                cam_y = pov->entities[pov_id].pos_y;
                cam_z = pov->entities[pov_id].pos_z;
            }
            const qk_usercmd_t *demo_cmd = qk_demo_get_last_usercmd();
            cam_pitch = demo_cmd->pitch;
            cam_yaw = demo_cmd->yaw;
        } else if (s_conn_mode == CONN_MODE_CONNECTING ||
                   s_conn_mode == CONN_MODE_LOADING_MAP ||
                   s_conn_mode == CONN_MODE_HANDSHAKING) {
            cam_pitch = qk_input_get_pitch();
            cam_yaw = qk_input_get_yaw();
        } else if (s_conn_mode == CONN_MODE_REMOTE) {
            // --- Remote play path ---
            cl_predict_tick(&input_state, phys_world, real_dt, true);

            qk_net_client_tick();
            cl_predict_reconcile(phys_world);

            {
                f64 input_tick = (f64)qk_net_client_get_input_sequence();
                f64 render_tick = input_tick - 2.0;
                if (render_tick < 0.0) render_tick = 0.0;
                f64 render_time = render_tick * QK_TICK_DT_F64;
                qk_net_client_interpolate(render_time);
            }

            if (cl_predict_has_state()) {
                const qk_player_state_t *pred = cl_predict_get_state();
                f32 pred_alpha = cl_predict_get_accumulator() / QK_TICK_DT;
                cam_x = pred->origin.x + pred->velocity.x * QK_TICK_DT * pred_alpha;
                cam_y = pred->origin.y + pred->velocity.y * QK_TICK_DT * pred_alpha;
                cam_z = pred->origin.z + pred->velocity.z * QK_TICK_DT * pred_alpha;
            }
            cam_pitch = qk_input_get_pitch();
            cam_yaw = qk_input_get_yaw();

            if (qk_net_client_get_state() == QK_CONN_DISCONNECTED) {
                qk_console_print("Lost connection to server.");
                qk_net_client_shutdown();
                restore_loopback_netcode();
                restore_local_gameplay();
                local_client_id = s_local_client_id;
            }
        } else {
            // --- Normal local game path ---
            cl_predict_tick(&input_state, phys_world, real_dt, false);

            server_accumulator += real_dt;
            while (server_accumulator >= QK_TICK_DT) {
                server_tick(phys_world);
                server_accumulator -= QK_TICK_DT;
            }

            qk_net_client_tick();
            cl_predict_reconcile(phys_world);

            {
                f64 srv_tick = (f64)qk_net_server_get_tick();
                f64 frac = (f64)server_accumulator / (f64)QK_TICK_DT;
                f64 render_tick = srv_tick + frac - 1.0;
                if (render_tick < 0.0) render_tick = 0.0;
                f64 render_time = render_tick * QK_TICK_DT_F64;
                qk_net_client_interpolate(render_time);
            }

            if (cl_predict_has_state()) {
                const qk_player_state_t *pred = cl_predict_get_state();
                f32 pred_alpha = cl_predict_get_accumulator() / QK_TICK_DT;
                cam_x = pred->origin.x + pred->velocity.x * QK_TICK_DT * pred_alpha;
                cam_y = pred->origin.y + pred->velocity.y * QK_TICK_DT * pred_alpha;
                cam_z = pred->origin.z + pred->velocity.z * QK_TICK_DT * pred_alpha;
            }
            cam_pitch = qk_input_get_pitch();
            cam_yaw = qk_input_get_yaw();
        }

        // Handle window resize
        qk_window_get_size(window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
            qk_renderer_handle_window_resize(win_w, win_h);
        }

        // Query current render resolution (may change after vid_restart)
        u32 render_w, render_h;
        qk_renderer_get_render_resolution(&render_w, &render_h);

        f32 fov = (s_cvar_fov ? s_cvar_fov->value.f : 90.0f);
        f32 aspect = (render_w > 0 && render_h > 0) ?
            (f32)render_w / (f32)render_h : 16.0f / 9.0f;
        qk_camera_t camera = cl_camera_build(cam_x, cam_y, cam_z,
                                               cam_pitch, cam_yaw,
                                               fov, aspect);

        // --- Render ---
        qk_renderer_begin_frame(&camera);
        qk_renderer_draw_world();

        // --- VFX (entities, smoke, explosions, viewmodel, beams) ---
        {
            const qk_player_state_t *pred = cl_predict_get_state();
            cl_fx_frame_t fx_frame = {
                .interp         = qk_net_client_get_interp_state(),
                .camera         = &camera,
                .input          = &input_state,
                .predicted_ps   = pred,
                .world          = phys_world,
                .local_client_id = local_client_id,
                .cam_pitch      = cam_pitch,
                .cam_yaw        = cam_yaw,
                .now            = now,
                .has_prediction = cl_predict_has_state(),
            };
            cl_fx_draw(&fx_frame);
        }

        // --- Diagnostic trace ---
        cl_diag_frame(now, real_dt, server_accumulator,
                       cl_predict_get_accumulator(), local_client_id,
                       qk_net_client_get_interp_state(),
                       cl_predict_has_state(), cl_predict_get_state());

        // --- HUD ---
        const qk_player_state_t *local_ps = qk_game_get_player_state(local_client_id);
        if (local_ps) {
            const qk_ca_state_t *ca = qk_game_get_ca_state();
            qk_ui_draw_hud(local_ps, ca,
                            (f32)render_w, (f32)render_h);
        }

        // FPS display
        {
            char fps_buf[16];
            snprintf(fps_buf, sizeof(fps_buf), "%u fps", fps_display);
            qk_ui_draw_text(10.0f, 10.0f, fps_buf, 16.0f, 0x00FF00FF);
        }

        // Speed display (from predicted state for responsiveness)
        if (cl_predict_has_state()) {
            const qk_player_state_t *pred = cl_predict_get_state();
            f32 speed = sqrtf(pred->velocity.x * pred->velocity.x +
                              pred->velocity.y * pred->velocity.y);
            char speed_buf[32];
            snprintf(speed_buf, sizeof(speed_buf), "%.0f ups", (double)speed);
            qk_ui_draw_text(10.0f, 30.0f, speed_buf, 16.0f, 0xFFFF00FF);

            char vz_buf[32];
            snprintf(vz_buf, sizeof(vz_buf), "vz %.0f", (double)pred->velocity.z);
            qk_ui_draw_text(10.0f, 48.0f, vz_buf, 16.0f, 0x00FFFFFF);
        }

        // Console renders in the overlay layer (compose pass at swapchain resolution),
        // so it uses window dimensions, not render resolution.
        qk_renderer_set_ui_layer(true);
        qk_console_draw((f32)win_w, (f32)win_h, real_dt);
        qk_renderer_set_ui_layer(false);

        // --- UI tick (fade timers) ---
        qk_ui_tick((u32)(real_dt * 1000.0f));

        // --- Present ---
        qk_renderer_end_frame();

        // --- Profiler data ---
        {
            qk_gpu_stats_t stats;
            qk_renderer_get_stats(&stats);
            qk_perf_end_frame(
                real_dt * 1000.0f,
                (f32)stats.gpu_frame_ms,
                (f32)stats.world_pass_ms,
                (f32)stats.compose_pass_ms,
                stats.draw_calls,
                stats.triangles,
                (u32)win_w, (u32)win_h,
                stats.fence_wait_ms,
                stats.acquire_ms);
        }
    }

    printf("Exiting main loop.\n");

    // --- Shutdown ---
shutdown:
    cl_diag_shutdown();
    qk_demo_shutdown();
    qk_perf_shutdown();
    qk_console_shutdown();
    qk_cvar_shutdown();
    qk_net_client_disconnect();
    qk_net_client_shutdown();
    qk_net_server_shutdown();
    qk_game_shutdown();
    qk_physics_world_destroy(phys_world);
    qk_renderer_free_world();
    qk_renderer_shutdown();
    qk_window_destroy(window);
    qk_map_free(&map_data);

    printf("Clean shutdown.\n");
    return 0;
}
