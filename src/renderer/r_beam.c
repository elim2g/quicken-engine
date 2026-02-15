/*
 * QUICKEN Renderer - Beam Effects (Rail + Lightning Gun)
 *
 * Rail beam: tight spiral of camera-facing quads with additive blending.
 * LG beam: chain of wobbling connected quads for electric arc effect.
 *
 * Both use a shared dynamic vertex buffer (host-visible, mapped persistently)
 * that gets rebuilt every frame. Vertices are generated CPU-side since beams
 * are transient visual effects.
 */

#include "r_types.h"
#include "renderer/qk_renderer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- Constants ---- */

#define PI 3.14159265358979f

/* Rail beam parameters */
#define RAIL_CORE_HALF_WIDTH    0.8f
#define RAIL_SPIRAL_PITCH       40.0f   /* world units per full spiral revolution */
#define RAIL_SPIRAL_PTS_PER_TURN 28     /* particles per revolution */
#define RAIL_SPIRAL_MIN_POINTS  12
#define RAIL_SPIRAL_MAX_POINTS  512
#define RAIL_SPIRAL_RADIUS      2.5f
#define RAIL_PARTICLE_HALF_SIZE 1.2f
#define RAIL_FADE_TIME          1.75f
#define RAIL_FIZZLE_AMPLITUDE   6.0f    /* max displacement at end of life */

/* LG beam parameters */
#define LG_SEGMENTS             24
#define LG_BEAM_LAYERS          3
#define LG_CORE_HALF_WIDTH      0.6f
#define LG_OUTER_HALF_WIDTH     1.8f
#define LG_WOBBLE_AMPLITUDE     4.0f
#define LG_WOBBLE_FREQ          12.0f

/* ---- Helpers ---- */

static void r_beam_cross(f32 *out, const f32 *a, const f32 *b)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static f32 r_beam_dot(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void r_beam_normalize(f32 *v)
{
    f32 len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) {
        f32 inv = 1.0f / len;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

static f32 r_beam_hash(u32 x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = (x >> 16) ^ x;
    return (f32)(x & 0xFFFF) / 65535.0f;
}

/* Emit a camera-facing quad (two triangles, 6 vertices) centered at `center`
 * with half-extent `half_size` and the given color. The quad faces the camera. */
static u32 r_beam_emit_billboard_quad(r_beam_vertex_t *verts, u32 offset,
                                       const f32 *center, f32 half_size,
                                       const f32 *cam_pos, const f32 *beam_axis,
                                       const f32 *color)
{
    if (offset + 6 > R_BEAM_MAX_VERTICES) return 0;

    /* Billboard direction: camera to center */
    f32 to_cam[3] = {
        cam_pos[0] - center[0],
        cam_pos[1] - center[1],
        cam_pos[2] - center[2]
    };
    r_beam_normalize(to_cam);

    /* Right vector = cross(beam_axis, to_cam) */
    f32 right[3];
    r_beam_cross(right, beam_axis, to_cam);
    r_beam_normalize(right);

    /* Up vector = cross(right, beam_axis) */
    f32 up[3];
    r_beam_cross(up, right, beam_axis);
    r_beam_normalize(up);

    /* Quad corners */
    f32 corners[4][3];
    for (int i = 0; i < 3; i++) {
        corners[0][i] = center[i] - right[i] * half_size - up[i] * half_size;
        corners[1][i] = center[i] + right[i] * half_size - up[i] * half_size;
        corners[2][i] = center[i] + right[i] * half_size + up[i] * half_size;
        corners[3][i] = center[i] - right[i] * half_size + up[i] * half_size;
    }

    /* Triangle 1: 0-1-2 */
    memcpy(verts[offset + 0].position, corners[0], sizeof(f32) * 3);
    memcpy(verts[offset + 0].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 1].position, corners[1], sizeof(f32) * 3);
    memcpy(verts[offset + 1].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 2].position, corners[2], sizeof(f32) * 3);
    memcpy(verts[offset + 2].color, color, sizeof(f32) * 4);

    /* Triangle 2: 0-2-3 */
    memcpy(verts[offset + 3].position, corners[0], sizeof(f32) * 3);
    memcpy(verts[offset + 3].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 4].position, corners[2], sizeof(f32) * 3);
    memcpy(verts[offset + 4].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 5].position, corners[3], sizeof(f32) * 3);
    memcpy(verts[offset + 5].color, color, sizeof(f32) * 4);

    return 6;
}

/* Emit a beam-axis-aligned quad strip segment (two triangles, 6 vertices).
 * The quad connects two points along the beam and faces the camera. */
static u32 r_beam_emit_strip_quad(r_beam_vertex_t *verts, u32 offset,
                                   const f32 *p0, const f32 *p1,
                                   f32 half_width, const f32 *cam_pos,
                                   const f32 *color)
{
    if (offset + 6 > R_BEAM_MAX_VERTICES) return 0;

    /* Segment direction */
    f32 seg[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    r_beam_normalize(seg);

    /* Mid-point to camera */
    f32 mid[3] = {
        (p0[0] + p1[0]) * 0.5f,
        (p0[1] + p1[1]) * 0.5f,
        (p0[2] + p1[2]) * 0.5f
    };
    f32 to_cam[3] = {
        cam_pos[0] - mid[0],
        cam_pos[1] - mid[1],
        cam_pos[2] - mid[2]
    };
    r_beam_normalize(to_cam);

    /* Right = cross(seg, to_cam) */
    f32 right[3];
    r_beam_cross(right, seg, to_cam);
    r_beam_normalize(right);

    /* Four corners: p0 +/- right*half_width, p1 +/- right*half_width */
    f32 c0[3], c1[3], c2[3], c3[3];
    for (int i = 0; i < 3; i++) {
        c0[i] = p0[i] - right[i] * half_width;
        c1[i] = p0[i] + right[i] * half_width;
        c2[i] = p1[i] + right[i] * half_width;
        c3[i] = p1[i] - right[i] * half_width;
    }

    /* Triangle 1: c0-c1-c2 */
    memcpy(verts[offset + 0].position, c0, sizeof(f32) * 3);
    memcpy(verts[offset + 0].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 1].position, c1, sizeof(f32) * 3);
    memcpy(verts[offset + 1].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 2].position, c2, sizeof(f32) * 3);
    memcpy(verts[offset + 2].color, color, sizeof(f32) * 4);

    /* Triangle 2: c0-c2-c3 */
    memcpy(verts[offset + 3].position, c0, sizeof(f32) * 3);
    memcpy(verts[offset + 3].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 4].position, c2, sizeof(f32) * 3);
    memcpy(verts[offset + 4].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 5].position, c3, sizeof(f32) * 3);
    memcpy(verts[offset + 5].color, color, sizeof(f32) * 4);

    return 6;
}

/* ---- Init / Shutdown ---- */

qk_result_t r_beam_init(void)
{
    memset(&g_r.beams, 0, sizeof(g_r.beams));

    VkDeviceSize buf_size = R_BEAM_MAX_VERTICES * sizeof(r_beam_vertex_t);

    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        qk_result_t res = r_memory_create_buffer(
            buf_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &g_r.beams.vertex_buffers[i], &g_r.beams.vertex_memories[i]);
        if (res != QK_SUCCESS) return res;

        vkMapMemory(g_r.device.handle, g_r.beams.vertex_memories[i], 0, buf_size, 0,
                     &g_r.beams.vertex_mapped[i]);
    }

    g_r.beams.initialized = true;
    fprintf(stderr, "[Renderer] Beam effects initialized\n");

    return QK_SUCCESS;
}

void r_beam_shutdown(void)
{
    VkDevice dev = g_r.device.handle;

    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        if (g_r.beams.vertex_mapped[i]) {
            vkUnmapMemory(dev, g_r.beams.vertex_memories[i]);
        }
        if (g_r.beams.vertex_buffers[i]) vkDestroyBuffer(dev, g_r.beams.vertex_buffers[i], NULL);
        if (g_r.beams.vertex_memories[i]) vkFreeMemory(dev, g_r.beams.vertex_memories[i], NULL);
    }

    memset(&g_r.beams, 0, sizeof(g_r.beams));
}

/* ---- Rail Beam ---- */

void qk_renderer_draw_rail_beam(f32 start_x, f32 start_y, f32 start_z,
                                 f32 end_x, f32 end_y, f32 end_z,
                                 f32 age_seconds, u32 color_rgba)
{
    if (!g_r.beams.initialized) return;
    if (g_r.beams.draw_count >= R_BEAM_MAX_DRAWS) return;
    if (age_seconds >= RAIL_FADE_TIME) return;

    f32 start[3] = { start_x, start_y, start_z };
    f32 end[3] = { end_x, end_y, end_z };

    /* Beam axis */
    f32 axis[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
    f32 beam_len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (beam_len < 1e-3f) return;

    f32 inv_len = 1.0f / beam_len;
    f32 axis_n[3] = { axis[0] * inv_len, axis[1] * inv_len, axis[2] * inv_len };

    /* Perpendicular vectors for spiral */
    f32 perp1[3], perp2[3];
    {
        f32 up[3] = { 0.0f, 0.0f, 1.0f };
        if (fabsf(r_beam_dot(axis_n, up)) > 0.9f) {
            up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f;
        }
        r_beam_cross(perp1, axis_n, up);
        r_beam_normalize(perp1);
        r_beam_cross(perp2, axis_n, perp1);
        r_beam_normalize(perp2);
    }

    /* Fade and age-based expansion */
    f32 fade = 1.0f - (age_seconds / RAIL_FADE_TIME);
    fade = fade * fade; /* quadratic falloff for nicer look */
    f32 spiral_expand = 1.0f + age_seconds * 2.0f;

    /* Base color from u32 RGBA */
    f32 base_r = (f32)((color_rgba >> 24) & 0xFF) / 255.0f;
    f32 base_g = (f32)((color_rgba >> 16) & 0xFF) / 255.0f;
    f32 base_b = (f32)((color_rgba >>  8) & 0xFF) / 255.0f;

    /* Camera position for billboarding */
    u32 fi = g_r.frame_index % R_FRAMES_IN_FLIGHT;
    r_view_uniforms_t *view = (r_view_uniforms_t *)g_r.frames[fi].view_ubo_mapped;
    f32 cam_pos[3] = { 0.0f, 0.0f, 0.0f };
    if (view) {
        cam_pos[0] = view->camera_pos[0];
        cam_pos[1] = view->camera_pos[1];
        cam_pos[2] = view->camera_pos[2];
    }

    u32 v_start = g_r.beams.vertex_count;
    u32 v_count = 0;
    r_beam_vertex_t *verts = g_r.beams.vertices;

    /* 1. Core beam: thin strip down the center */
    {
        u32 core_segments = 8;
        f32 core_color[4] = {
            base_r * fade * 1.5f,
            base_g * fade * 1.5f,
            base_b * fade * 1.5f,
            fade
        };

        for (u32 i = 0; i < core_segments; i++) {
            f32 t0 = (f32)i / (f32)core_segments;
            f32 t1 = (f32)(i + 1) / (f32)core_segments;

            f32 p0[3], p1[3];
            for (int c = 0; c < 3; c++) {
                p0[c] = start[c] + axis[c] * t0;
                p1[c] = start[c] + axis[c] * t1;
            }

            u32 emitted = r_beam_emit_strip_quad(verts, v_start + v_count,
                                                  p0, p1, RAIL_CORE_HALF_WIDTH * fade,
                                                  cam_pos, core_color);
            v_count += emitted;
        }
    }

    /* 2. Spiral particles - count scales linearly with beam length */
    {
        f32 spiral_turns = beam_len / RAIL_SPIRAL_PITCH;
        u32 spiral_points = (u32)(spiral_turns * RAIL_SPIRAL_PTS_PER_TURN);
        if (spiral_points < RAIL_SPIRAL_MIN_POINTS) spiral_points = RAIL_SPIRAL_MIN_POINTS;
        if (spiral_points > RAIL_SPIRAL_MAX_POINTS) spiral_points = RAIL_SPIRAL_MAX_POINTS;

        f32 spiral_radius = RAIL_SPIRAL_RADIUS * spiral_expand;

        /* Fizzle factor: 0 at birth, ramps quadratically toward 1 at death */
        f32 fizzle = age_seconds / RAIL_FADE_TIME;
        fizzle = fizzle * fizzle;

        for (u32 i = 0; i < spiral_points; i++) {
            f32 t = (f32)i / (f32)(spiral_points - 1);
            f32 angle = t * spiral_turns * 2.0f * PI;

            f32 cx = cosf(angle) * spiral_radius;
            f32 cy = sinf(angle) * spiral_radius;

            f32 center[3];
            for (int c = 0; c < 3; c++) {
                center[c] = start[c] + axis[c] * t
                           + perp1[c] * cx
                           + perp2[c] * cy;
            }

            /* Fizzle: particles drift outward in random directions as beam dies.
             * Each particle gets a fixed random drift direction; magnitude grows
             * with fizzle. A subtle wobble adds organic irregularity. */
            f32 fizzle_mag = RAIL_FIZZLE_AMPLITUDE * fizzle;
            f32 drift_r1 = r_beam_hash(i * 127 + 31) * 2.0f - 1.0f;
            f32 drift_r2 = r_beam_hash(i * 127 + 4999) * 2.0f - 1.0f;
            f32 drift_ax = r_beam_hash(i * 127 + 9973) * 2.0f - 1.0f;
            f32 wobble = sinf(age_seconds * 5.0f + (f32)i * 1.618f) * 0.3f + 1.0f;
            for (int c = 0; c < 3; c++) {
                center[c] += (perp1[c] * drift_r1 + perp2[c] * drift_r2) * fizzle_mag * wobble
                           + axis_n[c] * drift_ax * fizzle_mag * 0.3f;
            }

            /* Particle grows slightly as it disperses */
            f32 fizzle_size = 1.0f + fizzle * 0.6f;
            f32 particle_size = RAIL_PARTICLE_HALF_SIZE * fade * fizzle_size;

            /* Sparkle + wispy fade: some particles dim faster for a wispy look */
            f32 sparkle = 0.7f + 0.3f * r_beam_hash(i * 7 + (u32)(age_seconds * 60.0f));
            f32 wisp = 1.0f - fizzle * 0.4f * r_beam_hash(i * 31 + 7777);
            f32 particle_color[4] = {
                base_r * fade * sparkle * wisp,
                base_g * fade * sparkle * wisp,
                base_b * fade * sparkle * wisp,
                fade * sparkle * wisp
            };

            u32 emitted = r_beam_emit_billboard_quad(verts, v_start + v_count,
                                                      center, particle_size,
                                                      cam_pos, axis_n,
                                                      particle_color);
            v_count += emitted;
        }
    }

    if (v_count == 0) return;

    r_beam_draw_t *draw = &g_r.beams.draws[g_r.beams.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.beams.vertex_count = v_start + v_count;
}

/* ---- Lightning Gun Beam ---- */

void qk_renderer_draw_lg_beam(f32 start_x, f32 start_y, f32 start_z,
                                f32 end_x, f32 end_y, f32 end_z,
                                f32 time_seconds)
{
    if (!g_r.beams.initialized) return;
    if (g_r.beams.draw_count >= R_BEAM_MAX_DRAWS) return;

    f32 start[3] = { start_x, start_y, start_z };
    f32 end[3] = { end_x, end_y, end_z };

    f32 axis[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
    f32 beam_len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (beam_len < 1e-3f) return;

    f32 inv_len = 1.0f / beam_len;
    f32 axis_n[3] = { axis[0] * inv_len, axis[1] * inv_len, axis[2] * inv_len };

    /* Perpendicular vectors for displacement */
    f32 perp1[3], perp2[3];
    {
        f32 up[3] = { 0.0f, 0.0f, 1.0f };
        if (fabsf(r_beam_dot(axis_n, up)) > 0.9f) {
            up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f;
        }
        r_beam_cross(perp1, axis_n, up);
        r_beam_normalize(perp1);
        r_beam_cross(perp2, axis_n, perp1);
        r_beam_normalize(perp2);
    }

    u32 fi = g_r.frame_index % R_FRAMES_IN_FLIGHT;
    r_view_uniforms_t *view = (r_view_uniforms_t *)g_r.frames[fi].view_ubo_mapped;
    f32 cam_pos[3] = { 0.0f, 0.0f, 0.0f };
    if (view) {
        cam_pos[0] = view->camera_pos[0];
        cam_pos[1] = view->camera_pos[1];
        cam_pos[2] = view->camera_pos[2];
    }

    u32 v_start = g_r.beams.vertex_count;
    u32 v_count = 0;
    r_beam_vertex_t *verts = g_r.beams.vertices;

    /* Generate multiple overlapping beam layers with different wobble phases */
    for (u32 layer = 0; layer < LG_BEAM_LAYERS; layer++) {
        f32 phase_offset = (f32)layer * 2.094f; /* ~120 degrees apart */
        f32 layer_amplitude = LG_WOBBLE_AMPLITUDE * (1.0f - (f32)layer * 0.25f);

        /* Interpolate width: inner layer = thin bright core, outer = wider softer */
        f32 half_width;
        f32 brightness;
        if (layer == 0) {
            half_width = LG_CORE_HALF_WIDTH;
            brightness = 1.0f;
        } else {
            half_width = LG_OUTER_HALF_WIDTH * (0.5f + 0.5f * (f32)layer / (f32)(LG_BEAM_LAYERS - 1));
            brightness = 0.3f / (f32)layer;
        }

        /* Electric blue-white core color */
        f32 color[4] = {
            0.6f * brightness,
            0.8f * brightness,
            1.0f * brightness,
            brightness
        };

        /* Generate displaced points along beam */
        f32 points[LG_SEGMENTS + 1][3];
        for (u32 s = 0; s <= LG_SEGMENTS; s++) {
            f32 t = (f32)s / (f32)LG_SEGMENTS;

            /* Noise-based displacement using hash for jitter */
            u32 seed = s * 131 + layer * 37 + (u32)(time_seconds * LG_WOBBLE_FREQ);
            f32 disp1 = (r_beam_hash(seed) * 2.0f - 1.0f) * layer_amplitude;
            f32 disp2 = (r_beam_hash(seed + 9973) * 2.0f - 1.0f) * layer_amplitude;

            /* Add sinusoidal component for smoother overall shape */
            disp1 += sinf(t * 8.0f * PI + time_seconds * LG_WOBBLE_FREQ + phase_offset) * layer_amplitude * 0.5f;
            disp2 += cosf(t * 6.0f * PI + time_seconds * LG_WOBBLE_FREQ * 1.3f + phase_offset) * layer_amplitude * 0.5f;

            /* Taper displacement at endpoints so it connects cleanly */
            f32 taper = sinf(t * PI);
            disp1 *= taper;
            disp2 *= taper;

            for (int c = 0; c < 3; c++) {
                points[s][c] = start[c] + axis[c] * t
                             + perp1[c] * disp1
                             + perp2[c] * disp2;
            }
        }

        /* Emit quad strips connecting consecutive points */
        for (u32 s = 0; s < LG_SEGMENTS; s++) {
            u32 emitted = r_beam_emit_strip_quad(verts, v_start + v_count,
                                                  points[s], points[s + 1],
                                                  half_width, cam_pos, color);
            v_count += emitted;
        }
    }

    if (v_count == 0) return;

    r_beam_draw_t *draw = &g_r.beams.draws[g_r.beams.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.beams.vertex_count = v_start + v_count;
}

/* ---- Command Recording ---- */

void r_beam_record_commands(VkCommandBuffer cmd, u32 frame_index)
{
    if (!g_r.beam_pipeline.handle || !g_r.beams.initialized) return;
    if (g_r.beams.draw_count == 0) return;

    /* Upload vertices to this frame's mapped buffer */
    void *dst = g_r.beams.vertex_mapped[frame_index];
    if (!dst) return;
    memcpy(dst, g_r.beams.vertices,
           g_r.beams.vertex_count * sizeof(r_beam_vertex_t));

    r_frame_data_t *frame = &g_r.frames[frame_index];

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_r.beam_pipeline.handle);

    VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (f32)g_r.config.render_width,
        .height   = (f32)g_r.config.render_height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = { g_r.config.render_width, g_r.config.render_height }
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    /* Bind view UBO (set 0) */
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.beam_pipeline.layout, 0, 1,
                            &frame->view_descriptor_set, 0, NULL);

    /* Bind vertex buffer */
    VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_r.beams.vertex_buffers[frame_index], &vb_offset);

    /* Draw all beam batches */
    for (u32 i = 0; i < g_r.beams.draw_count; i++) {
        r_beam_draw_t *draw = &g_r.beams.draws[i];
        vkCmdDraw(cmd, draw->vertex_count, 1, draw->vertex_offset, 0);
        g_r.stats_draw_calls++;
        g_r.stats_triangles += draw->vertex_count / 3;
    }
}
