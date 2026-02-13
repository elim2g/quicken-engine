/*
 * QUICKEN Engine - Main Entry Point
 *
 * Phase 0: Verifies all stubs link and arena allocator works.
 * The gameplay agent owns this file and will wire the full game loop.
 */

#include <stdio.h>
#include "quicken.h"
#include "qk_arena.h"
#include "physics/qk_physics.h"
#include "renderer/qk_renderer.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"
#include "ui/qk_ui.h"
#include "core/qk_platform.h"

int main(int argc, char *argv[]) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);

    printf("QUICKEN Engine v%d.%d.%d\n",
           QUICKEN_VERSION_MAJOR,
           QUICKEN_VERSION_MINOR,
           QUICKEN_VERSION_PATCH);
    printf("Build: ");
#ifdef QUICKEN_DEBUG
    printf("Debug\n");
#else
    printf("Release\n");
#endif
    printf("\n");

    /* Verify arena allocator (real implementation) */
    qk_arena_t *arena = qk_arena_create(1024 * 1024);
    if (!arena) {
        fprintf(stderr, "FATAL: Failed to create arena\n");
        return 1;
    }

    void *test_alloc = qk_arena_alloc(arena, 256);
    if (!test_alloc) {
        fprintf(stderr, "FATAL: Arena alloc failed\n");
        qk_arena_destroy(arena);
        return 1;
    }

    printf("  Arena allocator: OK\n");
    qk_arena_reset(arena);

    /* Verify all module stubs link */
    qk_renderer_config_t rc = {0};
    qk_renderer_init(&rc);
    printf("  Renderer stub:   OK\n");

    qk_net_server_config_t nsc = {0};
    qk_net_server_init(&nsc);
    printf("  Netcode stub:    OK\n");

    qk_game_config_t gc = {0};
    qk_game_init(&gc);
    printf("  Gameplay stub:   OK\n");

    /* Verify physics stub */
    qk_trace_result_t tr = qk_physics_trace(NULL,
        (vec3_t){0, 0, 0}, (vec3_t){100, 0, 0},
        (vec3_t){-1, -1, -1}, (vec3_t){1, 1, 1});
    printf("  Physics stub:    OK (trace fraction=%.1f)\n", (double)tr.fraction);

    /* Verify UI stub */
    qk_ui_tick(16);
    printf("  UI stub:         OK\n");

    printf("\nPhase 0 contract lock verified. All subsystems link.\n");

    /* Clean shutdown */
    qk_renderer_shutdown();
    qk_net_server_shutdown();
    qk_game_shutdown();
    qk_arena_destroy(arena);

    printf("Clean shutdown.\n");
    return 0;
}
