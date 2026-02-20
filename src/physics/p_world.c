/*
 * QUICKEN Engine - Physics World
 *
 * World creation and destruction. Brute force for the vertical slice
 * (no BVH spatial acceleration).
 */

#include "p_internal.h"
#include <stdlib.h>

// --- Create physics world from collision model ---

qk_phys_world_t *p_world_create(qk_collision_model_t *cm) {
    if (!cm) return NULL;

    qk_phys_world_t *world = (qk_phys_world_t *)malloc(sizeof(qk_phys_world_t));
    if (!world) return NULL;

    world->cm = cm;
    world->owns_cm = false; // caller manages collision model lifetime

    // For each brush: compute AABB, then add bevel planes for correct
    // box tracing against raw .map geometry (no BSP compiler bevels).
    for (u32 i = 0; i < cm->brush_count; i++) {
        p_brush_compute_aabb(&cm->brushes[i]);
        p_brush_add_bevels(&cm->brushes[i]);
    }

    return world;
}

// --- Destroy physics world ---

void p_world_destroy(qk_phys_world_t *world) {
    if (!world) return;
    if (world->owns_cm && world->cm) {
        for (u32 i = 0; i < world->cm->brush_count; i++) {
            free(world->cm->brushes[i].planes);
        }
        free(world->cm->brushes);
        free(world->cm);
    }
    free(world);
}
