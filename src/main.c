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
#include "core/qk_cvar.h"
#include "core/qk_perf.h"
#include "core/qk_demo.h"
#include "ui/qk_console.h"

/* ---- File-static state for demo system access ---- */
static u8 s_local_client_id;
static const char *s_map_path;
static f64 s_demo_play_start_time;

/* ---- Cached cvar pointers (set during init, read each frame) ---- */

static qk_cvar_t *s_cvar_fov;
static qk_cvar_t *s_cvar_r_width;
static qk_cvar_t *s_cvar_r_height;
static qk_cvar_t *s_cvar_r_vsync;
static qk_cvar_t *s_cvar_r_windowwidth;
static qk_cvar_t *s_cvar_r_windowheight;
static qk_cvar_t *s_cvar_r_fullscreen;
static qk_cvar_t *s_cvar_r_perflog;

/* Window pointer for cvar callbacks */
static qk_window_t *s_window;

/* ---- perflog callback ---- */

static void cb_perflog_changed(qk_cvar_t *cvar) {
    qk_perf_set_enabled(cvar->value.b);
}

/* ---- vid_restart callback ---- */

static void cb_render_cvar_changed(qk_cvar_t *cvar) {
    QK_UNUSED(cvar);
    qk_console_printf("Run 'vid_restart' to apply.");
}

/* ---- Window size callback (immediate, windowed mode only) ---- */

static void cb_window_size_changed(qk_cvar_t *cvar) {
    QK_UNUSED(cvar);
    if (!s_window || !s_cvar_r_windowwidth || !s_cvar_r_windowheight) return;
    if (qk_window_is_fullscreen(s_window)) return;

    u32 w = (u32)s_cvar_r_windowwidth->value.i;
    u32 h = (u32)s_cvar_r_windowheight->value.i;
    qk_window_set_size(s_window, w, h);
    qk_renderer_handle_window_resize(w, h);
}

/* ---- Fullscreen callback ---- */

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

    f32 fov = (s_cvar_fov ? s_cvar_fov->value.f : 90.0f);
    build_perspective(proj, fov, aspect, 0.1f, 4096.0f);

    /* Eye position is at player origin + eye height */
    f32 eye_z = pos_z + 26.0f;
    build_view(view, pos_x, pos_y, eye_z, pitch, yaw);

    mat4_mul(cam.view_projection, proj, view);
    cam.position[0] = pos_x;
    cam.position[1] = pos_y;
    cam.position[2] = eye_z;

    return cam;
}

/* ---- Grid Texture ---- */

static qk_texture_id_t create_grid_texture(void) {
    #define GRID_SIZE 256
    #define GRID_LINE 4   /* line width in pixels */
    #define GRID_CELL 32  /* cell size in pixels */

    static u8 pixels[GRID_SIZE * GRID_SIZE * 4];

    for (u32 y = 0; y < GRID_SIZE; y++) {
        for (u32 x = 0; x < GRID_SIZE; x++) {
            u32 idx = (y * GRID_SIZE + x) * 4;
            bool on_line = (x % GRID_CELL) < GRID_LINE ||
                           (y % GRID_CELL) < GRID_LINE;
            u8 val = on_line ? 0x80 : 0x50;
            pixels[idx + 0] = val;
            pixels[idx + 1] = val;
            pixels[idx + 2] = val;
            pixels[idx + 3] = 0xFF;
        }
    }

    qk_texture_id_t id = qk_renderer_upload_texture(pixels, GRID_SIZE, GRID_SIZE, 4);

    #undef GRID_SIZE
    #undef GRID_LINE
    #undef GRID_CELL
    return id;
}

/* ---- Test Room Render Geometry ---- */

static void upload_test_room_geometry(qk_texture_id_t tex_id) {
    /*
     * Match the physics test room: interior [-1024,1024] x [-1024,1024] x [0,256].
     * 6 faces (floor, ceiling, 4 walls), 24 verts, 36 indices, 6 surfaces.
     */
    #define TR_HALF  1024.0f
    #define TR_TOP   256.0f
    #define TR_UV_SCALE 128.0f

    static const struct { f32 p[3]; f32 n[3]; } face_data[6][4] = {
        /* Floor (Z=0, normal up) */
        { {{-TR_HALF,-TR_HALF,0}, {0,0,1}},  {{ TR_HALF,-TR_HALF,0}, {0,0,1}},
          {{ TR_HALF, TR_HALF,0}, {0,0,1}},  {{-TR_HALF, TR_HALF,0}, {0,0,1}} },
        /* Ceiling (Z=256, normal down) */
        { {{-TR_HALF,-TR_HALF,TR_TOP}, {0,0,-1}},  {{-TR_HALF, TR_HALF,TR_TOP}, {0,0,-1}},
          {{ TR_HALF, TR_HALF,TR_TOP}, {0,0,-1}},  {{ TR_HALF,-TR_HALF,TR_TOP}, {0,0,-1}} },
        /* +X wall (normal inward -X) */
        { {{ TR_HALF, TR_HALF,0}, {-1,0,0}},  {{ TR_HALF,-TR_HALF,0}, {-1,0,0}},
          {{ TR_HALF,-TR_HALF,TR_TOP}, {-1,0,0}},  {{ TR_HALF, TR_HALF,TR_TOP}, {-1,0,0}} },
        /* -X wall (normal inward +X) */
        { {{-TR_HALF,-TR_HALF,0}, {1,0,0}},  {{-TR_HALF, TR_HALF,0}, {1,0,0}},
          {{-TR_HALF, TR_HALF,TR_TOP}, {1,0,0}},  {{-TR_HALF,-TR_HALF,TR_TOP}, {1,0,0}} },
        /* +Y wall (normal inward -Y) */
        { {{-TR_HALF, TR_HALF,0}, {0,-1,0}},  {{ TR_HALF, TR_HALF,0}, {0,-1,0}},
          {{ TR_HALF, TR_HALF,TR_TOP}, {0,-1,0}},  {{-TR_HALF, TR_HALF,TR_TOP}, {0,-1,0}} },
        /* -Y wall (normal inward +Y) */
        { {{ TR_HALF,-TR_HALF,0}, {0,1,0}},  {{-TR_HALF,-TR_HALF,0}, {0,1,0}},
          {{-TR_HALF,-TR_HALF,TR_TOP}, {0,1,0}},  {{ TR_HALF,-TR_HALF,TR_TOP}, {0,1,0}} },
    };

    /* UV axis indices per face: floor/ceiling use XY, X-walls use YZ, Y-walls use XZ */
    static const u32 uv_axis[6][2] = {
        {0, 1}, /* floor:    X, Y */
        {0, 1}, /* ceiling:  X, Y */
        {1, 2}, /* +X wall:  Y, Z */
        {1, 2}, /* -X wall:  Y, Z */
        {0, 2}, /* +Y wall:  X, Z */
        {0, 2}, /* -Y wall:  X, Z */
    };

    qk_world_vertex_t verts[24];
    u32 indices[36];
    qk_draw_surface_t surfaces[6];

    for (u32 f = 0; f < 6; f++) {
        u32 base = f * 4;
        for (u32 v = 0; v < 4; v++) {
            qk_world_vertex_t *wv = &verts[base + v];
            wv->position[0] = face_data[f][v].p[0];
            wv->position[1] = face_data[f][v].p[1];
            wv->position[2] = face_data[f][v].p[2];
            wv->normal[0]   = face_data[f][v].n[0];
            wv->normal[1]   = face_data[f][v].n[1];
            wv->normal[2]   = face_data[f][v].n[2];
            wv->uv[0] = wv->position[uv_axis[f][0]] / TR_UV_SCALE;
            wv->uv[1] = wv->position[uv_axis[f][1]] / TR_UV_SCALE;
            wv->texture_id = tex_id;
        }

        u32 bi = f * 6;
        indices[bi + 0] = base + 0;
        indices[bi + 1] = base + 1;
        indices[bi + 2] = base + 2;
        indices[bi + 3] = base + 0;
        indices[bi + 4] = base + 2;
        indices[bi + 5] = base + 3;

        surfaces[f].index_offset  = bi;
        surfaces[f].index_count   = 6;
        surfaces[f].vertex_offset = base;
        surfaces[f].texture_index = tex_id;
    }

    qk_result_t res = qk_renderer_upload_world(verts, 24, indices, 36, surfaces, 6);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "Warning: Failed to upload test room geometry (%d)\n", res);
    } else {
        printf("Test room geometry uploaded (6 faces, grid texture %u).\n", tex_id);
    }

    #undef TR_HALF
    #undef TR_TOP
    #undef TR_UV_SCALE
}

/* ---- Client Prediction State ---- */

#define CL_CMD_BUFFER_SIZE 128

typedef struct {
    qk_usercmd_t cmd;
    u32 sequence;
} cl_stored_cmd_t;

typedef struct {
    qk_player_state_t state;
    u32 sequence;
} cl_predicted_state_t;

static cl_stored_cmd_t      cl_cmd_buffer[CL_CMD_BUFFER_SIZE];
static cl_predicted_state_t cl_pred_history[CL_CMD_BUFFER_SIZE];
static qk_player_state_t   cl_predicted_ps;
static u32                  cl_cmd_sequence;
static bool                 cl_has_prediction;
static f32                  cl_pred_accumulator;
static u32                  cl_last_reconciled_ack;

/* ---- Reconciliation ---- */

static void cl_reconcile(u32 ack_sequence, qk_phys_world_t *world) {
    qk_player_state_t server_state;
    if (!qk_net_client_get_server_player_state(&server_state)) return;

    /* Find predicted state for ack_sequence */
    u32 ack_idx = ack_sequence % CL_CMD_BUFFER_SIZE;
    cl_predicted_state_t *predicted = &cl_pred_history[ack_idx];
    if (predicted->sequence != ack_sequence) return;

    /* Compare positions (epsilon = 0.1 units squared) */
    f32 dx = server_state.origin.x - predicted->state.origin.x;
    f32 dy = server_state.origin.y - predicted->state.origin.y;
    f32 dz = server_state.origin.z - predicted->state.origin.z;
    f32 dist_sq = dx * dx + dy * dy + dz * dz;

    if (dist_sq < 0.1f) return; /* No correction needed */

    /* Misprediction: snap to server state and replay */
    cl_predicted_ps = server_state;

    for (u32 seq = ack_sequence + 1; seq < cl_cmd_sequence; seq++) {
        u32 idx = seq % CL_CMD_BUFFER_SIZE;
        cl_stored_cmd_t *stored = &cl_cmd_buffer[idx];
        if (stored->sequence != seq) break;

        qk_physics_move(&cl_predicted_ps, &stored->cmd, world);

        cl_pred_history[idx].state = cl_predicted_ps;
        cl_pred_history[idx].sequence = seq;
    }
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

/* ---- Demo Console Commands ---- */

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
    s_map_path = map_path;

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
    s_window = window;

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

    /* ---- Init cvar + console ---- */
    qk_cvar_init();
    qk_console_init();

    /* Register game cvars */
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

    qk_perf_init();
    qk_demo_init();

    qk_console_register_cmd("vid_restart", cmd_vid_restart,
                             "Apply render setting changes");
    qk_console_register_cmd("demo_record", cmd_demo_record,
                             "Start recording a demo");
    qk_console_register_cmd("demo_stop", cmd_demo_stop,
                             "Stop recording or playing a demo");
    qk_console_register_cmd("demo_play", cmd_demo_play,
                             "Play back a recorded demo");

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
    } else {
        qk_texture_id_t grid_tex = create_grid_texture();
        upload_test_room_geometry(grid_tex);
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
    ncc.interp_delay = 0.0;     /* loopback: zero interpolation delay */

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

    /* ---- Init prediction state ---- */
    memset(cl_cmd_buffer, 0, sizeof(cl_cmd_buffer));
    memset(cl_pred_history, 0, sizeof(cl_pred_history));
    memset(&cl_predicted_ps, 0, sizeof(cl_predicted_ps));
    cl_cmd_sequence = 0;
    cl_has_prediction = false;
    cl_pred_accumulator = 0.0f;
    cl_last_reconciled_ack = 0;

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
        qk_perf_begin_frame();

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

        /* ---- 1. Poll input FIRST for minimum latency ---- */
        qk_input_poll(&input_state);
        if (input_state.quit_requested) {
            running = false;
            break;
        }

        /* ---- Game/Demo branch ---- */
        f32 cam_x = 0, cam_y = 0, cam_z = 24;
        f32 cam_pitch = 0, cam_yaw = 0;

        if (qk_demo_is_playing()) {
            /* ---- Demo playback path ---- */
            f64 elapsed = now - s_demo_play_start_time;
            u32 playback_tick = qk_demo_get_start_tick() +
                                (u32)(elapsed * (f64)QK_TICK_RATE);

            if (!qk_demo_play_tick(playback_tick)) {
                qk_demo_play_stop();
                qk_console_print("Demo playback ended.");
            }

            f64 render_time = (f64)playback_tick / (f64)QK_TICK_RATE - QK_TICK_DT_F64;
            qk_net_client_interpolate(render_time);

            /* Camera from POV entity */
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
        } else {
            /* ---- Normal game path ---- */

            /* 2. Client-side prediction at fixed tick rate */
            cl_pred_accumulator += real_dt;
            while (cl_pred_accumulator >= QK_TICK_DT) {
                u32 server_time = qk_net_server_get_tick() * QK_TICK_DT_MS_NOM;
                qk_usercmd_t cmd = qk_input_build_usercmd(&input_state, server_time);

                u32 cmd_idx = cl_cmd_sequence % CL_CMD_BUFFER_SIZE;
                cl_cmd_buffer[cmd_idx].cmd = cmd;
                cl_cmd_buffer[cmd_idx].sequence = cl_cmd_sequence;

                qk_net_client_send_input(&cmd);

                if (!cl_has_prediction) {
                    qk_player_state_t srv_state;
                    if (qk_net_client_get_server_player_state(&srv_state)) {
                        cl_predicted_ps = srv_state;
                        cl_has_prediction = true;
                    }
                }

                if (cl_has_prediction) {
                    qk_physics_move(&cl_predicted_ps, &cmd, phys_world);
                    cl_pred_history[cmd_idx].state = cl_predicted_ps;
                    cl_pred_history[cmd_idx].sequence = cl_cmd_sequence;
                }

                cl_cmd_sequence++;
                cl_pred_accumulator -= QK_TICK_DT;
            }

            /* 3. Server-side (loopback, runs in same process) */
            server_accumulator += real_dt;
            while (server_accumulator >= QK_TICK_DT) {
                server_tick(phys_world);
                server_accumulator -= QK_TICK_DT;
            }

            /* 4. Client tick (processes received snapshots) */
            qk_net_client_tick();

            /* 5. Reconciliation check */
            u32 ack = qk_net_client_get_server_cmd_ack();
            if (ack > cl_last_reconciled_ack && cl_has_prediction) {
                cl_reconcile(ack, phys_world);
                cl_last_reconciled_ack = ack;
            }

            /* 6. Interpolate OTHER entities */
            f64 render_time = now - QK_TICK_DT_F64;
            qk_net_client_interpolate(render_time);

            /* 7. Build camera from predicted state */
            if (cl_has_prediction) {
                f32 pred_alpha = cl_pred_accumulator / QK_TICK_DT;
                cam_x = cl_predicted_ps.origin.x + cl_predicted_ps.velocity.x * QK_TICK_DT * pred_alpha;
                cam_y = cl_predicted_ps.origin.y + cl_predicted_ps.velocity.y * QK_TICK_DT * pred_alpha;
                cam_z = cl_predicted_ps.origin.z + cl_predicted_ps.velocity.z * QK_TICK_DT * pred_alpha;
            }
            cam_pitch = qk_input_get_pitch();
            cam_yaw = qk_input_get_yaw();
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

        /* ---- 8. Render world ---- */
        qk_renderer_begin_frame(&camera);
        qk_renderer_draw_world();

        /* ---- 9. Draw entities (skip local player) ---- */
        const qk_interp_state_t *interp = qk_net_client_get_interp_state();
        if (interp) {
            for (u32 i = 0; i < QK_MAX_ENTITIES; i++) {
                const qk_interp_entity_t *ie = &interp->entities[i];
                if (!ie->active) continue;

                /* Skip local player capsule (first person) */
                if (i == (u32)local_client_id && ie->entity_type == 1) continue;

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

        /* ---- 10. HUD (health/armor from server state, speed from prediction) ---- */
        const qk_player_state_t *local_ps = qk_game_get_player_state(local_client_id);
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

        /* Speed display (from predicted state for responsiveness) */
        if (cl_has_prediction) {
            f32 speed = sqrtf(cl_predicted_ps.velocity.x * cl_predicted_ps.velocity.x +
                              cl_predicted_ps.velocity.y * cl_predicted_ps.velocity.y);
            char speed_buf[32];
            snprintf(speed_buf, sizeof(speed_buf), "%.0f ups", (double)speed);
            qk_ui_draw_text(10.0f, 30.0f, speed_buf, 16.0f, 0xFFFF00FF);
        }

        /* ---- 11. Console overlay ---- */
        qk_console_draw((f32)rc.render_width, (f32)rc.render_height, real_dt);

        /* ---- 12. UI tick (fade timers) ---- */
        qk_ui_tick((u32)(real_dt * 1000.0f));

        /* ---- 13. Present ---- */
        qk_renderer_end_frame();

        /* ---- 14. Profiler data ---- */
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

    /* ---- Shutdown ---- */
shutdown:
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
