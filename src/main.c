/*
 * QUICKEN Engine - Main Entry Point
 *
 * Full game loop: window, renderer, physics, gameplay, netcode (loopback),
 * input, HUD. This is the client executable.
 *
 * See docs/plans/INTEGRATION.md Section 3.3 for the loop design.
 */

#include <stdio.h>
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

/* ---- Camera Construction ---- */

static void build_perspective(f32 *out, f32 fov_deg, f32 aspect, f32 znear, f32 zfar) {
    memset(out, 0, 16 * sizeof(f32));
    f32 fov_rad = fov_deg * (3.14159265f / 180.0f);
    f32 f = 1.0f / tanf(fov_rad * 0.5f);

    out[0]  = f / aspect;
    out[5]  = -f;           /* Vulkan Y-down in clip space */
    out[10] = zfar / (znear - zfar);
    out[11] = -1.0f;
    out[14] = (znear * zfar) / (znear - zfar);
}

static void build_view(f32 *out, f32 pos_x, f32 pos_y, f32 pos_z,
                        f32 pitch_deg, f32 yaw_deg) {
    f32 p = pitch_deg * (3.14159265f / 180.0f);
    f32 y = yaw_deg   * (3.14159265f / 180.0f);

    f32 cp = cosf(p), sp = sinf(p);
    f32 cy = cosf(y), sy = sinf(y);

    /* forward = direction the camera looks */
    f32 fx = cp * cy;
    f32 fy = cp * sy;
    f32 fz = sp;

    /* right = cross(forward, world_up) */
    f32 rx = sy;
    f32 ry = -cy;
    f32 rz = 0.0f;

    /* up = cross(right, forward) */
    f32 ux = ry * fz - rz * fy;
    f32 uy = rz * fx - rx * fz;
    f32 uz = rx * fy - ry * fx;

    /* View matrix (column-major) */
    memset(out, 0, 16 * sizeof(f32));
    out[0] = rx;  out[4] = ry;  out[8]  = rz;
    out[1] = ux;  out[5] = uy;  out[9]  = uz;
    out[2] = -fx; out[6] = -fy; out[10] = -fz;

    out[12] = -(rx * pos_x + ry * pos_y + rz * pos_z);
    out[13] = -(ux * pos_x + uy * pos_y + uz * pos_z);
    out[14] = -(-fx * pos_x + (-fy) * pos_y + (-fz) * pos_z);
    out[15] = 1.0f;
}

static void mat4_mul(f32 *out, const f32 *a, const f32 *b) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = 0.0f;
            for (int k = 0; k < 4; k++) {
                out[i * 4 + j] += a[k * 4 + j] * b[i * 4 + k];
            }
        }
    }
}

static qk_camera_t build_camera(f32 pos_x, f32 pos_y, f32 pos_z,
                                  f32 pitch, f32 yaw,
                                  f32 aspect) {
    qk_camera_t cam;
    f32 proj[16], view[16];

    build_perspective(proj, 90.0f, aspect, 0.1f, 4096.0f);

    /* Eye position is at player origin + eye height */
    f32 eye_z = pos_z + 26.0f;
    build_view(view, pos_x, pos_y, eye_z, pitch, yaw);

    mat4_mul(cam.view_projection, proj, view);
    cam.position[0] = pos_x;
    cam.position[1] = pos_y;
    cam.position[2] = eye_z;

    return cam;
}

/* ---- Server Tick ---- */

static void server_tick(qk_phys_world_t *phys_world) {
    /* 1. Read inputs from all connected clients */
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        qk_usercmd_t cmd;
        if (qk_net_server_get_input(i, &cmd)) {
            qk_game_player_command(i, &cmd);
        }
    }

    /* 2. Run gameplay tick (mode logic, weapons, physics, combat) */
    qk_game_tick(phys_world, QK_TICK_DT);

    /* 3. Pack entity states for netcode snapshot */
    for (u32 i = 0; i < qk_game_get_entity_count(); i++) {
        n_entity_state_t net_state;
        qk_game_pack_entity((u8)i, &net_state);
        qk_net_server_set_entity((u8)i, &net_state);
    }

    /* 4. Netcode broadcasts snapshots to all clients */
    qk_net_server_tick();
}

/* ---- Main ---- */

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

    /* ---- Parse arguments ---- */
    const char *map_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        }
    }

    /* ---- Create window ---- */
    qk_window_t *window = NULL;
    qk_window_config_t wc = {0};
    wc.width = 1280;
    wc.height = 720;
    wc.title = "QUICKEN Engine";

    qk_result_t res = qk_window_create(&wc, &window);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to create window (%d)\n", res);
        return 1;
    }

    u32 win_w, win_h;
    qk_window_get_size(window, &win_w, &win_h);

    /* ---- Init renderer ---- */
    qk_renderer_config_t rc = {0};
    rc.sdl_window = qk_window_get_native_handle(window);
    rc.render_width = 1920;
    rc.render_height = 1080;
    rc.window_width = win_w;
    rc.window_height = win_h;
    rc.aspect_fit = true;
    rc.vsync = false;

    res = qk_renderer_init(&rc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init renderer (%d)\n", res);
        qk_window_destroy(window);
        return 1;
    }

    /* ---- Load map ---- */
    qk_map_data_t map_data;
    memset(&map_data, 0, sizeof(map_data));
    bool map_loaded = false;

    if (map_path) {
        res = qk_map_load(map_path, &map_data);
        if (res == QK_SUCCESS) {
            map_loaded = true;
            printf("Map loaded: %s\n", map_path);
        } else {
            fprintf(stderr, "Warning: Failed to load map '%s' (%d), using test room\n",
                    map_path, res);
        }
    }

    /* ---- Create physics world ---- */
    qk_phys_world_t *phys_world = NULL;
    if (map_loaded && map_data.collision.brush_count > 0) {
        phys_world = qk_physics_world_create(&map_data.collision);
    }
    if (!phys_world) {
        printf("Using hardcoded test room for physics.\n");
        phys_world = qk_physics_world_create_test_room();
    }

    /* ---- Upload world geometry to renderer ---- */
    if (map_loaded && map_data.vertex_count > 0) {
        res = qk_renderer_upload_world(map_data.vertices, map_data.vertex_count,
                                        map_data.indices, map_data.index_count,
                                        map_data.surfaces, map_data.surface_count);
        if (res != QK_SUCCESS) {
            fprintf(stderr, "Warning: Failed to upload world geometry (%d)\n", res);
        }
    }

    /* ---- Init gameplay ---- */
    qk_game_config_t gc = {0};
    res = qk_game_init(&gc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init gameplay (%d)\n", res);
        goto shutdown;
    }

    /* ---- Init netcode (server + client, loopback) ---- */
    qk_net_server_config_t nsc = {0};
    nsc.server_port = 0;
    nsc.max_clients = QK_MAX_PLAYERS;
    nsc.tick_rate = (f64)QK_TICK_RATE;

    res = qk_net_server_init(&nsc);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to init netcode server (%d)\n", res);
        goto shutdown;
    }

    qk_net_client_config_t ncc = {0};
    ncc.interp_delay = 0.020;

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
    printf("Connected as client %u (loopback)\n", local_client_id);

    /* ---- Connect local player to game ---- */
    res = qk_game_player_connect(local_client_id, "Player", QK_TEAM_ALPHA);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to connect player (%d)\n", res);
        goto shutdown;
    }

    /* Spawn the player */
    qk_player_state_t *ps = qk_game_get_player_state_mut(local_client_id);
    if (ps) {
        ps->alive_state = QK_PSTATE_ALIVE;
        ps->health = QK_CA_SPAWN_HEALTH;
        ps->armor = QK_CA_SPAWN_ARMOR;
        ps->weapon = QK_WEAPON_ROCKET;
        ps->ammo[QK_WEAPON_ROCKET] = 50;
        ps->ammo[QK_WEAPON_RAIL] = 25;
        ps->ammo[QK_WEAPON_LG] = 150;

        vec3_t spawn_pos = {0.0f, 0.0f, 24.0f};
        if (map_loaded && map_data.spawn_count > 0) {
            spawn_pos = map_data.spawn_points[0].origin;
            ps->yaw = map_data.spawn_points[0].yaw;
        }
        qk_physics_player_init(ps, spawn_pos);
    }

    /* ---- Main loop ---- */
    printf("Entering main loop...\n");

    f64 prev_time = qk_platform_time_now();
    f32 server_accumulator = 0.0f;
    qk_input_state_t input_state;
    bool running = true;
    u32 frame_count = 0;
    f64 fps_timer = 0.0;
    u32 fps_display = 0;

    while (running) {
        f64 now = qk_platform_time_now();
        f32 real_dt = (f32)(now - prev_time);
        prev_time = now;

        /* Clamp dt to avoid spiral of death */
        if (real_dt > 0.1f) real_dt = 0.1f;
        if (real_dt < 0.0f) real_dt = 0.0f;

        /* FPS counter */
        fps_timer += (f64)real_dt;
        frame_count++;
        if (fps_timer >= 1.0) {
            fps_display = frame_count;
            frame_count = 0;
            fps_timer -= 1.0;
        }

        /* ---- Server-side (loopback, runs in same process) ---- */
        server_accumulator += real_dt;
        while (server_accumulator >= QK_TICK_DT) {
            server_tick(phys_world);
            server_accumulator -= QK_TICK_DT;
        }

        /* ---- Client-side ---- */

        /* 1. Poll OS input */
        qk_input_poll(&input_state);
        if (input_state.quit_requested) {
            running = false;
            break;
        }

        /* 2. Build usercmd from raw input */
        u32 server_time = qk_net_server_get_tick() * QK_TICK_DT_MS_NOM;
        qk_usercmd_t cmd = qk_input_build_usercmd(&input_state, server_time);

        /* 3. Send input to server (via loopback) */
        qk_net_client_send_input(&cmd);

        /* 4. Client tick (processes received snapshots) */
        qk_net_client_tick();

        /* 5. Interpolate for rendering */
        f64 render_time = now - 0.020;
        qk_net_client_interpolate(render_time);

        /* 6. Build camera from local player state */
        const qk_player_state_t *local_ps = qk_game_get_player_state(local_client_id);
        f32 cam_x = 0, cam_y = 0, cam_z = 24, cam_pitch = 0, cam_yaw = 0;
        if (local_ps) {
            cam_x = local_ps->origin.x;
            cam_y = local_ps->origin.y;
            cam_z = local_ps->origin.z;
            cam_pitch = local_ps->pitch;
            cam_yaw = local_ps->yaw;
        }

        /* Handle window resize */
        qk_window_get_size(window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
            qk_renderer_handle_window_resize(win_w, win_h);
        }

        f32 aspect = (win_w > 0 && win_h > 0) ?
            (f32)rc.render_width / (f32)rc.render_height : 16.0f / 9.0f;
        qk_camera_t camera = build_camera(cam_x, cam_y, cam_z,
                                            cam_pitch, cam_yaw, aspect);

        /* 7. Render world */
        qk_renderer_begin_frame(&camera);
        qk_renderer_draw_world();

        /* 8. Draw entities (capsules for players, spheres for projectiles) */
        const qk_interp_state_t *interp = qk_net_client_get_interp_state();
        if (interp) {
            for (u32 i = 0; i < QK_MAX_ENTITIES; i++) {
                const qk_interp_entity_t *ie = &interp->entities[i];
                if (!ie->active) continue;

                if (ie->entity_type == 1) {
                    u32 color = 0x00FF00FF;
                    qk_renderer_draw_capsule(ie->pos_x, ie->pos_y, ie->pos_z,
                                              15.0f, 28.0f, ie->yaw, color);
                } else if (ie->entity_type == 2) {
                    qk_renderer_draw_sphere(ie->pos_x, ie->pos_y, ie->pos_z,
                                             4.0f, 0xFF8800FF);
                }
            }
        }

        /* 9. HUD */
        if (local_ps) {
            const qk_ca_state_t *ca = qk_game_get_ca_state();
            qk_ui_draw_hud(local_ps, ca,
                            (f32)rc.render_width, (f32)rc.render_height);
        }

        /* FPS display */
        {
            char fps_buf[16];
            snprintf(fps_buf, sizeof(fps_buf), "%u fps", fps_display);
            qk_ui_draw_text(10.0f, 10.0f, fps_buf, 16.0f, 0x00FF00FF);
        }

        /* Speed display */
        if (local_ps) {
            f32 speed = sqrtf(local_ps->velocity.x * local_ps->velocity.x +
                              local_ps->velocity.y * local_ps->velocity.y);
            char speed_buf[32];
            snprintf(speed_buf, sizeof(speed_buf), "%.0f ups", (double)speed);
            qk_ui_draw_text(10.0f, 30.0f, speed_buf, 16.0f, 0xFFFF00FF);
        }

        /* 10. UI tick (fade timers) */
        qk_ui_tick((u32)(real_dt * 1000.0f));

        /* 11. Present */
        qk_renderer_end_frame();
    }

    printf("Exiting main loop.\n");

    /* ---- Shutdown ---- */
shutdown:
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
