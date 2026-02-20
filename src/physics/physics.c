/*
 * QUICKEN Engine - Physics Public API
 *
 * Delegates to internal implementations in p_*.c files.
 * This is the only file that provides symbols matching qk_physics.h.
 */

#include "p_internal.h"
#include <stdlib.h>
#include <string.h>

// --- World lifecycle ---

qk_phys_world_t *qk_physics_world_create(qk_collision_model_t *cm) {
    return p_world_create(cm);
}

void qk_physics_world_destroy(qk_phys_world_t *world) {
    p_world_destroy(world);
}

// --- Create a hardcoded test room ---

/*
 * Helper: create a slab brush (axis-aligned box defined by 6 planes).
 * lo/hi define the box extents on each axis.
 */
static qk_brush_t p_make_box_brush(f32 lo_x, f32 lo_y, f32 lo_z,
                                    f32 hi_x, f32 hi_y, f32 hi_z) {
    qk_brush_t brush = {0};
    brush.plane_count = 6;
    brush.planes = (qk_plane_t *)malloc(6 * sizeof(qk_plane_t));

    // +X face
    brush.planes[0].normal = (vec3_t){ 1.0f, 0.0f, 0.0f };
    brush.planes[0].dist = hi_x;
    // -X face
    brush.planes[1].normal = (vec3_t){-1.0f, 0.0f, 0.0f };
    brush.planes[1].dist = -lo_x;
    // +Y face
    brush.planes[2].normal = (vec3_t){ 0.0f, 1.0f, 0.0f };
    brush.planes[2].dist = hi_y;
    // -Y face
    brush.planes[3].normal = (vec3_t){ 0.0f,-1.0f, 0.0f };
    brush.planes[3].dist = -lo_y;
    // +Z face (up)
    brush.planes[4].normal = (vec3_t){ 0.0f, 0.0f, 1.0f };
    brush.planes[4].dist = hi_z;
    // -Z face (down)
    brush.planes[5].normal = (vec3_t){ 0.0f, 0.0f,-1.0f };
    brush.planes[5].dist = -lo_z;

    return brush;
}

qk_phys_world_t *qk_physics_world_create_test_room(void) {
    /*
     * 16384x16384x256 hollow room centered at origin.
     * Interior spans: X [-8192, 8192], Y [-8192, 8192], Z [0, 256].
     * Wall thickness: 16 units.
     *
     * 6 slab brushes: floor, ceiling, +X wall, -X wall, +Y wall, -Y wall.
     */
    #define ROOM_HALF   8192.0f
    #define ROOM_TOP    256.0f
    #define WALL        16.0f

    qk_collision_model_t *cm = (qk_collision_model_t *)malloc(sizeof(qk_collision_model_t));
    if (!cm) return NULL;

    cm->brush_count = 6;
    cm->brushes = (qk_brush_t *)malloc(6 * sizeof(qk_brush_t));
    if (!cm->brushes) { free(cm); return NULL; }

    // Floor: z from -WALL to 0
    cm->brushes[0] = p_make_box_brush(
        -ROOM_HALF - WALL, -ROOM_HALF - WALL, -WALL,
         ROOM_HALF + WALL,  ROOM_HALF + WALL,  0.0f);

    // Ceiling: z from ROOM_TOP to ROOM_TOP + WALL
    cm->brushes[1] = p_make_box_brush(
        -ROOM_HALF - WALL, -ROOM_HALF - WALL, ROOM_TOP,
         ROOM_HALF + WALL,  ROOM_HALF + WALL, ROOM_TOP + WALL);

    // +X wall
    cm->brushes[2] = p_make_box_brush(
         ROOM_HALF, -ROOM_HALF - WALL, -WALL,
         ROOM_HALF + WALL,  ROOM_HALF + WALL, ROOM_TOP + WALL);

    // -X wall
    cm->brushes[3] = p_make_box_brush(
        -ROOM_HALF - WALL, -ROOM_HALF - WALL, -WALL,
        -ROOM_HALF,         ROOM_HALF + WALL, ROOM_TOP + WALL);

    // +Y wall
    cm->brushes[4] = p_make_box_brush(
        -ROOM_HALF - WALL,  ROOM_HALF, -WALL,
         ROOM_HALF + WALL,  ROOM_HALF + WALL, ROOM_TOP + WALL);

    // -Y wall
    cm->brushes[5] = p_make_box_brush(
        -ROOM_HALF - WALL, -ROOM_HALF - WALL, -WALL,
         ROOM_HALF + WALL, -ROOM_HALF,        ROOM_TOP + WALL);

    #undef ROOM_HALF
    #undef ROOM_TOP
    #undef WALL

    qk_phys_world_t *world = qk_physics_world_create(cm);
    if (world) world->owns_cm = true; // test room owns its collision model
    return world;
}

// --- Player init ---

void qk_physics_player_init(qk_player_state_t *ps, vec3_t spawn_origin) {
    if (!ps) return;

    ps->origin = spawn_origin;
    ps->velocity = (vec3_t){0.0f, 0.0f, 0.0f};
    ps->mins = QK_PLAYER_MINS;
    ps->maxs = QK_PLAYER_MAXS;
    ps->on_ground = false;
    ps->ground_normal = (vec3_t){0.0f, 0.0f, 0.0f};
    ps->jump_held = false;
    ps->max_speed = QK_PM_MAX_SPEED;
    ps->gravity = QK_PM_GRAVITY;
    ps->command_time = 0;
}

// --- Run one physics tick ---

void qk_physics_move(qk_player_state_t *ps, const qk_usercmd_t *cmd,
                      const qk_phys_world_t *world) {
    p_move(ps, cmd, world);
}

// --- Fixed-timestep wrapper ---

void qk_physics_update(qk_phys_time_t *ts, f32 frame_dt,
                        qk_player_state_t *ps, const qk_usercmd_t *cmd,
                        const qk_phys_world_t *world) {
    p_time_update(ts, frame_dt, ps, cmd, world);
}

// --- Trace a box through the world ---

qk_trace_result_t qk_physics_trace(const qk_phys_world_t *world,
                                     vec3_t start, vec3_t end,
                                     vec3_t mins, vec3_t maxs) {
    return p_trace_world(world, start, end, mins, maxs);
}

// --- Jump pad trajectory calculation ---

vec3_t qk_physics_jumppad_velocity(vec3_t start, vec3_t target) {
    return p_calc_launch_velocity(start, target, QK_PM_GRAVITY);
}

// --- Get interpolation alpha for rendering ---

f32 qk_physics_get_alpha(const qk_phys_time_t *ts) {
    return p_time_get_alpha(ts);
}
