/*
 * QUICKEN Engine - Client Map Loading
 *
 * Map file resolution and world rebuild helpers.
 */

#ifndef CL_MAP_H
#define CL_MAP_H

#include "quicken.h"
#include "core/qk_map.h"
#include "renderer/qk_renderer.h"
#include "physics/qk_physics.h"

/* Resolve a map name to its file path.
 * Searches: assets/maps/<name>, assets/maps/<name>.bsp,
 *           assets/maps/<name>.map, then raw <name>.
 * Returns true if a valid file was found. */
bool cl_map_resolve(const char *name, char *out_path, u32 path_size);

/* Build physics world and upload render geometry from loaded map data.
 * fallback_tex is assigned to surfaces lacking a real texture.
 * Returns the new physics world (test room fallback if collision is empty). */
qk_phys_world_t *cl_map_build_world(qk_map_data_t *map,
                                      qk_texture_id_t fallback_tex);

#endif /* CL_MAP_H */
