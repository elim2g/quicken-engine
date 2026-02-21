/*
 * QUICKEN Engine - Test Room
 */

#include "client/cl_testroom.h"

#include <stdio.h>
#include <string.h>

#include "quicken.h"
#include "renderer/qk_renderer.h"

qk_texture_id_t cl_testroom_create_texture(void) {
    #define GRID_SIZE 256
    #define GRID_LINE 4   // line width in pixels
    #define GRID_CELL 64  // cell size in pixels

    static u8 pixels[GRID_SIZE * GRID_SIZE * 4];

    for (u32 y = 0; y < GRID_SIZE; y++) {
        for (u32 x = 0; x < GRID_SIZE; x++) {
            u32 idx = (y * GRID_SIZE + x) * 4;
            bool on_line = (x % GRID_CELL) < GRID_LINE ||
                           (y % GRID_CELL) < GRID_LINE;
            u8 val = on_line ? 0x80 : 0x50;
            pixels[idx + 0] = val;
            pixels[idx + 1] = val;
            pixels[idx + 2] = val;
            pixels[idx + 3] = 0xFF;
        }
    }

    qk_texture_id_t id = qk_renderer_upload_texture(
        pixels, GRID_SIZE, GRID_SIZE, 4, false);

    #undef GRID_SIZE
    #undef GRID_LINE
    #undef GRID_CELL
    return id;
}

void cl_testroom_upload_geometry(qk_texture_id_t tex_id) {
    /*
     * Match the physics test room: interior [-1024,1024] x [-1024,1024] x [0,256].
     * 6 faces (floor, ceiling, 4 walls), 24 verts, 36 indices, 6 surfaces.
     */
    #define TR_HALF  1024.0f
    #define TR_TOP   256.0f
    #define TR_UV_SCALE 128.0f

    static const struct { f32 p[3]; f32 n[3]; } face_data[6][4] = {
        // Floor (Z=0, normal up)
        { {{-TR_HALF,-TR_HALF,0}, {0,0,1}},  {{ TR_HALF,-TR_HALF,0}, {0,0,1}},
          {{ TR_HALF, TR_HALF,0}, {0,0,1}},  {{-TR_HALF, TR_HALF,0}, {0,0,1}} },
        // Ceiling (Z=256, normal down)
        { {{-TR_HALF,-TR_HALF,TR_TOP}, {0,0,-1}},  {{-TR_HALF, TR_HALF,TR_TOP}, {0,0,-1}},
          {{ TR_HALF, TR_HALF,TR_TOP}, {0,0,-1}},  {{ TR_HALF,-TR_HALF,TR_TOP}, {0,0,-1}} },
        // +X wall (normal inward -X)
        { {{ TR_HALF, TR_HALF,0}, {-1,0,0}},  {{ TR_HALF,-TR_HALF,0}, {-1,0,0}},
          {{ TR_HALF,-TR_HALF,TR_TOP}, {-1,0,0}},  {{ TR_HALF, TR_HALF,TR_TOP}, {-1,0,0}} },
        // -X wall (normal inward +X)
        { {{-TR_HALF,-TR_HALF,0}, {1,0,0}},  {{-TR_HALF, TR_HALF,0}, {1,0,0}},
          {{-TR_HALF, TR_HALF,TR_TOP}, {1,0,0}},  {{-TR_HALF,-TR_HALF,TR_TOP}, {1,0,0}} },
        // +Y wall (normal inward -Y)
        { {{-TR_HALF, TR_HALF,0}, {0,-1,0}},  {{ TR_HALF, TR_HALF,0}, {0,-1,0}},
          {{ TR_HALF, TR_HALF,TR_TOP}, {0,-1,0}},  {{-TR_HALF, TR_HALF,TR_TOP}, {0,-1,0}} },
        // -Y wall (normal inward +Y)
        { {{ TR_HALF,-TR_HALF,0}, {0,1,0}},  {{-TR_HALF,-TR_HALF,0}, {0,1,0}},
          {{-TR_HALF,-TR_HALF,TR_TOP}, {0,1,0}},  {{ TR_HALF,-TR_HALF,TR_TOP}, {0,1,0}} },
    };

    // UV axis indices per face: floor/ceiling use XY, X-walls use YZ, Y-walls use XZ
    static const u32 uv_axis[6][2] = {
        {0, 1},  // floor:    X, Y
        {0, 1},  // ceiling:  X, Y
        {1, 2},  // +X wall:  Y, Z
        {1, 2},  // -X wall:  Y, Z
        {0, 2},  // +Y wall:  X, Z
        {0, 2},  // -Y wall:  X, Z
    };

    qk_world_vertex_t verts[24];
    u32 indices[36];
    qk_draw_surface_t surfaces[6];

    for (u32 f = 0; f < 6; f++) {
        u32 base = f * 4;
        for (u32 v = 0; v < 4; v++) {
            qk_world_vertex_t *vert = &verts[base + v];
            vert->position[0] = face_data[f][v].p[0];
            vert->position[1] = face_data[f][v].p[1];
            vert->position[2] = face_data[f][v].p[2];
            vert->normal[0]   = face_data[f][v].n[0];
            vert->normal[1]   = face_data[f][v].n[1];
            vert->normal[2]   = face_data[f][v].n[2];
            vert->uv[0] = vert->position[uv_axis[f][0]] / TR_UV_SCALE;
            vert->uv[1] = vert->position[uv_axis[f][1]] / TR_UV_SCALE;
            vert->texture_id = tex_id;
        }

        u32 base_idx = f * 6;
        indices[base_idx + 0] = base + 0;
        indices[base_idx + 1] = base + 1;
        indices[base_idx + 2] = base + 2;
        indices[base_idx + 3] = base + 0;
        indices[base_idx + 4] = base + 2;
        indices[base_idx + 5] = base + 3;

        surfaces[f].index_offset  = base_idx;
        surfaces[f].index_count   = 6;
        surfaces[f].vertex_offset = base;
        surfaces[f].texture_index = tex_id;
    }

    qk_result_t res = qk_renderer_upload_world(verts, 24, indices, 36, surfaces, 6);
    if (res != QK_SUCCESS) {
        fprintf(stderr, "Warning: Failed to upload test room geometry (%d)\n", res);
    } else {
        printf("Test room geometry uploaded (6 faces, grid texture %u).\n", tex_id);
    }

    #undef TR_HALF
    #undef TR_TOP
    #undef TR_UV_SCALE
}
