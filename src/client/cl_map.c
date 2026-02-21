/*
 * QUICKEN Engine - Client Map Loading
 */

#include "client/cl_map.h"

#include <stdio.h>
#include <string.h>

bool cl_map_resolve(const char *name, char *out_path, u32 path_size) {
    // Try exact name in assets/maps/ first (handles "asylum.bsp")
    snprintf(out_path, path_size, "assets/maps/%s", name);
    {
        FILE *map_file = fopen(out_path, "rb");
        if (map_file) { fclose(map_file); return true; }
    }

    // Then try appending extensions (handles bare "asylum")
    const char *exts[] = { ".bsp", ".map" };
    for (int e = 0; e < 2; e++) {
        snprintf(out_path, path_size, "assets/maps/%s%s", name, exts[e]);
        FILE *map_file = fopen(out_path, "rb");
        if (map_file) { fclose(map_file); return true; }
    }

    // Try as raw path (absolute or relative)
    snprintf(out_path, path_size, "%s", name);
    {
        FILE *map_file = fopen(out_path, "rb");
        if (map_file) { fclose(map_file); return true; }
    }

    return false;
}

qk_phys_world_t *cl_map_build_world(qk_map_data_t *map,
                                      qk_texture_id_t fallback_tex) {
    // Physics world
    qk_phys_world_t *world = NULL;
    if (map->collision.brush_count > 0)
        world = qk_physics_world_create(&map->collision);
    if (!world)
        world = qk_physics_world_create_test_room();

    // Render geometry
    if (map->vertex_count > 0) {
        for (u32 si = 0; si < map->surface_count; si++)
            map->surfaces[si].texture_index = fallback_tex;
        qk_renderer_upload_world(map->vertices, map->vertex_count,
                                  map->indices, map->index_count,
                                  map->surfaces, map->surface_count);
    }

    // Lightmap atlas
    if (map->lightmap_atlas) {
        qk_renderer_upload_lightmap_atlas(
            map->lightmap_atlas,
            map->lightmap_atlas_width,
            map->lightmap_atlas_height);
    }

    return world;
}
