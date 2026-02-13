/*
 * QUICKEN Engine - Netcode Loopback Integration Test
 *
 * Verifies the full loopback path:
 *   1. Server init (port=0, loopback only)
 *   2. Client init + connect_local
 *   3. Client sends input -> server receives via qk_net_server_get_input
 *   4. Server sets entities -> client receives snapshots
 *   5. Client interpolates -> entities visible in interp state
 *   6. Clock sync converges
 */

#include "quicken.h"
#include "qk_types.h"
#include "netcode/qk_netcode.h"
#include "netcode/n_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_tests_passed = 0;
static int s_tests_failed = 0;

#define TEST_CHECK(expr, msg) \
    do { \
        if (expr) { \
            s_tests_passed++; \
            printf("  [PASS] %s\n", msg); \
        } else { \
            s_tests_failed++; \
            printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
        } \
    } while (0)

/* ---------- Test 1: Server + Client init and connect ---------- */

static void test_connect_local(void) {
    printf("\n=== Test: Loopback Connect ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.server_port = 0;    /* loopback only, no UDP bind */
    srv_cfg.max_clients = 4;
    srv_cfg.tick_rate = 128.0;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init succeeds");

    qk_net_client_config_t cl_cfg = {0};
    cl_cfg.interp_delay = 0.020;

    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init succeeds");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Client connect_local succeeds");

    TEST_CHECK(qk_net_client_get_state() == QK_CONN_CONNECTED,
               "Client is immediately CONNECTED after loopback connect");

    TEST_CHECK(qk_net_server_client_count() == 1,
               "Server has 1 connected client");

    u8 cid = qk_net_client_get_id();
    TEST_CHECK(cid == 0, "Client got slot 0");

    /* Clean up */
    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Test 2: Input round-trip ---------- */

static void test_input_roundtrip(void) {
    printf("\n=== Test: Input Round-Trip ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.max_clients = 4;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init");

    qk_net_client_config_t cl_cfg = {0};
    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Connect local");

    u8 cid = qk_net_client_get_id();

    /* Build a usercmd with known values */
    qk_usercmd_t cmd = {0};
    cmd.forward_move = 1.0f;
    cmd.side_move = -0.5f;
    cmd.yaw = 90.0f;
    cmd.pitch = 15.0f;
    cmd.buttons = QK_BUTTON_JUMP;
    cmd.weapon_select = 0;

    /* Client sends input */
    qk_net_client_send_input(&cmd);

    /* Server ticks (receives loopback packets, broadcasts snapshots) */
    qk_net_server_tick();

    /* Server should now have the input */
    qk_usercmd_t out_cmd = {0};
    bool got_input = qk_net_server_get_input(cid, &out_cmd);
    TEST_CHECK(got_input, "Server got input from client");

    if (got_input) {
        /* Check values (expect some quantization loss: i8 range -127..127) */
        f32 fwd_err = out_cmd.forward_move - 1.0f;
        if (fwd_err < 0) fwd_err = -fwd_err;
        TEST_CHECK(fwd_err < 0.02f, "forward_move ~= 1.0");

        f32 side_err = out_cmd.side_move - (-0.5f);
        if (side_err < 0) side_err = -side_err;
        TEST_CHECK(side_err < 0.02f, "side_move ~= -0.5");

        /* Angle quantization: u16 gives ~0.0055 degree precision */
        f32 yaw_err = out_cmd.yaw - 90.0f;
        if (yaw_err < 0) yaw_err = -yaw_err;
        TEST_CHECK(yaw_err < 0.1f, "yaw ~= 90.0");

        f32 pitch_err = out_cmd.pitch - 15.0f;
        if (pitch_err < 0) pitch_err = -pitch_err;
        TEST_CHECK(pitch_err < 0.1f, "pitch ~= 15.0");

        TEST_CHECK(out_cmd.buttons == QK_BUTTON_JUMP, "buttons == JUMP");
    }

    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Test 3: Snapshot flow ---------- */

static void test_snapshot_flow(void) {
    printf("\n=== Test: Snapshot Flow ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.max_clients = 4;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init");

    qk_net_client_config_t cl_cfg = {0};
    cl_cfg.interp_delay = 0.0; /* zero delay for testing */
    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Connect local");

    /* Server sets an entity */
    n_entity_state_t ent = {0};
    ent.pos_x = (i16)(100.0f / 0.125f);  /* 100.0 in 13.3 fixed-point = 800 */
    ent.pos_y = (i16)(200.0f / 0.125f);
    ent.pos_z = (i16)(50.0f / 0.125f);
    ent.vel_x = 300;
    ent.vel_y = 0;
    ent.vel_z = 0;
    ent.yaw = (u16)(180.0f * (65536.0f / 360.0f));
    ent.pitch = 0;
    ent.entity_type = 1;
    ent.health = 200;
    ent.armor = 150;
    ent.weapon = QK_WEAPON_ROCKET;
    ent.ammo = 25;

    qk_net_server_set_entity(0, &ent);

    /* Run several server ticks to build up snapshot history.
     * Each server tick broadcasts a snapshot to the client. */
    for (int i = 0; i < 4; i++) {
        qk_net_server_tick();
    }

    /* Client ticks to receive the snapshots */
    for (int i = 0; i < 4; i++) {
        qk_net_client_tick();
    }

    /* Run interpolation at "current" server time.
     * For loopback, render_time = server_time - interp_delay.
     * We set interp_delay to minimum so render_time ~ server_time. */
    u32 server_tick = qk_net_server_get_tick();
    f64 render_time = (f64)server_tick / 128.0;
    qk_net_client_interpolate(render_time);

    const qk_interp_state_t *interp = qk_net_client_get_interp_state();
    TEST_CHECK(interp != NULL, "Interp state is not NULL");

    if (interp) {
        const qk_interp_entity_t *ie = &interp->entities[0];
        TEST_CHECK(ie->active, "Entity 0 is active in interp state");

        if (ie->active) {
            f32 pos_err_x = ie->pos_x - 100.0f;
            if (pos_err_x < 0) pos_err_x = -pos_err_x;
            TEST_CHECK(pos_err_x < 0.2f, "pos_x ~= 100.0");

            f32 pos_err_y = ie->pos_y - 200.0f;
            if (pos_err_y < 0) pos_err_y = -pos_err_y;
            TEST_CHECK(pos_err_y < 0.2f, "pos_y ~= 200.0");

            f32 pos_err_z = ie->pos_z - 50.0f;
            if (pos_err_z < 0) pos_err_z = -pos_err_z;
            TEST_CHECK(pos_err_z < 0.2f, "pos_z ~= 50.0");

            TEST_CHECK(ie->health == 200, "health == 200");
            TEST_CHECK(ie->armor == 150, "armor == 150");
            TEST_CHECK(ie->weapon == QK_WEAPON_ROCKET, "weapon == ROCKET");
            TEST_CHECK(ie->ammo == 25, "ammo == 25");
            TEST_CHECK(ie->entity_type == 1, "entity_type == 1");

            printf("    [DEBUG] interp pos: (%.2f, %.2f, %.2f)\n",
                   ie->pos_x, ie->pos_y, ie->pos_z);
        }
    }

    /* Remove entity and verify it goes away */
    qk_net_server_remove_entity(0);
    for (int i = 0; i < 4; i++) {
        qk_net_server_tick();
    }
    for (int i = 0; i < 4; i++) {
        qk_net_client_tick();
    }
    render_time = (f64)qk_net_server_get_tick() / 128.0;
    qk_net_client_interpolate(render_time);

    interp = qk_net_client_get_interp_state();
    if (interp) {
        TEST_CHECK(!interp->entities[0].active,
                   "Entity 0 is inactive after removal");
    }

    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Test 4: Multiple entities + delta compression ---------- */

static void test_delta_compression(void) {
    printf("\n=== Test: Delta Compression ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.max_clients = 4;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init");

    qk_net_client_config_t cl_cfg = {0};
    cl_cfg.interp_delay = 0.0;
    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Connect local");

    /* Set up 3 entities */
    for (u8 id = 0; id < 3; id++) {
        n_entity_state_t ent = {0};
        ent.pos_x = (i16)((f32)(id * 100) / 0.125f);
        ent.pos_y = (i16)((f32)(id * 50) / 0.125f);
        ent.entity_type = id + 1;
        ent.health = 100 + id * 50;
        qk_net_server_set_entity(id, &ent);
    }

    /* Tick to get first full snapshot through */
    for (int i = 0; i < 4; i++) {
        qk_net_server_tick();
        qk_net_client_tick();
    }

    /* Now update only entity 1's position (delta should be small) */
    n_entity_state_t ent1_update = {0};
    ent1_update.pos_x = (i16)(150.0f / 0.125f);
    ent1_update.pos_y = (i16)(75.0f / 0.125f);
    ent1_update.entity_type = 2;
    ent1_update.health = 150;
    qk_net_server_set_entity(1, &ent1_update);

    /* More ticks for delta snapshot */
    for (int i = 0; i < 4; i++) {
        qk_net_server_tick();
        qk_net_client_tick();
    }

    f64 render_time = (f64)qk_net_server_get_tick() / 128.0;
    qk_net_client_interpolate(render_time);

    const qk_interp_state_t *interp = qk_net_client_get_interp_state();
    TEST_CHECK(interp != NULL, "Interp state valid");

    if (interp) {
        /* Entity 0 unchanged */
        TEST_CHECK(interp->entities[0].active, "Entity 0 still active");
        TEST_CHECK(interp->entities[2].active, "Entity 2 still active");

        /* Entity 1 updated */
        TEST_CHECK(interp->entities[1].active, "Entity 1 active");
        if (interp->entities[1].active) {
            f32 err = interp->entities[1].pos_x - 150.0f;
            if (err < 0) err = -err;
            TEST_CHECK(err < 0.2f, "Entity 1 pos_x updated to ~150.0");
        }
    }

    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Test 5: Disconnect + reconnect ---------- */

static void test_disconnect_reconnect(void) {
    printf("\n=== Test: Disconnect + Reconnect ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.max_clients = 4;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init");

    qk_net_client_config_t cl_cfg = {0};
    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Connect local");
    TEST_CHECK(qk_net_server_client_count() == 1, "1 client connected");

    /* Disconnect */
    qk_net_client_disconnect();
    TEST_CHECK(qk_net_client_get_state() == QK_CONN_DISCONNECTED,
               "Client is disconnected");

    /* Client shutdown + reinit for reconnect (need fresh client state) */
    qk_net_client_shutdown();

    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client reinit");

    /* Server tick to process the disconnect */
    qk_net_server_tick();

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Reconnect succeeds");
    TEST_CHECK(qk_net_client_get_state() == QK_CONN_CONNECTED,
               "Client is CONNECTED after reconnect");

    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Test 6: Full game loop simulation ---------- */

static void test_full_game_loop(void) {
    printf("\n=== Test: Full Game Loop Simulation (50 ticks) ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.max_clients = 4;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init");

    qk_net_client_config_t cl_cfg = {0};
    cl_cfg.interp_delay = 0.020;
    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Connect local");

    u8 cid = qk_net_client_get_id();
    bool ever_got_input = false;
    bool entity_ever_active = false;

    /* Simulate a player entity moving */
    f32 player_x = 0.0f;

    for (int tick = 0; tick < 50; tick++) {
        /* 1. Client sends input */
        qk_usercmd_t cmd = {0};
        cmd.forward_move = 1.0f;
        cmd.yaw = (f32)(tick * 2);
        cmd.buttons = (tick % 10 == 0) ? QK_BUTTON_JUMP : 0;
        qk_net_client_send_input(&cmd);

        /* 2. Server tick: read inputs, simulate, set entities, broadcast */
        qk_usercmd_t srv_cmd = {0};
        if (qk_net_server_get_input(cid, &srv_cmd)) {
            ever_got_input = true;
            /* Simple "physics": move forward */
            player_x += srv_cmd.forward_move * 320.0f * (1.0f / 128.0f);
        }

        /* Update entity state */
        n_entity_state_t ent = {0};
        ent.pos_x = (i16)(player_x / 0.125f);
        ent.pos_y = 0;
        ent.pos_z = 0;
        ent.entity_type = 1;
        ent.health = 200;
        ent.armor = 200;
        ent.weapon = QK_WEAPON_ROCKET;
        ent.ammo = 25;
        qk_net_server_set_entity(0, &ent);

        qk_net_server_tick();

        /* 3. Client tick: receive snapshots */
        qk_net_client_tick();

        /* 4. Client interpolate */
        u32 stk = qk_net_server_get_tick();
        f64 render_time = (f64)stk / 128.0 - 0.020;
        qk_net_client_interpolate(render_time);

        const qk_interp_state_t *interp = qk_net_client_get_interp_state();
        if (interp && interp->entities[0].active) {
            entity_ever_active = true;
        }
    }

    TEST_CHECK(ever_got_input, "Server received input from client during loop");
    TEST_CHECK(entity_ever_active, "Client saw entity active during loop");

    /* Check final interp state */
    const qk_interp_state_t *final_interp = qk_net_client_get_interp_state();
    if (final_interp && final_interp->entities[0].active) {
        printf("    [DEBUG] Final entity pos_x: %.2f  (expected ~%.2f)\n",
               final_interp->entities[0].pos_x, player_x);
        f32 err = final_interp->entities[0].pos_x - player_x;
        if (err < 0) err = -err;
        /* Allow for interpolation delay */
        TEST_CHECK(err < 20.0f, "Final entity position within tolerance");
    }

    /* Check RTT */
    i32 rtt = qk_net_client_get_rtt();
    printf("    [DEBUG] RTT: %d ms\n", rtt);
    /* Loopback RTT should be near 0 */
    TEST_CHECK(rtt < 5, "Loopback RTT < 5ms");

    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Test 7: Early-frame interpolation (no crash with empty buffer) ---------- */

static void test_early_frame_interpolation(void) {
    printf("\n=== Test: Early-Frame Interpolation (20ms delay) ===\n");

    qk_net_server_config_t srv_cfg = {0};
    srv_cfg.max_clients = 4;

    qk_result_t res = qk_net_server_init(&srv_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Server init");

    qk_net_client_config_t cl_cfg = {0};
    cl_cfg.interp_delay = 0.020; /* real interp delay */
    res = qk_net_client_init(&cl_cfg);
    TEST_CHECK(res == QK_SUCCESS, "Client init");

    res = qk_net_client_connect_local();
    TEST_CHECK(res == QK_SUCCESS, "Connect local");

    /* Interpolate BEFORE any snapshots exist.
     * This simulates the very first render frame. Should not crash. */
    f64 render_time = 0.0;
    qk_net_client_interpolate(render_time);

    const qk_interp_state_t *interp = qk_net_client_get_interp_state();
    TEST_CHECK(interp != NULL, "Interp state not NULL even with no snapshots");

    /* All entities should be inactive */
    bool any_active = false;
    if (interp) {
        for (u32 i = 0; i < 256; i++) {
            if (interp->entities[i].active) {
                any_active = true;
                break;
            }
        }
    }
    TEST_CHECK(!any_active, "No entities active before any snapshots received");

    /* Now set an entity and tick once */
    n_entity_state_t ent = {0};
    ent.pos_x = (i16)(10.0f / 0.125f);
    ent.entity_type = 1;
    ent.health = 100;
    qk_net_server_set_entity(0, &ent);
    qk_net_server_tick();
    qk_net_client_tick();

    /* Interpolate at tick 0 (way before the snapshot tick) -- should use
     * single-snapshot fallback, no crash */
    qk_net_client_interpolate(0.0);
    interp = qk_net_client_get_interp_state();
    if (interp) {
        /* With only 1 snapshot, find_interp_pair fails but fallback to
         * latest snapshot kicks in */
        TEST_CHECK(interp->entities[0].active,
                   "Entity active via single-snapshot fallback");
    }

    /* After a few more ticks, interpolation should work normally */
    for (int i = 0; i < 5; i++) {
        qk_net_server_tick();
        qk_net_client_tick();
    }

    u32 stk = qk_net_server_get_tick();
    render_time = (f64)stk / 128.0 - 0.020;
    qk_net_client_interpolate(render_time);
    interp = qk_net_client_get_interp_state();
    if (interp) {
        TEST_CHECK(interp->entities[0].active,
                   "Entity active after buffer fills up");
    }

    qk_net_client_shutdown();
    qk_net_server_shutdown();
}

/* ---------- Main ---------- */

int main(int argc, char **argv) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);

    printf("QUICKEN Netcode Loopback Tests\n");
    printf("==============================\n");

    test_connect_local();
    test_input_roundtrip();
    test_snapshot_flow();
    test_delta_compression();
    test_disconnect_reconnect();
    test_full_game_loop();
    test_early_frame_interpolation();

    printf("\n==============================\n");
    printf("Results: %d passed, %d failed\n", s_tests_passed, s_tests_failed);

    return s_tests_failed > 0 ? 1 : 0;
}
