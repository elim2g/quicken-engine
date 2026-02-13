/*
 * QUICKEN Engine - Dedicated Server Entry Point
 *
 * Headless mode: no window, no renderer, no SDL3.
 * Runs the authoritative game simulation + netcode only.
 */

#include <stdio.h>
#include "quicken.h"
#include "qk_arena.h"
#include "physics/qk_physics.h"
#include "netcode/qk_netcode.h"
#include "gameplay/qk_gameplay.h"
#include "core/qk_platform.h"

int main(int argc, char *argv[]) {
    QK_UNUSED(argc);
    QK_UNUSED(argv);

    printf("QUICKEN Dedicated Server v%d.%d.%d\n",
           QUICKEN_VERSION_MAJOR,
           QUICKEN_VERSION_MINOR,
           QUICKEN_VERSION_PATCH);
    printf("Headless mode (QK_HEADLESS)\n\n");

    /* Arena allocator */
    qk_arena_t *arena = qk_arena_create(1024 * 1024);
    if (!arena) {
        fprintf(stderr, "FATAL: Failed to create arena\n");
        return 1;
    }
    printf("  Arena allocator: OK\n");

    /* Initialize stubs */
    qk_game_config_t gc = {0};
    qk_game_init(&gc);
    printf("  Gameplay:        OK\n");

    qk_net_server_config_t nsc = {0};
    qk_net_server_init(&nsc);
    printf("  Netcode:         OK\n");

    printf("\nDedicated server stubs verified.\n");

    /* Shutdown */
    qk_net_server_shutdown();
    qk_game_shutdown();
    qk_arena_destroy(arena);

    printf("Clean shutdown.\n");
    return 0;
}
