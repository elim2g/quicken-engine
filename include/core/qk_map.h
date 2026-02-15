/*
 * QUICKEN Engine - Map Loader
 *
 * Parses Quake 3 .map format and produces:
 *   1. Collision model (brushes with planes) for physics
 *   2. Render geometry (vertices + indices + surfaces) for renderer
 *   3. Spawn points for gameplay
 *
 * Texture references are hashed to deterministic solid colors (no real textures).
 */

#ifndef QK_MAP_H
#define QK_MAP_H

#include "quicken.h"
#include "qk_math.h"
#include "qk_types.h"
#include "physics/qk_physics.h"
#include "renderer/qk_renderer.h"

/* Maximum limits */
#define QK_MAP_MAX_BRUSHES          4096
#define QK_MAP_MAX_PLANES_PER_BRUSH 32
#define QK_MAP_MAX_VERTICES         65536
#define QK_MAP_MAX_INDICES          (65536 * 3)
#define QK_MAP_MAX_SURFACES         4096
#define QK_MAP_MAX_SPAWN_POINTS     64
#define QK_MAP_MAX_TELEPORTERS      64
#define QK_MAP_MAX_JUMP_PADS        64

/* Map data produced by the loader */
typedef struct {
    /* Collision data for physics */
    qk_collision_model_t    collision;

    /* Render data for renderer */
    qk_world_vertex_t      *vertices;
    u32                     vertex_count;
    u32                    *indices;
    u32                     index_count;
    qk_draw_surface_t      *surfaces;
    u32                     surface_count;

    /* Spawn points for gameplay */
    qk_spawn_point_t       *spawn_points;
    u32                     spawn_count;

    /* Teleporters for gameplay */
    qk_teleporter_t        *teleporters;
    u32                     teleporter_count;

    /* Jump pads for gameplay */
    qk_jump_pad_t          *jump_pads;
    u32                     jump_pad_count;
} qk_map_data_t;

/* Load a .map file and produce all game data */
qk_result_t qk_map_load(const char *filepath, qk_map_data_t *out);

/* Load from a string buffer (for embedded maps) */
qk_result_t qk_map_load_from_memory(const char *data, u64 data_len, qk_map_data_t *out);

/* Load from a Q3 BSP binary blob (IBSP v46/v47) */
qk_result_t qk_bsp_load(const u8 *data, u64 data_len, qk_map_data_t *out);

/* Free all memory allocated by qk_map_load */
void qk_map_free(qk_map_data_t *map);

#endif /* QK_MAP_H */
