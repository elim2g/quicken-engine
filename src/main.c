/*
 * QUICKEN Engine - Main Entry Point
 *
 * High-performance Arena FPS engine
 * Target: 1000+ fps on modern hardware
 */

#include <stdio.h>
#include <stdbool.h>
#include "quicken.h"
#include "physics/physics.h"
#include "renderer/renderer.h"

/* SDL will be included once integrated */
/* #include <SDL3/SDL.h> */

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    printf("QUICKEN Engine v%d.%d.%d\n",
           QUICKEN_VERSION_MAJOR,
           QUICKEN_VERSION_MINOR,
           QUICKEN_VERSION_PATCH);
    printf("Target: 1000+ fps Arena FPS\n");
    printf("Movement: TURNT (QUAKE 4-style)\n");
    printf("Combat: Arena FPS (QUAKE LIVE-style)\n\n");

    printf("Build Configuration:\n");
    #ifdef QUICKEN_DEBUG
        printf("  Mode: Debug\n");
    #else
        printf("  Mode: Release\n");
    #endif

    printf("  Physics: Precise floating-point (cross-platform determinism)\n");
    printf("  Renderer: Fast floating-point (maximum performance)\n\n");

    /* Initialize subsystems */
    physics_init();
    renderer_init();

    printf("Subsystems initialized successfully.\n");
    printf("SDL3 integration coming next...\n");

    /* Cleanup */
    renderer_shutdown();
    physics_shutdown();

    return 0;
}
