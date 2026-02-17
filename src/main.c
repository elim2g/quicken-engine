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

/* ---- File-static state for demo system access ---- */
static u8 s_local_client_id;
static const char *s_map_path;
static f64 s_demo_play_start_time;

/* ---- Deferred map change ---- */
static char s_pending_map[256];
static char s_loaded_map_path[512];

/* ---- Remote client tracking (for mid-session join detection) ---- */
static bool s_client_map_ready[QK_MAX_PLAYERS];

/* ---- Connection mode ---- */
typedef enum {
    CONN_MODE_LOCAL,        /* loopback: hosting + playing locally */
    CONN_MODE_CONNECTING,   /* UDP handshake in progress */
    CONN_MODE_LOADING_MAP,  /* connected, loading server's map */
    CONN_MODE_HANDSHAKING,  /* map loaded, waiting for server confirmation */
    CONN_MODE_REMOTE        /* fully connected to remote server */
} conn_mode_t;

static conn_mode_t s_conn_mode = CONN_MODE_LOCAL;
static f64         s_connect_start_time;

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
    #define GRID_CELL 64  /* cell size in pixels */

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

/* ---- Beam Effect State ---- */

#define MAX_RAIL_BEAMS 16
#define RAIL_BEAM_LIFETIME 1.5

typedef struct {
    f32     start[3];
    f32     end[3];
    f64     birth_time;
    u32     color;
    bool    active;
} rail_beam_t;

static rail_beam_t s_rail_beams[MAX_RAIL_BEAMS];
static u8          s_prev_flags[QK_MAX_ENTITIES];
static u32         s_rail_beam_next;

/* ---- Rocket Smoke Particles ---- */

#define SMOKE_POOL_SIZE       1024
#define SMOKE_MAX_AGE         2.0f
#define SMOKE_SPAWN_SPACING   8.0f
#define MAX_TRACKED_ROCKETS   32

typedef struct {
    f32 pos[3];
    f64 birth_time;
    f32 angle;      /* random billboard rotation (radians) */
} smoke_particle_t;

static smoke_particle_t s_smoke_pool[SMOKE_POOL_SIZE];
static u32 s_smoke_pool_head;

typedef struct {
    u32  entity_id;
    bool active;
    f32  last_pos[3];
    u32  last_tick;
} rocket_smoke_tracker_t;

static rocket_smoke_tracker_t s_rocket_trackers[MAX_TRACKED_ROCKETS];

/* ---- Explosion Effects ---- */

#define MAX_EXPLOSIONS        16
#define EXPLOSION_LIFETIME    1.0f

typedef struct {
    f32  pos[3];
    f32  radius;
    f64  birth_time;
    bool active;
} explosion_t;

static explosion_t s_explosions[MAX_EXPLOSIONS];
static u32         s_explosion_next;

/* ---- Diagnostic Trace ---- */

static FILE *s_diag_file;
static u32   s_diag_frame;

static void cmd_diag(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: diag start|stop");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        if (s_diag_file) {
            qk_console_print("diag: already running");
            return;
        }
        s_diag_file = fopen("diag_trace.log", "w");
        s_diag_frame = 0;
        if (s_diag_file) {
            fprintf(s_diag_file, "# QUICKEN diagnostic trace\n");
            fprintf(s_diag_file, "# Columns vary by line prefix. Greppable.\n\n");
            qk_console_print("diag: trace started -> diag_trace.log");
        } else {
            qk_console_print("diag: failed to open file");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        if (s_diag_file) {
            fclose(s_diag_file);
            s_diag_file = NULL;
            qk_console_printf("diag: stopped (%u frames written)", s_diag_frame);
        }
    }
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

    /* Always sync authoritative gameplay state from server
     * (weapon, ammo, health, alive_state, etc.) so changes
     * are visible immediately even when standing still. */
    cl_predicted_ps.weapon          = server_state.weapon;
    cl_predicted_ps.pending_weapon  = server_state.pending_weapon;
    cl_predicted_ps.weapon_time     = server_state.weapon_time;
    cl_predicted_ps.switch_time     = server_state.switch_time;
    memcpy(cl_predicted_ps.ammo, server_state.ammo, sizeof(server_state.ammo));
    cl_predicted_ps.health          = server_state.health;
    cl_predicted_ps.armor           = server_state.armor;
    cl_predicted_ps.alive_state     = server_state.alive_state;
    cl_predicted_ps.frags           = server_state.frags;
    cl_predicted_ps.deaths          = server_state.deaths;
    cl_predicted_ps.damage_given    = server_state.damage_given;
    cl_predicted_ps.damage_taken    = server_state.damage_taken;
    cl_predicted_ps.respawn_time    = server_state.respawn_time;

    /* Compare positions (epsilon = 0.1 units squared) */
    f32 dx = server_state.origin.x - predicted->state.origin.x;
    f32 dy = server_state.origin.y - predicted->state.origin.y;
    f32 dz = server_state.origin.z - predicted->state.origin.z;
    f32 dist_sq = dx * dx + dy * dy + dz * dz;

    if (dist_sq < 0.1f) return; /* Position matches, no replay needed */

    /* Position misprediction: snap to server state and replay */
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
    /* 0. Detect remote client connects and disconnects */
    for (u8 i = 0; i < QK_MAX_PLAYERS; i++) {
        bool was_ready = s_client_map_ready[i];
        bool is_ready = qk_net_server_is_client_map_ready(i);

        if (is_ready && !was_ready) {
            /* New client is map-ready: connect them to gameplay.
             * They start as spectators and join the next round. */
            qk_game_player_connect(i, "Remote", QK_TEAM_ALPHA);
            s_client_map_ready[i] = true;
        } else if (!is_ready && was_ready) {
            /* Client disconnected or lost map-ready: remove from gameplay */
            qk_game_player_disconnect(i);
            s_client_map_ready[i] = false;
        }
    }

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
        if (net_state.entity_type == 0) {
            qk_net_server_remove_entity((u8)i);
        } else {
            qk_net_server_set_entity((u8)i, &net_state);
        }
    }

    /* 4. Netcode broadcasts snapshots to all clients */
    qk_net_server_tick();
}

/* ---- Map Console Command ---- */

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

/* ---- Loopback Restoration Helpers ---- */

/* Restore loopback netcode (server + client). Caller must have already
 * shut down the previous client (and server if applicable). */
static void restore_loopback_netcode(void) {
    qk_net_server_config_t nsc = {0};
    nsc.server_port = 0;
    nsc.max_clients = QK_MAX_PLAYERS;
    nsc.tick_rate = (f64)QK_TICK_RATE;
    qk_net_server_init(&nsc);

    qk_net_client_config_t ncc = {0};
    ncc.interp_delay = 0.0;
    qk_net_client_init(&ncc);
    qk_net_client_connect_local();

    s_conn_mode = CONN_MODE_LOCAL;
}

/* Restore local gameplay after remote disconnect.
 * Reinits gameplay, connects local player with spawn state, resets prediction. */
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

    memset(cl_cmd_buffer, 0, sizeof(cl_cmd_buffer));
    memset(cl_pred_history, 0, sizeof(cl_pred_history));
    memset(&cl_predicted_ps, 0, sizeof(cl_predicted_ps));
    cl_cmd_sequence = 0;
    cl_has_prediction = false;
    cl_pred_accumulator = 0.0f;
    cl_last_reconciled_ack = 0;
}

/* ---- Connect / Disconnect Console Commands ---- */

static void cmd_connect(i32 argc, const char **argv) {
    if (argc < 2) {
        qk_console_print("Usage: connect <ip>:<port>");
        return;
    }
    if (s_conn_mode != CONN_MODE_LOCAL) {
        qk_console_print("Already connecting or connected to a remote server. Disconnect first.");
        return;
    }

    /* Parse address:port */
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

    /* Tear down loopback server + client */
    qk_net_client_disconnect();
    qk_net_client_shutdown();
    qk_net_server_shutdown();

    /* Init fresh client for remote connection */
    qk_net_client_config_t ncc = {0};
    ncc.interp_delay = 0.020;  /* 20ms interpolation delay for remote */
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
    qk_console_register_cmd("diag", cmd_diag,
                             "Diagnostic trace: diag start|stop");
    qk_console_register_cmd("connect", cmd_connect,
                             "Connect to a remote server (connect <ip>:<port>)");
    qk_console_register_cmd("disconnect", cmd_disconnect,
                             "Disconnect from remote server");

    /* ---- Initial world (test room as baseline) ---- */
    qk_map_data_t map_data;
    memset(&map_data, 0, sizeof(map_data));
    bool map_loaded = false;

    qk_phys_world_t *phys_world = qk_physics_world_create_test_room();
    qk_texture_id_t grid_tex = create_grid_texture();
    upload_test_room_geometry(grid_tex);

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
    s_client_map_ready[local_client_id] = true;

    /* Spawn the player at default position (map load will respawn) */
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

    /* ---- Init prediction state ---- */
    memset(cl_cmd_buffer, 0, sizeof(cl_cmd_buffer));
    memset(cl_pred_history, 0, sizeof(cl_pred_history));
    memset(&cl_predicted_ps, 0, sizeof(cl_predicted_ps));
    cl_cmd_sequence = 0;
    cl_has_prediction = false;
    cl_pred_accumulator = 0.0f;
    cl_last_reconciled_ack = 0;

    /* ---- Queue command-line map for deferred loading (same path as console) ---- */
    if (map_path) {
        snprintf(s_pending_map, sizeof(s_pending_map), "%s", map_path);
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

        /* Sync local_client_id from file-static (console commands may change it) */
        local_client_id = s_local_client_id;

        /* ---- Handle deferred map change (local mode only) ---- */
        if (s_pending_map[0] != '\0' && s_conn_mode == CONN_MODE_LOCAL) {
            char path[512];
            bool found = false;

            /* Try exact name in assets/maps/ first (handles "asylum.bsp") */
            snprintf(path, sizeof(path), "assets/maps/%s", s_pending_map);
            { FILE *mf = fopen(path, "rb"); if (mf) { fclose(mf); found = true; } }

            /* Then try appending extensions (handles bare "asylum") */
            if (!found) {
                const char *exts[] = { ".bsp", ".map" };
                for (int e = 0; e < 2 && !found; e++) {
                    snprintf(path, sizeof(path), "assets/maps/%s%s", s_pending_map, exts[e]);
                    FILE *mf = fopen(path, "rb");
                    if (mf) { fclose(mf); found = true; }
                }
            }

            /* Try as raw path (absolute or relative) */
            if (!found) {
                snprintf(path, sizeof(path), "%s", s_pending_map);
                FILE *mf = fopen(path, "rb");
                if (mf) { fclose(mf); found = true; }
            }

            if (!found) {
                qk_console_printf("Map not found: %s", s_pending_map);
            } else {
                /* Load new map data first (before tearing anything down) */
                qk_map_data_t new_map;
                memset(&new_map, 0, sizeof(new_map));
                res = qk_map_load(path, &new_map);
                if (res != QK_SUCCESS) {
                    qk_console_printf("Failed to load map '%s' (%d)", s_pending_map, res);
                } else {
                    /* === CLEAN SLATE: tear down everything === */
                    qk_net_client_disconnect();
                    qk_net_client_shutdown();
                    qk_net_server_shutdown();
                    qk_game_shutdown();
                    qk_renderer_free_world();
                    qk_physics_world_destroy(phys_world);
                    phys_world = NULL;
                    qk_map_free(&map_data);

                    /* === REBUILD: fresh state from scratch === */
                    map_data = new_map;
                    map_loaded = true;
                    strncpy(s_loaded_map_path, path, sizeof(s_loaded_map_path) - 1);
                    s_loaded_map_path[sizeof(s_loaded_map_path) - 1] = '\0';
                    s_map_path = s_loaded_map_path;

                    /* Physics world */
                    if (map_data.collision.brush_count > 0)
                        phys_world = qk_physics_world_create(&map_data.collision);
                    if (!phys_world)
                        phys_world = qk_physics_world_create_test_room();

                    /* Render geometry */
                    if (map_data.vertex_count > 0) {
                        for (u32 si = 0; si < map_data.surface_count; si++)
                            map_data.surfaces[si].texture_index = grid_tex;
                        qk_renderer_upload_world(map_data.vertices, map_data.vertex_count,
                                                  map_data.indices, map_data.index_count,
                                                  map_data.surfaces, map_data.surface_count);
                    }

                    /* Game state (clean init) */
                    qk_game_config_t gc = {0};
                    qk_game_init(&gc);

                    /* Netcode (full reinit) */
                    qk_net_server_config_t nsc = {0};
                    nsc.server_port = 0;
                    nsc.max_clients = QK_MAX_PLAYERS;
                    nsc.tick_rate = (f64)QK_TICK_RATE;
                    qk_net_server_init(&nsc);
                    qk_net_server_set_map(path);

                    qk_net_client_config_t ncc = {0};
                    ncc.interp_delay = 0.0;
                    qk_net_client_init(&ncc);
                    qk_net_client_connect_local();

                    local_client_id = qk_net_client_get_id();
                    s_local_client_id = local_client_id;

                    /* Player (connect + spawn) */
                    memset(s_client_map_ready, 0, sizeof(s_client_map_ready));
                    qk_game_player_connect(local_client_id, "Player", QK_TEAM_ALPHA);
                    s_client_map_ready[local_client_id] = true;
                    qk_player_state_t *mps = qk_game_get_player_state_mut(local_client_id);
                    if (mps) {
                        mps->alive_state = QK_PSTATE_ALIVE;
                        mps->health = QK_CA_SPAWN_HEALTH;
                        mps->armor = QK_CA_SPAWN_ARMOR;
                        mps->weapon = QK_WEAPON_ROCKET;
                        mps->ammo[QK_WEAPON_ROCKET] = 50;
                        mps->ammo[QK_WEAPON_RAIL] = 25;
                        mps->ammo[QK_WEAPON_LG] = 150;

                        vec3_t spawn = {0.0f, 0.0f, 24.0f};
                        if (map_data.spawn_count > 0) {
                            spawn = map_data.spawn_points[0].origin;
                            mps->yaw = map_data.spawn_points[0].yaw;
                        }
                        qk_physics_player_init(mps, spawn);
                    }

                    /* Load trigger volumes (teleporters + jump pads) */
                    qk_game_load_triggers(map_data.teleporters, map_data.teleporter_count,
                                           map_data.jump_pads, map_data.jump_pad_count);

                    /* Prediction + beam state (clean slate) */
                    memset(cl_cmd_buffer, 0, sizeof(cl_cmd_buffer));
                    memset(cl_pred_history, 0, sizeof(cl_pred_history));
                    memset(&cl_predicted_ps, 0, sizeof(cl_predicted_ps));
                    cl_cmd_sequence = 0;
                    cl_has_prediction = false;
                    cl_pred_accumulator = 0.0f;
                    cl_last_reconciled_ack = 0;
                    server_accumulator = 0.0f;
                    memset(s_rail_beams, 0, sizeof(s_rail_beams));
                    memset(s_prev_flags, 0, sizeof(s_prev_flags));
                    s_rail_beam_next = 0;
                    memset(s_smoke_pool, 0, sizeof(s_smoke_pool));
                    s_smoke_pool_head = 0;
                    memset(s_rocket_trackers, 0, sizeof(s_rocket_trackers));
                    memset(s_explosions, 0, sizeof(s_explosions));
                    s_explosion_next = 0;

                    /* Map handshake: notify netcode that map is loaded.
                     * For loopback this is fast-tracked (no round trip).
                     * For remote, this sends N_MSG_MAP_LOADED to server
                     * and the server won't send snapshots until confirmed. */
                    qk_net_client_notify_map_loaded(path);

                    /* Reset time reference so the next frame's dt
                     * doesn't include the map loading duration.
                     * Without this, the accumulators spike and cause
                     * a multi-tick batch that breaks delta encoding. */
                    prev_time = qk_platform_time_now();

                    qk_console_printf("Loaded: %s", path);
                }
            }
            s_pending_map[0] = '\0';
        }

        /* ---- Remote connection state machine ---- */
        if (s_conn_mode == CONN_MODE_CONNECTING) {
            qk_net_client_tick();  /* drive the UDP handshake */
            qk_conn_state_t cs = qk_net_client_get_state();
            if (cs == QK_CONN_CONNECTED) {
                const char *server_map = qk_net_client_get_server_map();
                if (server_map && server_map[0] != '\0') {
                    qk_console_printf("Connected! Server map: %s", server_map);
                    s_conn_mode = CONN_MODE_LOADING_MAP;

                    /* Load map for remote play (no netcode teardown) */
                    char rpath[512];
                    bool rfound = false;
                    snprintf(rpath, sizeof(rpath), "assets/maps/%s", server_map);
                    { FILE *mf = fopen(rpath, "rb"); if (mf) { fclose(mf); rfound = true; } }
                    if (!rfound) {
                        const char *exts[] = { ".bsp", ".map" };
                        for (int e = 0; e < 2 && !rfound; e++) {
                            snprintf(rpath, sizeof(rpath), "assets/maps/%s%s", server_map, exts[e]);
                            FILE *mf = fopen(rpath, "rb");
                            if (mf) { fclose(mf); rfound = true; }
                        }
                    }
                    if (!rfound) {
                        snprintf(rpath, sizeof(rpath), "%s", server_map);
                        FILE *mf = fopen(rpath, "rb");
                        if (mf) { fclose(mf); rfound = true; }
                    }

                    if (!rfound) {
                        qk_console_printf("Map not found locally: %s", server_map);
                        qk_console_print("Disconnecting...");
                        qk_net_client_disconnect();
                        qk_net_client_shutdown();
                        restore_loopback_netcode();
                    } else {
                        qk_map_data_t new_map;
                        memset(&new_map, 0, sizeof(new_map));
                        res = qk_map_load(rpath, &new_map);
                        if (res != QK_SUCCESS) {
                            qk_console_printf("Failed to load map '%s' (%d)", server_map, res);
                            qk_net_client_disconnect();
                            qk_net_client_shutdown();
                            restore_loopback_netcode();
                        } else {
                            /* Tear down old game state (but NOT netcode) */
                            qk_game_shutdown();
                            qk_renderer_free_world();
                            qk_physics_world_destroy(phys_world);
                            phys_world = NULL;
                            qk_map_free(&map_data);

                            /* Rebuild world */
                            map_data = new_map;
                            map_loaded = true;
                            strncpy(s_loaded_map_path, rpath, sizeof(s_loaded_map_path) - 1);
                            s_loaded_map_path[sizeof(s_loaded_map_path) - 1] = '\0';
                            s_map_path = s_loaded_map_path;

                            if (map_data.collision.brush_count > 0)
                                phys_world = qk_physics_world_create(&map_data.collision);
                            if (!phys_world)
                                phys_world = qk_physics_world_create_test_room();

                            if (map_data.vertex_count > 0) {
                                for (u32 si = 0; si < map_data.surface_count; si++)
                                    map_data.surfaces[si].texture_index = grid_tex;
                                qk_renderer_upload_world(map_data.vertices, map_data.vertex_count,
                                                          map_data.indices, map_data.index_count,
                                                          map_data.surfaces, map_data.surface_count);
                            }

                            /* Game state */
                            qk_game_config_t gc2 = {0};
                            qk_game_init(&gc2);

                            local_client_id = qk_net_client_get_id();
                            s_local_client_id = local_client_id;
                            memset(s_client_map_ready, 0, sizeof(s_client_map_ready));
                            qk_game_player_connect(local_client_id, "Player", QK_TEAM_ALPHA);
                            s_client_map_ready[local_client_id] = true;

                            qk_game_load_triggers(map_data.teleporters, map_data.teleporter_count,
                                                   map_data.jump_pads, map_data.jump_pad_count);

                            /* Reset prediction + effect state */
                            memset(cl_cmd_buffer, 0, sizeof(cl_cmd_buffer));
                            memset(cl_pred_history, 0, sizeof(cl_pred_history));
                            memset(&cl_predicted_ps, 0, sizeof(cl_predicted_ps));
                            cl_cmd_sequence = 0;
                            cl_has_prediction = false;
                            cl_pred_accumulator = 0.0f;
                            cl_last_reconciled_ack = 0;
                            memset(s_rail_beams, 0, sizeof(s_rail_beams));
                            memset(s_prev_flags, 0, sizeof(s_prev_flags));
                            s_rail_beam_next = 0;
                            memset(s_smoke_pool, 0, sizeof(s_smoke_pool));
                            s_smoke_pool_head = 0;
                            memset(s_rocket_trackers, 0, sizeof(s_rocket_trackers));
                            memset(s_explosions, 0, sizeof(s_explosions));
                            s_explosion_next = 0;

                            /* Handshake: notify server that map is loaded */
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
        } else if (s_conn_mode == CONN_MODE_CONNECTING ||
                   s_conn_mode == CONN_MODE_LOADING_MAP ||
                   s_conn_mode == CONN_MODE_HANDSHAKING) {
            /* Waiting for remote connection -- just show current view */
            cam_pitch = qk_input_get_pitch();
            cam_yaw = qk_input_get_yaw();
        } else if (s_conn_mode == CONN_MODE_REMOTE) {
            /* ---- Remote play path (no local server) ---- */

            /* Client-side prediction at fixed tick rate */
            cl_pred_accumulator += real_dt;
            while (cl_pred_accumulator >= QK_TICK_DT) {
                u32 server_time = cl_cmd_sequence * QK_TICK_DT_MS_NOM;
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

            /* No local server tick -- server runs remotely */

            /* Client tick (processes received snapshots from remote server) */
            qk_net_client_tick();

            /* Reconciliation check */
            u32 ack = qk_net_client_get_server_cmd_ack();
            if (ack > cl_last_reconciled_ack && cl_has_prediction) {
                cl_reconcile(ack, phys_world);
                cl_last_reconciled_ack = ack;
            }

            /* Interpolate other entities.
             * For remote, estimate the server's current tick from input_sequence
             * (initialized from server tick at connect, advances with local timing).
             * Render ~2 ticks behind to leave room for interpolation. */
            {
                f64 input_tick = (f64)qk_net_client_get_input_sequence();
                f64 render_tick = input_tick - 2.0;
                if (render_tick < 0.0) render_tick = 0.0;
                f64 render_time = render_tick * QK_TICK_DT_F64;
                qk_net_client_interpolate(render_time);
            }

            /* Build camera from predicted state */
            if (cl_has_prediction) {
                f32 pred_alpha = cl_pred_accumulator / QK_TICK_DT;
                cam_x = cl_predicted_ps.origin.x + cl_predicted_ps.velocity.x * QK_TICK_DT * pred_alpha;
                cam_y = cl_predicted_ps.origin.y + cl_predicted_ps.velocity.y * QK_TICK_DT * pred_alpha;
                cam_z = cl_predicted_ps.origin.z + cl_predicted_ps.velocity.z * QK_TICK_DT * pred_alpha;
            }
            cam_pitch = qk_input_get_pitch();
            cam_yaw = qk_input_get_yaw();

            /* Detect disconnect */
            if (qk_net_client_get_state() == QK_CONN_DISCONNECTED) {
                qk_console_print("Lost connection to server.");
                qk_net_client_shutdown();
                restore_loopback_netcode();
                restore_local_gameplay();
                local_client_id = s_local_client_id;
            }
        } else {
            /* ---- Normal local game path ---- */

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

            /* 6. Interpolate OTHER entities.
             * render_time must be in the server's time domain (seconds since
             * tick 0) so that render_tick lands between actual snapshot ticks.
             * We target one tick behind the latest server tick to leave room
             * for interpolation on both sides of the bracket. */
            {
                f64 srv_tick = (f64)qk_net_server_get_tick();
                f64 frac = (f64)server_accumulator / (f64)QK_TICK_DT;
                f64 render_tick = srv_tick + frac - 1.0;
                if (render_tick < 0.0) render_tick = 0.0;
                f64 render_time = render_tick * QK_TICK_DT_F64;
                qk_net_client_interpolate(render_time);
            }

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

                    /* Smoke trail: spawn particles when tick advances */
                    u32 cur_tick = qk_net_server_get_tick();
                    rocket_smoke_tracker_t *tracker = NULL;
                    for (u32 t = 0; t < MAX_TRACKED_ROCKETS; t++) {
                        if (s_rocket_trackers[t].active && s_rocket_trackers[t].entity_id == i) {
                            tracker = &s_rocket_trackers[t];
                            break;
                        }
                    }
                    if (!tracker) {
                        for (u32 t = 0; t < MAX_TRACKED_ROCKETS; t++) {
                            if (!s_rocket_trackers[t].active) {
                                tracker = &s_rocket_trackers[t];
                                tracker->active = true;
                                tracker->entity_id = i;
                                tracker->last_pos[0] = ie->pos_x;
                                tracker->last_pos[1] = ie->pos_y;
                                tracker->last_pos[2] = ie->pos_z;
                                tracker->last_tick = cur_tick;
                                break;
                            }
                        }
                    }
                    if (tracker && cur_tick > tracker->last_tick) {
                        f32 dx = ie->pos_x - tracker->last_pos[0];
                        f32 dy = ie->pos_y - tracker->last_pos[1];
                        f32 dz = ie->pos_z - tracker->last_pos[2];
                        f32 dist = sqrtf(dx*dx + dy*dy + dz*dz);

                        u32 num_puffs = (dist > 0.1f)
                            ? (u32)(dist / SMOKE_SPAWN_SPACING) + 1 : 1;

                        for (u32 p = 0; p < num_puffs; p++) {
                            f32 frac = (num_puffs > 1)
                                ? (f32)(p + 1) / (f32)num_puffs : 1.0f;
                            u32 slot = s_smoke_pool_head % SMOKE_POOL_SIZE;
                            smoke_particle_t *puff = &s_smoke_pool[slot];
                            s_smoke_pool_head++;
                            puff->pos[0] = tracker->last_pos[0] + dx * frac;
                            puff->pos[1] = tracker->last_pos[1] + dy * frac;
                            puff->pos[2] = tracker->last_pos[2] + dz * frac;
                            puff->birth_time = now;
                            /* Random rotation: hash the slot index */
                            u32 h = slot * 0x45d9f3bu;
                            h = ((h >> 16) ^ h) * 0x45d9f3bu;
                            h = (h >> 16) ^ h;
                            puff->angle = (f32)(h & 0xFFFF) / 65535.0f
                                          * 6.28318530f;
                        }

                        tracker->last_pos[0] = ie->pos_x;
                        tracker->last_pos[1] = ie->pos_y;
                        tracker->last_pos[2] = ie->pos_z;
                        tracker->last_tick = cur_tick;
                    }
                }
            }

            /* Expire rocket trackers -- spawn explosion when rocket disappears */
            for (u32 t = 0; t < MAX_TRACKED_ROCKETS; t++) {
                if (!s_rocket_trackers[t].active) continue;
                u32 eid = s_rocket_trackers[t].entity_id;
                const qk_interp_entity_t *ie = &interp->entities[eid];
                if (!ie->active || ie->entity_type != 2) {
                    /* Rocket just disappeared -- spawn explosion at last known pos */
                    explosion_t *ex = &s_explosions[s_explosion_next % MAX_EXPLOSIONS];
                    s_explosion_next++;
                    ex->active = true;
                    ex->pos[0] = s_rocket_trackers[t].last_pos[0];
                    ex->pos[1] = s_rocket_trackers[t].last_pos[1];
                    ex->pos[2] = s_rocket_trackers[t].last_pos[2];
                    ex->radius = 120.0f; /* rocket splash radius */
                    ex->birth_time = now;

                    s_rocket_trackers[t].active = false;
                }
            }
        }

        /* ---- 9a. Draw smoke particles (independent of rocket lifetime) ---- */
        qk_renderer_begin_smoke();
        for (u32 s = 0; s < SMOKE_POOL_SIZE; s++) {
            smoke_particle_t *p = &s_smoke_pool[s];
            if (p->birth_time == 0.0) continue;
            f32 age = (f32)(now - p->birth_time);
            if (age > SMOKE_MAX_AGE) {
                p->birth_time = 0.0;
                continue;
            }
            f32 frac = age / SMOKE_MAX_AGE;
            f32 fade = 1.0f - frac;
            fade = fade * fade; /* quadratic falloff */
            f32 half_size = 1.5f + frac * 5.0f;
            u8 grey = (u8)(80.0f * fade);
            u8 alpha = (u8)(180.0f * fade);
            u32 color = ((u32)grey << 24) | ((u32)grey << 16)
                       | ((u32)grey << 8) | alpha;
            qk_renderer_emit_smoke_puff(p->pos[0], p->pos[1], p->pos[2],
                                         half_size, color, p->angle);
        }
        qk_renderer_end_smoke();

        /* ---- 9a2. Draw active explosions ---- */
        for (u32 ex = 0; ex < MAX_EXPLOSIONS; ex++) {
            explosion_t *e = &s_explosions[ex];
            if (!e->active) continue;
            f32 age = (f32)(now - e->birth_time);
            if (age > EXPLOSION_LIFETIME) {
                e->active = false;
                continue;
            }
            f32 fade = 1.0f - (age / EXPLOSION_LIFETIME);
            qk_renderer_draw_explosion(e->pos[0], e->pos[1], e->pos[2],
                                        e->radius, age,
                                        1.0f, 0.6f, 0.1f, fade);
        }

        /* ---- 9b. Viewmodel (first-person weapon) ---- */
        if (cl_has_prediction && cl_predicted_ps.alive_state == QK_PSTATE_ALIVE) {
            qk_renderer_draw_viewmodel(
                cl_predicted_ps.weapon,
                cam_pitch, cam_yaw,
                (f32)now,
                input_state.mouse_buttons[0] && !input_state.console_active);
        }

        /* ---- 9c. Beam effects ---- */
        if (interp) {
            vec3_t zero_ext = {0, 0, 0};

            for (u32 i = 0; i < QK_MAX_ENTITIES; i++) {
                const qk_interp_entity_t *ie = &interp->entities[i];
                if (!ie->active || ie->entity_type != 1) {
                    s_prev_flags[i] = 0;
                    continue;
                }

                u8 cur_flags = ie->flags;
                u8 prev = s_prev_flags[i];
                bool firing_now = (cur_flags & QK_ENT_FLAG_FIRING) != 0;
                bool firing_prev = (prev & QK_ENT_FLAG_FIRING) != 0;

                /* Determine eye position and forward direction */
                f32 eye_x, eye_y, eye_z, fwd_pitch, fwd_yaw;
                bool is_local = (i == (u32)local_client_id);

                if (is_local && cl_has_prediction) {
                    /* Local player: camera position + raw input angles */
                    eye_x = camera.position[0];
                    eye_y = camera.position[1];
                    eye_z = camera.position[2];
                    fwd_pitch = cam_pitch;
                    fwd_yaw = cam_yaw;
                } else {
                    eye_x = ie->pos_x;
                    eye_y = ie->pos_y;
                    eye_z = ie->pos_z + 26.0f;
                    fwd_pitch = ie->pitch;
                    fwd_yaw = ie->yaw;
                }

                /* Rail beam: detect rising edge of FIRING flag */
                if (ie->weapon == QK_WEAPON_RAIL && firing_now && !firing_prev) {
                    f32 pr = fwd_pitch * (3.14159265f / 180.0f);
                    f32 yr = fwd_yaw * (3.14159265f / 180.0f);
                    f32 cp = cosf(pr), sp = sinf(pr);
                    f32 cy = cosf(yr), sy = sinf(yr);
                    f32 dx = cp * cy, dy = cp * sy, dz = sp;

                    f32 range = 8192.0f;
                    vec3_t start = {eye_x, eye_y, eye_z};
                    vec3_t end_pt = {eye_x + dx * range,
                                     eye_y + dy * range,
                                     eye_z + dz * range};

                    qk_trace_result_t tr = qk_physics_trace(phys_world,
                                                              start, end_pt,
                                                              zero_ext, zero_ext);

                    rail_beam_t *rb = &s_rail_beams[s_rail_beam_next % MAX_RAIL_BEAMS];
                    s_rail_beam_next++;
                    rb->active = true;
                    rb->birth_time = now;
                    if (is_local) {
                        /* Muzzle offset: 8 units right, 4 units below eye.
                           right = (sy, -cy, 0) in QUAKE yaw convention. */
                        rb->start[0] = camera.position[0] + sy * 8.0f;
                        rb->start[1] = camera.position[1] + (-cy) * 8.0f;
                        rb->start[2] = camera.position[2] - 4.0f;
                    } else {
                        rb->start[0] = eye_x;
                        rb->start[1] = eye_y;
                        rb->start[2] = eye_z;
                    }
                    rb->end[0] = tr.end_pos.x;
                    rb->end[1] = tr.end_pos.y;
                    rb->end[2] = tr.end_pos.z;
                    rb->color = is_local ? 0x00FF00FF : 0xFF0000FF;
                }

                /* LG beam: remote players only (local handled below) */
                if (ie->weapon == QK_WEAPON_LG && firing_now && !is_local) {
                    f32 pr = fwd_pitch * (3.14159265f / 180.0f);
                    f32 yr = fwd_yaw * (3.14159265f / 180.0f);
                    f32 cp = cosf(pr), sp = sinf(pr);
                    f32 cy = cosf(yr), sy = sinf(yr);
                    f32 dx = cp * cy, dy = cp * sy, dz = sp;

                    f32 range = 768.0f;
                    vec3_t start = {eye_x, eye_y, eye_z};
                    vec3_t end_pt = {eye_x + dx * range,
                                     eye_y + dy * range,
                                     eye_z + dz * range};

                    qk_trace_result_t tr = qk_physics_trace(phys_world,
                                                              start, end_pt,
                                                              zero_ext, zero_ext);

                    qk_renderer_draw_lg_beam(eye_x, eye_y, eye_z,
                                              tr.end_pos.x, tr.end_pos.y, tr.end_pos.z,
                                              (f32)now);
                }

                s_prev_flags[i] = cur_flags;
            }

            /* Draw active rail beams (persistent with decay) */
            for (u32 i = 0; i < MAX_RAIL_BEAMS; i++) {
                rail_beam_t *rb = &s_rail_beams[i];
                if (!rb->active) continue;

                f32 age = (f32)(now - rb->birth_time);
                if (age > RAIL_BEAM_LIFETIME) {
                    rb->active = false;
                    continue;
                }

                qk_renderer_draw_rail_beam(rb->start[0], rb->start[1], rb->start[2],
                                            rb->end[0], rb->end[1], rb->end[2],
                                            age, rb->color);
            }

            /* Local player LG beam: input-driven, per-frame, muzzle offset */
            if (cl_has_prediction &&
                cl_predicted_ps.weapon == QK_WEAPON_LG &&
                cl_predicted_ps.alive_state == QK_PSTATE_ALIVE &&
                cl_predicted_ps.pending_weapon == QK_WEAPON_NONE &&
                cl_predicted_ps.switch_time == 0 &&
                cl_predicted_ps.ammo[QK_WEAPON_LG] > 0 &&
                input_state.mouse_buttons[0] &&
                !input_state.console_active) {
                f32 pr = cam_pitch * (3.14159265f / 180.0f);
                f32 yr = cam_yaw * (3.14159265f / 180.0f);
                f32 cp = cosf(pr), sp = sinf(pr);
                f32 cy = cosf(yr), sy = sinf(yr);
                f32 dx = cp * cy, dy = cp * sy, dz = sp;

                f32 range = 768.0f;
                vec3_t trace_start = {camera.position[0],
                                      camera.position[1],
                                      camera.position[2]};
                vec3_t trace_end = {camera.position[0] + dx * range,
                                    camera.position[1] + dy * range,
                                    camera.position[2] + dz * range};

                qk_trace_result_t tr = qk_physics_trace(phys_world,
                    trace_start, trace_end, zero_ext, zero_ext);

                /* Muzzle offset: 8 units right, 4 units below eye.
                   right = (sy, -cy, 0) in QUAKE yaw convention. */
                f32 muzzle_x = camera.position[0] + sy * 8.0f;
                f32 muzzle_y = camera.position[1] + (-cy) * 8.0f;
                f32 muzzle_z = camera.position[2] - 4.0f;

                qk_renderer_draw_lg_beam(muzzle_x, muzzle_y, muzzle_z,
                                          tr.end_pos.x, tr.end_pos.y,
                                          tr.end_pos.z, (f32)now);
            }
        }

        /* ---- 9c. Diagnostic trace ---- */
        if (s_diag_file) {
            const qk_interp_diag_t *idiag = qk_net_client_get_interp_diag();

            /* Frame header */
            fprintf(s_diag_file,
                "FRAME %u now=%.6f dt=%.6f srv_tick=%u srv_acc=%.6f cl_acc=%.6f"
                " interp=[%s a=%u b=%u t=%.4f rt=%.2f cnt=%u]\n",
                s_diag_frame, now, (double)real_dt,
                qk_net_server_get_tick(),
                (double)server_accumulator, (double)cl_pred_accumulator,
                (idiag && idiag->valid) ? "OK" : "NONE",
                idiag ? idiag->snap_a_tick : 0,
                idiag ? idiag->snap_b_tick : 0,
                idiag ? (double)idiag->t : 0.0,
                idiag ? idiag->render_tick : 0.0,
                idiag ? idiag->interp_count : 0);

            /* Projectile entities: server f32 vs packed i16 vs interp f32 */
            for (u32 di = 0; di < qk_game_get_entity_count(); di++) {
                n_entity_state_t packed;
                qk_game_pack_entity((u8)di, &packed);
                if (packed.entity_type != 2) continue;

                f32 sx, sy, sz;
                if (!qk_game_get_entity_origin((u8)di, &sx, &sy, &sz)) continue;

                const qk_interp_entity_t *die = interp ? &interp->entities[di] : NULL;
                bool di_active = die && die->active;
                fprintf(s_diag_file,
                    "  PROJ[%u] srv=(%.2f,%.2f,%.2f) i16=(%d,%d,%d) "
                    "interp=(%s%.2f,%.2f,%.2f) vel=(%d,%d,%d)\n",
                    di, (double)sx, (double)sy, (double)sz,
                    packed.pos_x, packed.pos_y, packed.pos_z,
                    di_active ? "" : "INACTIVE ",
                    di_active ? (double)die->pos_x : 0.0,
                    di_active ? (double)die->pos_y : 0.0,
                    di_active ? (double)die->pos_z : 0.0,
                    packed.vel_x, packed.vel_y, packed.vel_z);
            }

            /* Local player weapon/beam state */
            {
                const qk_player_state_t *dps =
                    qk_game_get_player_state(local_client_id);
                const qk_interp_entity_t *dle =
                    interp ? &interp->entities[local_client_id] : NULL;
                if (dps) {
                    fprintf(s_diag_file,
                        "  LOCAL srv_weapon=%u srv_wtime=%u "
                        "interp_flags=0x%02x prev_flags=0x%02x "
                        "interp_weapon=%u\n",
                        dps->weapon, dps->weapon_time,
                        dle ? dle->flags : 0,
                        s_prev_flags[local_client_id],
                        dle ? dle->weapon : 0);
                }
            }

            s_diag_frame++;
            if (s_diag_frame % 128 == 0) fflush(s_diag_file);
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
    if (s_diag_file) { fclose(s_diag_file); s_diag_file = NULL; }
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
