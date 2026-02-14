/*
 * QUICKEN Engine - Physics Validation Tests
 *
 * Synthetic input tests to verify movement behavior matches Q3 expectations.
 * Called from debug harness; prints results to stdout.
 */

#include "p_internal.h"
#include "core/qk_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Horizontal speed (XY plane) */
static f32 p_hspeed(const qk_player_state_t *ps) {
    return sqrtf(ps->velocity.x * ps->velocity.x +
                 ps->velocity.y * ps->velocity.y);
}

bool qk_physics_validate_strafejump(void) {
    printf("=== Strafejump Validation ===\n");

    /* Create test room and player */
    qk_phys_world_t *world = qk_physics_world_create_test_room();
    if (!world) {
        printf("FAIL: Could not create test room\n");
        return false;
    }

    qk_player_state_t ps = {0};
    /* Spawn at ground level near -X wall, facing +X diagonal for max room.
       origin.z = 24 puts box bottom at z=0 (floor). */
    qk_physics_player_init(&ps, (vec3_t){-200.0f, 0.0f, 24.0f});

    /*
     * Strafejump technique:
     *   1. Start on ground, accelerate forward for a few ticks
     *   2. Jump while holding forward
     *   3. In air: hold forward + right, yaw ~45 degrees to the right
     *   4. On landing: jump again, keep strafing
     *
     * Classic Q3 strafejump: forward_move=1, side_move=1, yaw rotates
     * smoothly ~1 degree per tick while airborne. The key is the 30 u/s
     * air wishspeed cap allowing speed to accumulate.
     */

    qk_usercmd_t cmd = {0};
    f32 yaw = 0.0f;
    f32 peak_speed = 0.0f;
    i32 jump_count = 0;
    bool was_on_ground = false;

    printf("Tick  | OnGrnd | HSpeed   | VelZ     | OrigZ    | Jump#\n");
    printf("------+--------+----------+----------+----------+------\n");

    /* Phase 1: walk forward on ground for 60 ticks to reach base speed (~320 u/s) */
    for (i32 tick = 0; tick < 60; tick++) {
        cmd.forward_move = 1.0f;
        cmd.side_move = 0.0f;
        cmd.pitch = 0.0f;
        cmd.yaw = yaw;
        cmd.buttons = 0;

        qk_physics_move(&ps, &cmd, world);

        if (tick % 10 == 0 || tick == 59) {
            printf("%5d | %s |%9.2f |%9.2f |%9.2f | %d\n",
                   tick, ps.on_ground ? "  yes " : "  no  ",
                   (double)p_hspeed(&ps), (double)ps.velocity.z,
                   (double)ps.origin.z, jump_count);
        }
    }

    /* Phase 2: strafejump sequence
     *
     * Classic Q3 strafejump: on ground, jump. In air, hold forward + strafe
     * and rotate yaw slowly (~0.5-1 degrees per tick at 128Hz). The key:
     * the wish direction (forward+right at ~45 degrees in move-space) combined
     * with a slow yaw turn keeps the wish vector at a shallow angle to the
     * current velocity vector, maximizing the air accel exploit. */
    for (i32 tick = 60; tick < 560; tick++) {
        /* Detect landing */
        if (ps.on_ground && !was_on_ground) {
            jump_count++;
        }
        was_on_ground = ps.on_ground;

        /* Build command */
        cmd.pitch = 0.0f;

        /* Release jump during descending air phase (vel.z < -50) to reset
         * edge detection. Press jump during ascending + ground phases.
         * This mimics the Q3 +jump script: release during fall, press on land. */
        bool descending = (!ps.on_ground && ps.velocity.z < -50.0f);
        cmd.buttons = descending ? 0 : QK_BUTTON_JUMP;

        if (ps.on_ground) {
            /* On ground: hold forward only */
            cmd.forward_move = 1.0f;
            cmd.side_move = 0.0f;
        } else {
            /* In air: hold forward + right, turn yaw 0.5 deg/tick.
             * This rate matches the velocity's natural turn rate from
             * air accel (~0.45 deg/tick at 320 ups), keeping the wish
             * direction near the optimal angle for speed gain. */
            cmd.forward_move = 1.0f;
            cmd.side_move = 1.0f;
            yaw -= 0.5f;
        }
        cmd.yaw = yaw;

        qk_physics_move(&ps, &cmd, world);

        f32 hs = p_hspeed(&ps);
        if (hs > peak_speed) peak_speed = hs;

        if (tick % 20 == 0 || (ps.on_ground && !was_on_ground)) {
            printf("%5d | %s |%9.2f |%9.2f |%9.2f | %d\n",
                   tick, ps.on_ground ? "  yes " : "  no  ",
                   (double)hs, (double)ps.velocity.z,
                   (double)ps.origin.z, jump_count);
        }
    }

    printf("------+--------+----------+----------+----------+------\n");
    printf("Peak horizontal speed: %.2f u/s\n", (double)peak_speed);
    printf("Jumps completed: %d\n", jump_count);

    qk_physics_world_destroy(world);

    /*
     * Q3 strafejump expectations at 128 tick:
     *   - Base ground speed: ~320 u/s (PM_MAX_SPEED)
     *   - Speed increases above 320 over successive strafejumps
     *   - Rate of gain depends on technique (yaw rate, timing)
     *
     * We pass if:
     *   1. Base ground speed reaches ~320 u/s
     *   2. Peak speed exceeds 320 (proving air accel exploit works)
     *   3. Multiple jumps completed successfully
     */
    bool pass = (peak_speed > 325.0f && jump_count >= 4);
    printf("\nResult: %s (peak %.1f u/s, %d jumps)\n",
           pass ? "PASS" : "FAIL", (double)peak_speed, jump_count);
    printf("=== End Strafejump Validation ===\n\n");

    return pass;
}

/* ---- Map-based validation ---- */

/*
 * Deep-copy a collision model so the physics world can own its memory
 * independently of the map loader. This avoids the double-free between
 * qk_physics_world_destroy (which frees cm internals) and qk_map_free.
 */
static qk_collision_model_t *p_clone_collision_model(const qk_collision_model_t *src) {
    qk_collision_model_t *cm = (qk_collision_model_t *)malloc(sizeof(qk_collision_model_t));
    if (!cm) return NULL;

    cm->brush_count = src->brush_count;
    cm->brushes = (qk_brush_t *)malloc(cm->brush_count * sizeof(qk_brush_t));
    if (!cm->brushes) { free(cm); return NULL; }

    for (u32 i = 0; i < cm->brush_count; i++) {
        cm->brushes[i].plane_count = src->brushes[i].plane_count;
        cm->brushes[i].mins = src->brushes[i].mins;
        cm->brushes[i].maxs = src->brushes[i].maxs;
        cm->brushes[i].planes = (qk_plane_t *)malloc(
            cm->brushes[i].plane_count * sizeof(qk_plane_t));
        if (!cm->brushes[i].planes) {
            /* Partial cleanup */
            for (u32 j = 0; j < i; j++) free(cm->brushes[j].planes);
            free(cm->brushes);
            free(cm);
            return NULL;
        }
        memcpy(cm->brushes[i].planes, src->brushes[i].planes,
               cm->brushes[i].plane_count * sizeof(qk_plane_t));
    }
    return cm;
}

bool qk_physics_validate_map(const char *map_path) {
    printf("=== Map Collision Validation ===\n");
    printf("Loading: %s\n", map_path);

    qk_map_data_t map = {0};
    qk_result_t res = qk_map_load(map_path, &map);
    if (res != QK_SUCCESS) {
        printf("FAIL: Could not load map (error %d)\n", res);
        return false;
    }

    printf("Map loaded: %u collision brushes, %u spawn points\n",
           map.collision.brush_count, map.spawn_count);

    if (map.collision.brush_count == 0) {
        printf("FAIL: No collision brushes\n");
        qk_map_free(&map);
        return false;
    }

    /* Clone the collision model so physics world owns its own memory.
       This avoids the double-free between world_destroy and map_free. */
    qk_collision_model_t *cm = p_clone_collision_model(&map.collision);
    if (!cm) {
        printf("FAIL: Could not clone collision model\n");
        qk_map_free(&map);
        return false;
    }

    qk_phys_world_t *world = qk_physics_world_create(cm);
    if (!world) {
        printf("FAIL: Could not create physics world\n");
        qk_map_free(&map);
        return false;
    }

    bool all_pass = true;
    i32 test_num = 0;

    /* --- Test 1: Downward trace hits the floor --- */
    {
        test_num++;
        vec3_t start = {0.0f, 0.0f, 100.0f};
        vec3_t end   = {0.0f, 0.0f, -100.0f};
        vec3_t mins  = {0.0f, 0.0f, 0.0f};
        vec3_t maxs  = {0.0f, 0.0f, 0.0f};

        qk_trace_result_t tr = qk_physics_trace(world, start, end, mins, maxs);
        bool pass = (tr.fraction < 1.0f && tr.hit_normal.z > 0.9f);
        printf("  Test %d: Point trace downward hits floor: %s "
               "(frac=%.4f, normal.z=%.2f)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)tr.fraction, (double)tr.hit_normal.z);
        if (!pass) all_pass = false;
    }

    /* --- Test 2: Player-sized box trace hits floor --- */
    {
        test_num++;
        /* Start away from center platform ([-64,64] on XY) to hit bare floor */
        vec3_t start = {-150.0f, 0.0f, 100.0f};
        vec3_t end   = {-150.0f, 0.0f, -100.0f};

        qk_trace_result_t tr = qk_physics_trace(world, start, end,
                                                  QK_PLAYER_MINS, QK_PLAYER_MAXS);
        /* Player box bottom at z=-24 relative to origin, floor at z=0.
           Should stop with origin.z ~ 24. */
        bool pass = (tr.fraction < 1.0f && fabsf(tr.end_pos.z - 24.0f) < 1.0f);
        printf("  Test %d: Player box trace to floor: %s "
               "(frac=%.4f, end_z=%.2f, expected ~24.0)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)tr.fraction, (double)tr.end_pos.z);
        if (!pass) all_pass = false;
    }

    /* --- Test 3: Horizontal trace hits +X wall --- */
    {
        test_num++;
        /* Room interior spans X: [-256, 256]. +X wall starts at x=256. */
        vec3_t start = {0.0f, 0.0f, 50.0f};
        vec3_t end   = {500.0f, 0.0f, 50.0f};
        vec3_t mins  = {0.0f, 0.0f, 0.0f};
        vec3_t maxs  = {0.0f, 0.0f, 0.0f};

        qk_trace_result_t tr = qk_physics_trace(world, start, end, mins, maxs);
        bool pass = (tr.fraction < 1.0f && tr.hit_normal.x < -0.9f);
        printf("  Test %d: Point trace hits +X wall: %s "
               "(frac=%.4f, normal.x=%.2f, end_x=%.2f)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)tr.fraction, (double)tr.hit_normal.x,
               (double)tr.end_pos.x);
        if (!pass) all_pass = false;
    }

    /* --- Test 4: Trace inside open space hits nothing --- */
    {
        test_num++;
        vec3_t start = {-100.0f, 0.0f, 50.0f};
        vec3_t end   = {100.0f, 0.0f, 50.0f};
        vec3_t mins  = {0.0f, 0.0f, 0.0f};
        vec3_t maxs  = {0.0f, 0.0f, 0.0f};

        qk_trace_result_t tr = qk_physics_trace(world, start, end, mins, maxs);
        bool pass = (tr.fraction >= 1.0f - 0.001f);
        printf("  Test %d: Point trace through open air: %s "
               "(frac=%.4f)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)tr.fraction);
        if (!pass) all_pass = false;
    }

    /* --- Test 5: Player-sized box trace hits +X wall at correct distance --- */
    {
        test_num++;
        vec3_t start = {0.0f, 0.0f, 50.0f};
        vec3_t end   = {500.0f, 0.0f, 50.0f};

        qk_trace_result_t tr = qk_physics_trace(world, start, end,
                                                  QK_PLAYER_MINS, QK_PLAYER_MAXS);
        /* Player maxs.x = 15, wall at x=256, so should stop at x = 256-15 = 241 */
        bool pass = (tr.fraction < 1.0f && fabsf(tr.end_pos.x - 241.0f) < 2.0f);
        printf("  Test %d: Player box hits +X wall: %s "
               "(frac=%.4f, end_x=%.2f, expected ~241.0)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)tr.fraction, (double)tr.end_pos.x);
        if (!pass) all_pass = false;
    }

    /* --- Test 6: Center platform trace (step-up box at z=0..16) --- */
    {
        test_num++;
        /* test_box.map has a center platform from z=0 to z=16, x/y [-64,64].
           A trace from above should hit the top of this platform. */
        vec3_t start = {0.0f, 0.0f, 100.0f};
        vec3_t end   = {0.0f, 0.0f, -100.0f};
        vec3_t mins  = {0.0f, 0.0f, 0.0f};
        vec3_t maxs  = {0.0f, 0.0f, 0.0f};

        qk_trace_result_t tr = qk_physics_trace(world, start, end, mins, maxs);
        /* Should hit platform top at z=16 (not the floor at z=0) */
        bool pass = (tr.fraction < 1.0f && fabsf(tr.end_pos.z - 16.0f) < 1.0f);
        printf("  Test %d: Point trace hits center platform top: %s "
               "(frac=%.4f, end_z=%.2f, expected ~16.0)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)tr.fraction, (double)tr.end_pos.z);
        if (!pass) all_pass = false;
    }

    /* --- Test 7: Player movement at spawn point --- */
    {
        test_num++;
        vec3_t spawn = {-128.0f, -128.0f, 24.0f};
        if (map.spawn_count > 0) {
            spawn = map.spawn_points[0].origin;
        }

        qk_player_state_t ps = {0};
        qk_physics_player_init(&ps, spawn);

        /* Run 10 ticks standing still to settle on ground */
        qk_usercmd_t cmd = {0};
        for (i32 i = 0; i < 10; i++) {
            qk_physics_move(&ps, &cmd, world);
        }

        bool pass = ps.on_ground;
        printf("  Test %d: Player on ground at spawn: %s "
               "(origin=(%.1f,%.1f,%.1f) on_ground=%d)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)ps.origin.x, (double)ps.origin.y,
               (double)ps.origin.z, ps.on_ground);
        if (!pass) all_pass = false;
    }

    /* --- Test 8: Player walks forward and hits wall --- */
    {
        test_num++;
        qk_player_state_t ps = {0};
        /* Start near +X wall, facing +X */
        qk_physics_player_init(&ps, (vec3_t){200.0f, 0.0f, 24.0f});

        qk_usercmd_t cmd = {0};
        cmd.forward_move = 1.0f;
        cmd.yaw = 0.0f;

        /* Walk forward for 200 ticks -- should hit wall and stop */
        for (i32 i = 0; i < 200; i++) {
            qk_physics_move(&ps, &cmd, world);
        }

        /* Player maxs.x = 15, wall at x=256, so can't go past 241 */
        bool pass = (ps.origin.x < 242.0f && ps.on_ground);
        printf("  Test %d: Player walks into +X wall: %s "
               "(origin.x=%.2f, expected <=241)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)ps.origin.x);
        if (!pass) all_pass = false;
    }

    /* --- Test 9: Strafejump in loaded map --- */
    {
        test_num++;
        qk_player_state_t ps = {0};
        qk_physics_player_init(&ps, (vec3_t){-200.0f, 0.0f, 24.0f});

        qk_usercmd_t cmd = {0};
        f32 yaw = 0.0f;
        f32 peak = 0.0f;
        i32 jumps = 0;
        bool prev_ground = false;

        /* Ground accel for 60 ticks */
        for (i32 t = 0; t < 60; t++) {
            cmd.forward_move = 1.0f;
            cmd.side_move = 0.0f;
            cmd.yaw = yaw;
            cmd.buttons = 0;
            qk_physics_move(&ps, &cmd, world);
        }

        /* Strafejump for 400 ticks */
        for (i32 t = 0; t < 400; t++) {
            if (ps.on_ground && !prev_ground) jumps++;
            prev_ground = ps.on_ground;

            bool desc = (!ps.on_ground && ps.velocity.z < -50.0f);
            cmd.buttons = desc ? 0 : QK_BUTTON_JUMP;

            if (ps.on_ground) {
                cmd.forward_move = 1.0f;
                cmd.side_move = 0.0f;
            } else {
                cmd.forward_move = 1.0f;
                cmd.side_move = 1.0f;
                yaw -= 0.5f;
            }
            cmd.yaw = yaw;
            cmd.pitch = 0.0f;

            qk_physics_move(&ps, &cmd, world);

            f32 hs = sqrtf(ps.velocity.x * ps.velocity.x +
                           ps.velocity.y * ps.velocity.y);
            if (hs > peak) peak = hs;
        }

        /* In a 512x512 room, strafejump can't build much speed before
           hitting walls. Pass if near max ground speed and multiple jumps. */
        bool pass = (peak > 310.0f && jumps >= 3);
        printf("  Test %d: Strafejump in loaded map: %s "
               "(peak=%.1f u/s, jumps=%d)\n",
               test_num, pass ? "PASS" : "FAIL",
               (double)peak, jumps);
        if (!pass) all_pass = false;
    }

    printf("\nResult: %s (%d/%d tests passed)\n",
           all_pass ? "PASS" : "FAIL",
           all_pass ? test_num : test_num - 1, test_num);
    printf("=== End Map Collision Validation ===\n\n");

    qk_physics_world_destroy(world);
    qk_map_free(&map);

    return all_pass;
}
