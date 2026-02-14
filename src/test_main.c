/*
 * QUICKEN Engine - Automated Test Harness
 *
 * Headless binary that runs scripted gameplay scenarios, asserts outcomes,
 * and outputs machine-parseable results. AI agents run this after every
 * change like a unit test suite.
 *
 * Usage:
 *   quicken-test --all         Run all tests
 *   quicken-test --test NAME   Run a single test by name
 *   quicken-test --list        List available tests
 *
 * Output:
 *   Human-readable: [PASS]/[FAIL] lines
 *   Machine-parseable: CSV: prefixed lines (test_name,pass|fail,detail)
 *   Exit code: 0 = all pass, 1 = any fail
 */

#include "quicken.h"
#include "qk_types.h"
#include "qk_math.h"
#include "physics/qk_physics.h"
#include "gameplay/qk_gameplay.h"
#include "g_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Test Framework ---- */

static int s_tests_passed = 0;
static int s_tests_failed = 0;
static const char *s_current_test = "";

#define TEST_CHECK(expr, msg) \
    do { \
        if (expr) { \
            s_tests_passed++; \
            printf("  [PASS] %s\n", msg); \
            printf("CSV:%s,pass,%s\n", s_current_test, msg); \
        } else { \
            s_tests_failed++; \
            printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            printf("CSV:%s,fail,%s\n", s_current_test, msg); \
        } \
    } while (0)

/* ---- Helper: set up a player for testing ---- */

static void setup_player(u8 id, const char *name, qk_team_t team,
                          vec3_t spawn, qk_weapon_id_t weapon) {
    qk_game_player_connect(id, name, team);
    qk_player_state_t *ps = qk_game_get_player_state_mut(id);
    if (!ps) return;

    ps->alive_state = QK_PSTATE_ALIVE;
    ps->health = QK_CA_SPAWN_HEALTH;
    ps->armor = QK_CA_SPAWN_ARMOR;
    ps->weapon = weapon;
    ps->ammo[QK_WEAPON_ROCKET] = 50;
    ps->ammo[QK_WEAPON_RAIL] = 25;
    ps->ammo[QK_WEAPON_LG] = 150;
    qk_physics_player_init(ps, spawn);
}

/* ---- Test 1: forward_move ---- */

static void test_forward_move(void) {
    printf("\n=== Test: forward_move ===\n");
    s_current_test = "forward_move";

    qk_phys_world_t *world = qk_physics_world_create_test_room();
    qk_player_state_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.mins = QK_PLAYER_MINS;
    ps.maxs = QK_PLAYER_MAXS;
    ps.max_speed = QK_PM_MAX_SPEED;
    ps.gravity = QK_PM_GRAVITY;
    ps.alive_state = QK_PSTATE_ALIVE;
    qk_physics_player_init(&ps, (vec3_t){0, 0, 24});

    qk_usercmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.forward_move = 1.0f;
    cmd.yaw = 0.0f;

    for (int i = 0; i < 128; i++) {
        qk_physics_move(&ps, &cmd, world);
    }

    f32 speed = sqrtf(ps.velocity.x * ps.velocity.x +
                      ps.velocity.y * ps.velocity.y);

    printf("  [INFO] speed=%.1f pos=(%.1f, %.1f, %.1f)\n",
           (double)speed, (double)ps.origin.x, (double)ps.origin.y,
           (double)ps.origin.z);

    TEST_CHECK(speed > 250.0f, "Speed > 250 ups after 128 ticks of forward move");

    f32 dist = sqrtf(ps.origin.x * ps.origin.x + ps.origin.y * ps.origin.y);
    TEST_CHECK(dist > 100.0f, "Position advanced > 100 units");

    qk_physics_world_destroy(world);
}

/* ---- Test 2: strafejump ---- */

static void test_strafejump(void) {
    printf("\n=== Test: strafejump ===\n");
    s_current_test = "strafejump";

    qk_phys_world_t *world = qk_physics_world_create_test_room();
    qk_player_state_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.mins = QK_PLAYER_MINS;
    ps.maxs = QK_PLAYER_MAXS;
    ps.max_speed = QK_PM_MAX_SPEED;
    ps.gravity = QK_PM_GRAVITY;
    ps.alive_state = QK_PSTATE_ALIVE;
    qk_physics_player_init(&ps, (vec3_t){0, 0, 24});

    f32 peak_speed = 0;
    f32 yaw = 0;
    int strafe_dir = 1;
    bool prev_on_ground = true;

    /* Continuous strafejump: jump on landing, strafe+turn while airborne */
    for (int t = 0; t < 600; t++) {
        qk_usercmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.forward_move = 1.0f;
        cmd.side_move = (f32)strafe_dir;

        /* Jump when grounded and jump_held cleared; alternate strafe on re-jump */
        if (ps.on_ground && !ps.jump_held) {
            cmd.buttons = QK_BUTTON_JUMP;
            if (!prev_on_ground) {
                strafe_dir = -strafe_dir;
                cmd.side_move = (f32)strafe_dir;
            }
        }
        /* else: no jump button → clears jump_held for next landing */

        /* Turn in strafe direction while airborne */
        if (!ps.on_ground) {
            yaw += 2.5f * (f32)strafe_dir;
        }
        cmd.yaw = yaw;

        prev_on_ground = ps.on_ground;
        qk_physics_move(&ps, &cmd, world);

        f32 speed = sqrtf(ps.velocity.x * ps.velocity.x +
                          ps.velocity.y * ps.velocity.y);
        if (speed > peak_speed) peak_speed = speed;
    }

    printf("  [INFO] peak_speed=%.1f (max_speed=%.1f, 120%%=%.1f)\n",
           (double)peak_speed, (double)QK_PM_MAX_SPEED,
           (double)(QK_PM_MAX_SPEED * 1.2f));

    TEST_CHECK(peak_speed > QK_PM_MAX_SPEED * 1.2f,
               "Peak speed > 120% of max_speed via strafejumping");

    qk_physics_world_destroy(world);
}

/* ---- Test 3: rocket_jump ---- */

static void test_rocket_jump(void) {
    printf("\n=== Test: rocket_jump ===\n");
    s_current_test = "rocket_jump";

    qk_phys_world_t *world = qk_physics_world_create_test_room();
    qk_game_config_t gc = {0};
    qk_game_init(&gc);

    setup_player(0, "RJPlayer", QK_TEAM_ALPHA, (vec3_t){0, 0, 24},
                 QK_WEAPON_ROCKET);

    /* Force CA state to PLAYING with time remaining */
    qk_game_state_t *gs = qk_game_get_state();
    gs->ca.state = CA_STATE_PLAYING;
    gs->ca.state_timer_ms = 120000;

    /* Aim straight down and fire + jump */
    qk_usercmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.pitch = -89.0f;
    cmd.yaw = 0.0f;
    cmd.buttons = QK_BUTTON_ATTACK | QK_BUTTON_JUMP;

    qk_game_player_command(0, &cmd);
    qk_game_tick(world, QK_TICK_DT);

    /* Continue ticking for the rocket to reach floor and explode */
    cmd.buttons = 0;
    for (int i = 0; i < 30; i++) {
        qk_game_player_command(0, &cmd);
        qk_game_tick(world, QK_TICK_DT);
    }

    const qk_player_state_t *ps = qk_game_get_player_state(0);
    printf("  [INFO] velocity.z=%.1f origin.z=%.1f health=%d\n",
           (double)ps->velocity.z, (double)ps->origin.z, ps->health);

    TEST_CHECK(ps->velocity.z > 100.0f || ps->origin.z > 50.0f,
               "Player launched upward by rocket explosion");

    qk_game_shutdown();
    qk_physics_world_destroy(world);
}

/* ---- Test 4: rail_damage ---- */

static void test_rail_damage(void) {
    printf("\n=== Test: rail_damage ===\n");
    s_current_test = "rail_damage";

    qk_phys_world_t *world = qk_physics_world_create_test_room();
    qk_game_config_t gc = {0};
    qk_game_init(&gc);

    setup_player(0, "Attacker", QK_TEAM_ALPHA, (vec3_t){0, 0, 24},
                 QK_WEAPON_RAIL);
    setup_player(1, "Target", QK_TEAM_BETA, (vec3_t){200, 0, 24},
                 QK_WEAPON_ROCKET);

    /* Force CA state to PLAYING with time remaining */
    qk_game_state_t *gs = qk_game_get_state();
    gs->ca.state = CA_STATE_PLAYING;
    gs->ca.state_timer_ms = 120000;

    /* Attacker aims at target (+X direction, yaw=0, pitch=0) and fires */
    qk_usercmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.yaw = 0.0f;
    cmd.pitch = 0.0f;
    cmd.buttons = QK_BUTTON_ATTACK;

    qk_game_player_command(0, &cmd);
    qk_game_tick(world, QK_TICK_DT);

    const qk_player_state_t *target = qk_game_get_player_state(1);
    i16 total_hp = target->health + target->armor;

    printf("  [INFO] target health=%d armor=%d total=%d (started at %d)\n",
           target->health, target->armor, total_hp,
           QK_CA_SPAWN_HEALTH + QK_CA_SPAWN_ARMOR);

    TEST_CHECK(total_hp < QK_CA_SPAWN_HEALTH + QK_CA_SPAWN_ARMOR,
               "Target took damage from rail hit");

    /* Rail does 80 damage; check approximate damage */
    i16 damage_taken = (QK_CA_SPAWN_HEALTH + QK_CA_SPAWN_ARMOR) - total_hp;
    TEST_CHECK(damage_taken >= 60 && damage_taken <= 100,
               "Damage taken in expected range (60-100)");

    qk_game_shutdown();
    qk_physics_world_destroy(world);
}

/* ---- Test 5: ca_lifecycle ---- */

static void test_ca_lifecycle(void) {
    printf("\n=== Test: ca_lifecycle ===\n");
    s_current_test = "ca_lifecycle";

    qk_phys_world_t *world = qk_physics_world_create_test_room();
    qk_game_config_t gc = {0};
    gc.countdown_time_ms = 100;        /* fast for testing */
    gc.round_time_limit_ms = 500;
    qk_game_init(&gc);

    /* Connect two players on different teams */
    setup_player(0, "Alpha1", QK_TEAM_ALPHA, (vec3_t){-100, 0, 24},
                 QK_WEAPON_ROCKET);
    setup_player(1, "Beta1", QK_TEAM_BETA, (vec3_t){100, 0, 24},
                 QK_WEAPON_ROCKET);

    const qk_ca_state_t *ca = qk_game_get_ca_state();
    TEST_CHECK(ca->state == CA_STATE_WARMUP, "Initial state is WARMUP");

    /* Warmup has no auto-transition; manually start countdown */
    qk_game_state_t *gs = qk_game_get_state();
    g_ca_start_countdown(gs);

    ca = qk_game_get_ca_state();
    TEST_CHECK(ca->state == CA_STATE_COUNTDOWN, "Transitioned to COUNTDOWN");

    /* Tick through countdown (100ms / ~7.8ms per tick = ~13 ticks; use 20) */
    for (int i = 0; i < 20; i++) {
        qk_game_tick(world, QK_TICK_DT);
    }

    ca = qk_game_get_ca_state();
    printf("  [INFO] state after countdown: %d\n", ca->state);
    TEST_CHECK(ca->state == CA_STATE_PLAYING,
               "Reached PLAYING after countdown");

    /* Tick through round time limit (500ms / ~7.8ms = ~64 ticks; use 80) */
    for (int i = 0; i < 80; i++) {
        qk_game_tick(world, QK_TICK_DT);
    }

    ca = qk_game_get_ca_state();
    printf("  [INFO] state after round timeout: %d\n", ca->state);
    TEST_CHECK(ca->state >= CA_STATE_ROUND_END,
               "Transitioned to ROUND_END after time limit");

    qk_game_shutdown();
    qk_physics_world_destroy(world);
}

/* ---- Test 6: physics_trace ---- */

static void test_physics_trace(void) {
    printf("\n=== Test: physics_trace ===\n");
    s_current_test = "physics_trace";

    qk_phys_world_t *world = qk_physics_world_create_test_room();
    vec3_t zero = {0, 0, 0};

    /* Trace downward from center: should hit floor */
    qk_trace_result_t r = qk_physics_trace(world,
        (vec3_t){0, 0, 128}, (vec3_t){0, 0, -100}, zero, zero);
    TEST_CHECK(r.fraction < 1.0f, "Downward trace hits floor");
    if (r.fraction < 1.0f) {
        TEST_CHECK(r.hit_normal.z > 0.9f, "Floor normal points up");
    }

    /* Trace toward +X wall from center */
    r = qk_physics_trace(world,
        (vec3_t){0, 0, 128}, (vec3_t){2000, 0, 128}, zero, zero);
    TEST_CHECK(r.fraction < 1.0f, "Trace hits +X wall");

    /* Short trace in open space: should not hit */
    r = qk_physics_trace(world,
        (vec3_t){0, 0, 128}, (vec3_t){100, 0, 128}, zero, zero);
    TEST_CHECK(r.fraction >= 0.99f, "Short trace in open space misses");

    /* Trace toward -Y wall */
    r = qk_physics_trace(world,
        (vec3_t){0, 0, 128}, (vec3_t){0, -2000, 128}, zero, zero);
    TEST_CHECK(r.fraction < 1.0f, "Trace hits -Y wall");

    qk_physics_world_destroy(world);
}

/* ---- Test Registry ---- */

typedef struct {
    const char *name;
    void (*func)(void);
} test_entry_t;

static const test_entry_t s_tests[] = {
    { "forward_move",   test_forward_move },
    { "strafejump",     test_strafejump },
    { "rocket_jump",    test_rocket_jump },
    { "rail_damage",    test_rail_damage },
    { "ca_lifecycle",   test_ca_lifecycle },
    { "physics_trace",  test_physics_trace },
};

#define NUM_TESTS (sizeof(s_tests) / sizeof(s_tests[0]))

/* ---- Main ---- */

int main(int argc, char **argv) {
    bool run_all = false;
    const char *run_single = NULL;
    bool list_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            run_all = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) {
            run_single = argv[++i];
        }
    }

    if (list_only) {
        printf("Available tests:\n");
        for (u32 i = 0; i < NUM_TESTS; i++) {
            printf("  %s\n", s_tests[i].name);
        }
        return 0;
    }

    /* Default to --all if no specific test requested */
    if (!run_all && !run_single) {
        run_all = true;
    }

    printf("QUICKEN Automated Test Harness\n");
    printf("==============================\n");

    if (run_all) {
        for (u32 i = 0; i < NUM_TESTS; i++) {
            s_tests[i].func();
        }
    } else if (run_single) {
        bool found = false;
        for (u32 i = 0; i < NUM_TESTS; i++) {
            if (strcmp(s_tests[i].name, run_single) == 0) {
                s_tests[i].func();
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown test: %s\n", run_single);
            return 1;
        }
    }

    printf("\n==============================\n");
    printf("Results: %d passed, %d failed\n", s_tests_passed, s_tests_failed);
    printf("CSV:SUMMARY,%d,%d\n", s_tests_passed, s_tests_failed);

    return s_tests_failed > 0 ? 1 : 0;
}
