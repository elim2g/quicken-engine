/*
 * QUICKEN Renderer - Visual Effects (beams, trails, particles, explosions)
 *
 * All transient visual effects share a single dynamic vertex buffer
 * (host-visible, mapped persistently) rebuilt every frame. Vertices are
 * generated CPU-side. Additive blending, depth-test ON, depth-write OFF.
 *
 * Effects: rail beam, LG beam, rocket smoke trail, smoke puff batch,
 * rocket explosion, railgun impact sparks.
 */

#include "r_types.h"
#include "renderer/qk_renderer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- Constants ---

static const f32 PI = 3.14159265358979f;

/* Hash channel offsets for r_fx_hash().
 *
 * Particle effects need multiple independent random values per particle
 * (position, size, rotation, color, etc.). r_fx_hash(seed) returns a
 * deterministic pseudo-random float in [0,1]. To decorrelate different
 * properties, each one uses a unique channel offset added to the seed:
 *
 *     r_fx_hash(particle_index * stride + FX_CH_xxx + extra_seed)
 *
 * `stride` is any small prime that spreads sequential particles apart.
 * `FX_CH_xxx` separates properties so (e.g.) X-jitter and Y-jitter
 * don't produce the same value for the same particle. */
#define FX_CH_X         31      // spatial X / primary displacement
#define FX_CH_Y         4999    // spatial Y / secondary displacement
#define FX_CH_Z         9973    // spatial Z / tertiary displacement
#define FX_CH_SIZE      113     // per-particle size variation
#define FX_CH_ROTATION  1993    // billboard rotation angle
#define FX_CH_SPEED     73      // velocity / speed variation
#define FX_CH_HUE       997     // color / hue shift
#define FX_CH_SPREAD    97      // directional spread
#define FX_CH_SPARKLE   7777    // time-varying brightness
#define FX_CH_WISP      3331    // per-particle fade rate
#define FX_CH_DRIFT     53      // drift rate variation
#define FX_CH_AZIMUTH   17      // angular distribution
#define FX_CH_ELEV      41      // elevation / polar angle

// Rail beam parameters
#define RAIL_CORE_HALF_WIDTH    0.8f
#define RAIL_SPIRAL_PITCH       40.0f   // world units per full spiral revolution
#define RAIL_SPIRAL_PTS_PER_TURN 28     // particles per revolution
#define RAIL_SPIRAL_MIN_POINTS  12
#define RAIL_SPIRAL_MAX_POINTS  512
#define RAIL_SPIRAL_RADIUS      2.5f
#define RAIL_PARTICLE_HALF_SIZE 1.2f
#define RAIL_FADE_TIME          1.75f
#define RAIL_FIZZLE_AMPLITUDE   6.0f    // max displacement at end of life

// LG beam parameters
#define LG_SEGMENTS             24
#define LG_BEAM_LAYERS          3
#define LG_CORE_HALF_WIDTH      0.6f
#define LG_OUTER_HALF_WIDTH     1.8f
#define LG_WOBBLE_AMPLITUDE     5.0f
#define LG_WOBBLE_FREQ          10.0f

// --- Helpers ---

static void r_fx_cross(f32 *out, const f32 *a, const f32 *b)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static f32 r_fx_dot(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void r_fx_normalize(f32 *v)
{
    f32 len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) {
        f32 inv = 1.0f / len;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

static f32 r_fx_hash(u32 x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = (x >> 16) ^ x;
    return (f32)(x & 0xFFFF) / 65535.0f;
}

// Signed variant: maps to [-1, 1] instead of [0, 1]
static f32 r_fx_hash_signed(u32 x)
{
    return r_fx_hash(x) * 2.0f - 1.0f;
}

/* Fast sine/cosine for visual-only rotation angles (particle rotation).
 * Parabolic approximation with one refinement step, ~0.001 max error.
 * Sufficient for particle billboards where exact precision is invisible. */
static void r_fx_fast_sincos(f32 angle, f32 *out_sin, f32 *out_cos)
{
    // Reduce to [-PI, PI]
    angle += PI;
    angle -= floorf(angle / (2.0f * PI)) * (2.0f * PI);
    angle -= PI;

    // sin via parabolic approximation + refinement
    f32 s = (4.0f / PI) * angle - (4.0f / (PI * PI)) * angle * fabsf(angle);
    *out_sin = 0.225f * (s * fabsf(s) - s) + s;

    // cos = sin(angle + PI/2)
    f32 ca = angle + PI * 0.5f;
    if (ca > PI) ca -= 2.0f * PI;
    f32 c = (4.0f / PI) * ca - (4.0f / (PI * PI)) * ca * fabsf(ca);
    *out_cos = 0.225f * (c * fabsf(c) - c) + c;
}

/* Fetch camera position from the current frame's view UBO */
static void r_fx_get_cam_pos(f32 *out)
{
    u32 fi = g_r.frame_index % R_FRAMES_IN_FLIGHT;
    r_view_uniforms_t *view = (r_view_uniforms_t *)g_r.frames[fi].view_ubo_mapped;
    if (view) {
        out[0] = view->camera_pos[0];
        out[1] = view->camera_pos[1];
        out[2] = view->camera_pos[2];
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
}

/* Build two perpendicular vectors from a normalized axis.
 * Falls back to X-up when axis is nearly vertical. */
static void r_fx_build_perp_frame(const f32 *axis, f32 *perp1, f32 *perp2)
{
    f32 up[3] = { 0.0f, 0.0f, 1.0f };
    if (fabsf(r_fx_dot(axis, up)) > 0.9f) {
        up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f;
    }
    r_fx_cross(perp1, axis, up);
    r_fx_normalize(perp1);
    r_fx_cross(perp2, axis, perp1);
    r_fx_normalize(perp2);
}

/* Build camera-facing billboard frame from an effect origin.
 * Returns cam_right and cam_up suitable for screen-facing quads. */
static void r_fx_build_cam_frame(const f32 *origin, const f32 *cam_pos,
                                  f32 *cam_right, f32 *cam_up)
{
    f32 cam_fwd[3] = {
        cam_pos[0] - origin[0],
        cam_pos[1] - origin[1],
        cam_pos[2] - origin[2]
    };
    r_fx_normalize(cam_fwd);

    f32 world_up[3] = { 0.0f, 0.0f, 1.0f };
    if (fabsf(cam_fwd[2]) > 0.99f) {
        world_up[0] = 1.0f; world_up[1] = 0.0f; world_up[2] = 0.0f;
    }
    r_fx_cross(cam_right, world_up, cam_fwd);
    r_fx_normalize(cam_right);
    r_fx_cross(cam_up, cam_fwd, cam_right);
    r_fx_normalize(cam_up);
}

/* Emit a camera-facing quad (two triangles, 6 vertices) centered at `center`
 * with half-extent `half_size` and the given color. The quad faces the camera. */
static u32 r_fx_emit_billboard_quad(r_fx_vertex_t *verts, u32 offset,
                                       const f32 *center, f32 half_size,
                                       const f32 *cam_pos, const f32 *beam_axis,
                                       const f32 *color)
{
    if (offset + 6 > R_FX_MAX_VERTICES) return 0;

    // Billboard direction: camera to center
    f32 to_cam[3] = {
        cam_pos[0] - center[0],
        cam_pos[1] - center[1],
        cam_pos[2] - center[2]
    };
    r_fx_normalize(to_cam);

    // Right vector = cross(beam_axis, to_cam)
    f32 right[3];
    r_fx_cross(right, beam_axis, to_cam);
    r_fx_normalize(right);

    // Up vector = cross(right, beam_axis)
    f32 up[3];
    r_fx_cross(up, right, beam_axis);
    r_fx_normalize(up);

    // Quad corners
    f32 corners[4][3];
    for (int i = 0; i < 3; i++) {
        corners[0][i] = center[i] - right[i] * half_size - up[i] * half_size;
        corners[1][i] = center[i] + right[i] * half_size - up[i] * half_size;
        corners[2][i] = center[i] + right[i] * half_size + up[i] * half_size;
        corners[3][i] = center[i] - right[i] * half_size + up[i] * half_size;
    }

    // Triangle 1: 0-1-2
    memcpy(verts[offset + 0].position, corners[0], sizeof(f32) * 3);
    memcpy(verts[offset + 0].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 1].position, corners[1], sizeof(f32) * 3);
    memcpy(verts[offset + 1].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 2].position, corners[2], sizeof(f32) * 3);
    memcpy(verts[offset + 2].color, color, sizeof(f32) * 4);

    // Triangle 2: 0-2-3
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
static u32 r_fx_emit_strip_quad(r_fx_vertex_t *verts, u32 offset,
                                   const f32 *p0, const f32 *p1,
                                   f32 half_width, const f32 *cam_pos,
                                   const f32 *color)
{
    if (offset + 6 > R_FX_MAX_VERTICES) return 0;

    // Segment direction
    f32 seg[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    r_fx_normalize(seg);

    // Mid-point to camera
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
    r_fx_normalize(to_cam);

    // Right = cross(seg, to_cam)
    f32 right[3];
    r_fx_cross(right, seg, to_cam);
    r_fx_normalize(right);

    // Four corners: p0 +/- right*half_width, p1 +/- right*half_width
    f32 c0[3], c1[3], c2[3], c3[3];
    for (int i = 0; i < 3; i++) {
        c0[i] = p0[i] - right[i] * half_width;
        c1[i] = p0[i] + right[i] * half_width;
        c2[i] = p1[i] + right[i] * half_width;
        c3[i] = p1[i] - right[i] * half_width;
    }

    // Triangle 1: c0-c1-c2
    memcpy(verts[offset + 0].position, c0, sizeof(f32) * 3);
    memcpy(verts[offset + 0].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 1].position, c1, sizeof(f32) * 3);
    memcpy(verts[offset + 1].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 2].position, c2, sizeof(f32) * 3);
    memcpy(verts[offset + 2].color, color, sizeof(f32) * 4);

    // Triangle 2: c0-c2-c3
    memcpy(verts[offset + 3].position, c0, sizeof(f32) * 3);
    memcpy(verts[offset + 3].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 4].position, c2, sizeof(f32) * 3);
    memcpy(verts[offset + 4].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 5].position, c3, sizeof(f32) * 3);
    memcpy(verts[offset + 5].color, color, sizeof(f32) * 4);

    return 6;
}

// --- Init / Shutdown ---

qk_result_t r_fx_init(void)
{
    memset(&g_r.fx, 0, sizeof(g_r.fx));

    VkDeviceSize buf_size = R_FX_MAX_VERTICES * sizeof(r_fx_vertex_t);

    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        qk_result_t res = r_memory_create_buffer(
            buf_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &g_r.fx.vertex_buffers[i], &g_r.fx.vertex_memories[i]);
        if (res != QK_SUCCESS) return res;

        vkMapMemory(g_r.device.handle, g_r.fx.vertex_memories[i], 0, buf_size, 0,
                     &g_r.fx.vertex_mapped[i]);
    }

    g_r.fx.initialized = true;
    fprintf(stderr, "[Renderer] FX effects initialized\n");

    return QK_SUCCESS;
}

void r_fx_shutdown(void)
{
    VkDevice dev = g_r.device.handle;

    for (u32 i = 0; i < R_FRAMES_IN_FLIGHT; i++) {
        if (g_r.fx.vertex_mapped[i]) {
            vkUnmapMemory(dev, g_r.fx.vertex_memories[i]);
        }
        if (g_r.fx.vertex_buffers[i]) vkDestroyBuffer(dev, g_r.fx.vertex_buffers[i], NULL);
        if (g_r.fx.vertex_memories[i]) vkFreeMemory(dev, g_r.fx.vertex_memories[i], NULL);
    }

    memset(&g_r.fx, 0, sizeof(g_r.fx));
}

// --- Rail Beam ---

void qk_renderer_draw_rail_beam(f32 start_x, f32 start_y, f32 start_z,
                                 f32 end_x, f32 end_y, f32 end_z,
                                 f32 age_seconds, u32 color_rgba)
{
    if (!g_r.fx.initialized) return;
    if (g_r.fx.draw_count >= R_FX_MAX_DRAWS) return;
    if (age_seconds >= RAIL_FADE_TIME) return;

    f32 start[3] = { start_x, start_y, start_z };
    f32 end[3] = { end_x, end_y, end_z };

    // Beam axis
    f32 axis[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
    f32 beam_len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (beam_len < 1e-3f) return;

    f32 inv_len = 1.0f / beam_len;
    f32 axis_n[3] = { axis[0] * inv_len, axis[1] * inv_len, axis[2] * inv_len };

    // Perpendicular vectors for spiral
    f32 perp1[3], perp2[3];
    r_fx_build_perp_frame(axis_n, perp1, perp2);

    // Fade and age-based expansion
    f32 fade = 1.0f - (age_seconds / RAIL_FADE_TIME);
    fade = fade * fade; // quadratic falloff for nicer look
    f32 spiral_expand = 1.0f + age_seconds * 2.0f;

    // Base color from u32 RGBA
    f32 base_r = (f32)((color_rgba >> 24) & 0xFF) / 255.0f;
    f32 base_g = (f32)((color_rgba >> 16) & 0xFF) / 255.0f;
    f32 base_b = (f32)((color_rgba >>  8) & 0xFF) / 255.0f;

    f32 cam_pos[3];
    r_fx_get_cam_pos(cam_pos);

    u32 v_start = g_r.fx.vertex_count;
    u32 v_count = 0;
    r_fx_vertex_t *verts = g_r.fx.vertices;

    // 1. Core beam: thin strip down the center
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

            u32 emitted = r_fx_emit_strip_quad(verts, v_start + v_count,
                                                  p0, p1, RAIL_CORE_HALF_WIDTH * fade,
                                                  cam_pos, core_color);
            v_count += emitted;
        }
    }

    // 2. Spiral particles - count scales linearly with beam length
    {
        f32 spiral_turns = beam_len / RAIL_SPIRAL_PITCH;
        u32 spiral_points = (u32)(spiral_turns * RAIL_SPIRAL_PTS_PER_TURN);
        if (spiral_points < RAIL_SPIRAL_MIN_POINTS) spiral_points = RAIL_SPIRAL_MIN_POINTS;
        if (spiral_points > RAIL_SPIRAL_MAX_POINTS) spiral_points = RAIL_SPIRAL_MAX_POINTS;

        f32 spiral_radius = RAIL_SPIRAL_RADIUS * spiral_expand;

        // Fizzle factor: 0 at birth, ramps quadratically toward 1 at death
        f32 fizzle = age_seconds / RAIL_FADE_TIME;
        fizzle = fizzle * fizzle;

        /* Incremental sin/cos rotation: compute step once, then rotate
         * each iteration via addition formulas. Eliminates ~1000 trig calls
         * per rail beam at max spiral points. */
        f32 total_angle = spiral_turns * 2.0f * PI;
        f32 angle_step = total_angle / (f32)(spiral_points - 1);
        f32 step_cos = cosf(angle_step);
        f32 step_sin = sinf(angle_step);
        f32 cur_cos = 1.0f;  // cos(0)
        f32 cur_sin = 0.0f;  // sin(0)

        for (u32 i = 0; i < spiral_points; i++) {
            f32 t = (f32)i / (f32)(spiral_points - 1);

            f32 cx = cur_cos * spiral_radius;
            f32 cy = cur_sin * spiral_radius;

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
            f32 drift_r1 = r_fx_hash_signed(i * 127 + FX_CH_X);
            f32 drift_r2 = r_fx_hash_signed(i * 127 + FX_CH_Y);
            f32 drift_ax = r_fx_hash_signed(i * 127 + FX_CH_Z);
            f32 wobble = sinf(age_seconds * 5.0f + (f32)i * 1.618f) * 0.3f + 1.0f;
            for (int c = 0; c < 3; c++) {
                center[c] += (perp1[c] * drift_r1 + perp2[c] * drift_r2) * fizzle_mag * wobble
                           + axis_n[c] * drift_ax * fizzle_mag * 0.3f;
            }

            // Particle grows slightly as it disperses
            f32 fizzle_size = 1.0f + fizzle * 0.6f;
            f32 particle_size = RAIL_PARTICLE_HALF_SIZE * fade * fizzle_size;

            // Sparkle + wispy fade: some particles dim faster for a wispy look
            f32 sparkle = 0.7f + 0.3f * r_fx_hash(i * 7 + FX_CH_SPARKLE + (u32)(age_seconds * 60.0f));
            f32 wisp = 1.0f - fizzle * 0.4f * r_fx_hash(i * 31 + FX_CH_WISP);
            f32 particle_color[4] = {
                base_r * fade * sparkle * wisp,
                base_g * fade * sparkle * wisp,
                base_b * fade * sparkle * wisp,
                fade * sparkle * wisp
            };

            u32 emitted = r_fx_emit_billboard_quad(verts, v_start + v_count,
                                                      center, particle_size,
                                                      cam_pos, axis_n,
                                                      particle_color);
            v_count += emitted;

            // Rotate to next angle: (cos,sin)(a+s) = (cos*cs - sin*ss, sin*cs + cos*ss)
            f32 new_cos = cur_cos * step_cos - cur_sin * step_sin;
            f32 new_sin = cur_sin * step_cos + cur_cos * step_sin;
            cur_cos = new_cos;
            cur_sin = new_sin;
        }
    }

    if (v_count == 0) return;

    r_fx_draw_t *draw = &g_r.fx.draws[g_r.fx.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.fx.vertex_count = v_start + v_count;
}

// --- Lightning Gun Beam ---

void qk_renderer_draw_lg_beam(f32 start_x, f32 start_y, f32 start_z,
                                f32 end_x, f32 end_y, f32 end_z,
                                f32 time_seconds)
{
    if (!g_r.fx.initialized) return;
    if (g_r.fx.draw_count >= R_FX_MAX_DRAWS) return;

    f32 start[3] = { start_x, start_y, start_z };
    f32 end[3] = { end_x, end_y, end_z };

    f32 axis[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
    f32 beam_len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (beam_len < 1e-3f) return;

    f32 inv_len = 1.0f / beam_len;
    f32 axis_n[3] = { axis[0] * inv_len, axis[1] * inv_len, axis[2] * inv_len };

    /* Auto-emit dynamic light near beam endpoint, pulled back along beam
       so the light isn't inside the surface the beam is hitting. */
    {
        const f32 LIGHT_OFFSET = 8.0f;
        qk_dynamic_light_t light = {
            .position  = { end_x - axis_n[0] * LIGHT_OFFSET,
                           end_y - axis_n[1] * LIGHT_OFFSET,
                           end_z - axis_n[2] * LIGHT_OFFSET },
            .radius    = 100.0f,
            .color     = { 0.6f, 0.8f, 1.0f },
            .intensity = 1.2f
        };
        qk_renderer_submit_light(&light);
    }

    // Perpendicular vectors for displacement
    f32 perp1[3], perp2[3];
    r_fx_build_perp_frame(axis_n, perp1, perp2);

    f32 cam_pos[3];
    r_fx_get_cam_pos(cam_pos);

    u32 v_start = g_r.fx.vertex_count;
    u32 v_count = 0;
    r_fx_vertex_t *verts = g_r.fx.vertices;

    // Generate multiple overlapping beam layers with different wobble phases
    for (u32 layer = 0; layer < LG_BEAM_LAYERS; layer++) {
        f32 phase_offset = (f32)layer * (2.0f * PI / 3.0f); // 120 degrees apart
        f32 layer_amplitude = LG_WOBBLE_AMPLITUDE * (1.0f - (f32)layer * 0.25f);

        // Interpolate width: inner layer = thin bright core, outer = wider softer
        f32 half_width;
        f32 brightness;
        if (layer == 0) {
            half_width = LG_CORE_HALF_WIDTH;
            brightness = 1.0f;
        } else {
            half_width = LG_OUTER_HALF_WIDTH * (0.5f + 0.5f * (f32)layer / (f32)(LG_BEAM_LAYERS - 1));
            brightness = 0.3f / (f32)layer;
        }

        // Electric blue-white core color
        f32 color[4] = {
            0.6f * brightness,
            0.8f * brightness,
            1.0f * brightness,
            brightness
        };

        // Generate displaced points along beam
        f32 points[LG_SEGMENTS + 1][3];
        for (u32 s = 0; s <= LG_SEGMENTS; s++) {
            f32 t = (f32)s / (f32)LG_SEGMENTS;

            // Noise-based displacement using hash for jitter
            u32 seed = s * 131 + layer * 37 + (u32)(time_seconds * LG_WOBBLE_FREQ);
            f32 disp1 = r_fx_hash_signed(seed + FX_CH_X) * layer_amplitude;
            f32 disp2 = r_fx_hash_signed(seed + FX_CH_Y) * layer_amplitude;

            // Add sinusoidal component for smoother overall shape
            disp1 += sinf(t * 8.0f * PI + time_seconds * LG_WOBBLE_FREQ + phase_offset) * layer_amplitude * 0.5f;
            disp2 += cosf(t * 6.0f * PI + time_seconds * LG_WOBBLE_FREQ * 1.3f + phase_offset) * layer_amplitude * 0.5f;

            // Taper displacement at endpoints so it connects cleanly
            f32 taper = sinf(t * PI);
            disp1 *= taper;
            disp2 *= taper;

            for (int c = 0; c < 3; c++) {
                points[s][c] = start[c] + axis[c] * t
                             + perp1[c] * disp1
                             + perp2[c] * disp2;
            }
        }

        // Emit quad strips connecting consecutive points
        for (u32 s = 0; s < LG_SEGMENTS; s++) {
            u32 emitted = r_fx_emit_strip_quad(verts, v_start + v_count,
                                                  points[s], points[s + 1],
                                                  half_width, cam_pos, color);
            v_count += emitted;
        }
    }

    if (v_count == 0) return;

    r_fx_draw_t *draw = &g_r.fx.draws[g_r.fx.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.fx.vertex_count = v_start + v_count;
}

// --- Rocket Smoke Trail ---

#define ROCKET_TRAIL_PARTICLES  8.0f
#define ROCKET_TRAIL_LENGTH     20.0f
#define ROCKET_TRAIL_BASE_SIZE  1.25f
#define ROCKET_TRAIL_EXPAND     10.0f

void qk_renderer_draw_rocket_trail(f32 pos_x, f32 pos_y, f32 pos_z,
                                    f32 vel_x, f32 vel_y, f32 vel_z,
                                    f32 age_seconds)
{
    if (!g_r.fx.initialized) return;
    if (g_r.fx.draw_count >= R_FX_MAX_DRAWS) return;

    // Auto-emit dim dynamic light at rocket position
    {
        qk_dynamic_light_t light = {
            .position  = { pos_x, pos_y, pos_z },
            .radius    = 80.0f,
            .color     = { 1.0f, 0.6f, 0.2f },
            .intensity = 0.6f
        };
        qk_renderer_submit_light(&light);
    }

    // Trail direction = opposite of velocity (smoke is behind the rocket)
    f32 vel_len = sqrtf(vel_x * vel_x + vel_y * vel_y + vel_z * vel_z);
    if (vel_len < 1e-3f) return;

    f32 inv_vel = 1.0f / vel_len;
    f32 trail_dir[3] = { -vel_x * inv_vel, -vel_y * inv_vel, -vel_z * inv_vel };

    // Use trail_dir as beam axis for billboarding
    f32 axis_n[3] = { trail_dir[0], trail_dir[1], trail_dir[2] };

    f32 cam_pos[3];
    r_fx_get_cam_pos(cam_pos);

    // Perpendicular vectors for jitter displacement
    f32 perp1[3], perp2[3];
    r_fx_build_perp_frame(axis_n, perp1, perp2);

    u32 v_start = g_r.fx.vertex_count;
    u32 v_count = 0;
    r_fx_vertex_t *verts = g_r.fx.vertices;

    // Hash seed based on age for per-rocket variation
    u32 age_seed = (u32)(age_seconds * 1000.0f);

    for (u32 i = 0; i < ROCKET_TRAIL_PARTICLES; i++) {
        f32 t = (f32)i / (f32)(ROCKET_TRAIL_PARTICLES - 1); // 0 = at rocket, 1 = trail end

        f32 dist = t * ROCKET_TRAIL_LENGTH;

        // Base position along trail
        f32 pos[3] = { pos_x, pos_y, pos_z };
        f32 center[3];
        for (int c = 0; c < 3; c++) {
            center[c] = pos[c] + trail_dir[c] * dist;
        }

        // Random perpendicular jitter for organic look - hash-based, stable per particle
        f32 jitter_scale = 2.0f + t * 4.0f; // more jitter further from rocket
        f32 j1 = r_fx_hash_signed(i * 127 + age_seed + FX_CH_X) * jitter_scale;
        f32 j2 = r_fx_hash_signed(i * 127 + age_seed + FX_CH_Y) * jitter_scale;
        for (int c = 0; c < 3; c++) {
            center[c] += perp1[c] * j1 + perp2[c] * j2;
        }

        // Size: grows with distance from rocket
        f32 half_size = ROCKET_TRAIL_BASE_SIZE + t * ROCKET_TRAIL_EXPAND;

        // Color: smoky grey, fading with distance
        f32 alpha = (1.0f - t) * 0.5f; // more opaque near rocket
        f32 brightness = 0.3f - t * 0.1f; // darker further back
        if (brightness < 0.1f) brightness = 0.1f;

        f32 color[4] = {
            brightness * alpha,
            brightness * alpha,
            brightness * alpha,
            alpha
        };

        u32 emitted = r_fx_emit_billboard_quad(verts, v_start + v_count,
                                                  center, half_size,
                                                  cam_pos, axis_n,
                                                  color);
        v_count += emitted;
    }

    if (v_count == 0) return;

    r_fx_draw_t *draw = &g_r.fx.draws[g_r.fx.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.fx.vertex_count = v_start + v_count;
}

// --- Smoke Particle Batch ---

static u32 s_smoke_batch_v_start;

void qk_renderer_begin_smoke(void)
{
    s_smoke_batch_v_start = g_r.fx.vertex_count;
}

void qk_renderer_emit_smoke_puff(f32 x, f32 y, f32 z,
                                  f32 half_size, u32 color_rgba,
                                  f32 angle_rad)
{
    if (!g_r.fx.initialized) return;
    u32 offset = g_r.fx.vertex_count;
    if (offset + 6 > R_FX_MAX_VERTICES) return;

    f32 cam_pos[3];
    r_fx_get_cam_pos(cam_pos);

    f32 center[3] = { x, y, z };

    // Camera-facing billboard
    f32 to_cam[3] = {
        cam_pos[0] - x,
        cam_pos[1] - y,
        cam_pos[2] - z
    };
    r_fx_normalize(to_cam);

    // Right = world_up x to_cam
    f32 world_up[3] = { 0, 0, 1 };
    f32 right[3];
    r_fx_cross(right, world_up, to_cam);
    r_fx_normalize(right);

    // Up = to_cam x right
    f32 up[3];
    r_fx_cross(up, to_cam, right);
    r_fx_normalize(up);

    // Rotate right/up by angle_rad around the to_cam axis
    f32 sa, ca;
    r_fx_fast_sincos(angle_rad, &sa, &ca);
    f32 rot_right[3], rot_up[3];
    for (int i = 0; i < 3; i++) {
        rot_right[i] = right[i] * ca + up[i] * sa;
        rot_up[i]    = -right[i] * sa + up[i] * ca;
    }

    // Color from RGBA u32
    f32 color[4] = {
        (f32)((color_rgba >> 24) & 0xFF) / 255.0f,
        (f32)((color_rgba >> 16) & 0xFF) / 255.0f,
        (f32)((color_rgba >>  8) & 0xFF) / 255.0f,
        (f32)( color_rgba        & 0xFF) / 255.0f
    };

    // Quad corners (rotated)
    r_fx_vertex_t *verts = g_r.fx.vertices;
    f32 corners[4][3];
    for (int i = 0; i < 3; i++) {
        corners[0][i] = center[i] - rot_right[i] * half_size - rot_up[i] * half_size;
        corners[1][i] = center[i] + rot_right[i] * half_size - rot_up[i] * half_size;
        corners[2][i] = center[i] + rot_right[i] * half_size + rot_up[i] * half_size;
        corners[3][i] = center[i] - rot_right[i] * half_size + rot_up[i] * half_size;
    }

    memcpy(verts[offset + 0].position, corners[0], sizeof(f32) * 3);
    memcpy(verts[offset + 0].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 1].position, corners[1], sizeof(f32) * 3);
    memcpy(verts[offset + 1].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 2].position, corners[2], sizeof(f32) * 3);
    memcpy(verts[offset + 2].color, color, sizeof(f32) * 4);

    memcpy(verts[offset + 3].position, corners[0], sizeof(f32) * 3);
    memcpy(verts[offset + 3].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 4].position, corners[2], sizeof(f32) * 3);
    memcpy(verts[offset + 4].color, color, sizeof(f32) * 4);
    memcpy(verts[offset + 5].position, corners[3], sizeof(f32) * 3);
    memcpy(verts[offset + 5].color, color, sizeof(f32) * 4);

    g_r.fx.vertex_count = offset + 6;
}

void qk_renderer_end_smoke(void)
{
    u32 v_count = g_r.fx.vertex_count - s_smoke_batch_v_start;
    if (v_count == 0) return;
    if (g_r.fx.draw_count >= R_FX_MAX_DRAWS) return;

    r_fx_draw_t *draw = &g_r.fx.draws[g_r.fx.draw_count++];
    draw->vertex_offset = s_smoke_batch_v_start;
    draw->vertex_count = v_count;
}

// --- Screen-facing billboard (fully camera-oriented) ---

static u32 r_fx_emit_screen_facing_quad(r_fx_vertex_t *verts, u32 offset,
                                            const f32 *center, f32 half_size,
                                            const f32 *cam_right,
                                            const f32 *cam_up,
                                            const f32 *color)
{
    if (offset + 6 > R_FX_MAX_VERTICES) return 0;

    f32 corners[4][3];
    for (int i = 0; i < 3; i++) {
        corners[0][i] = center[i] - cam_right[i] * half_size - cam_up[i] * half_size;
        corners[1][i] = center[i] + cam_right[i] * half_size - cam_up[i] * half_size;
        corners[2][i] = center[i] + cam_right[i] * half_size + cam_up[i] * half_size;
        corners[3][i] = center[i] - cam_right[i] * half_size + cam_up[i] * half_size;
    }

    // Two triangles: 0-1-2, 0-2-3
    r_fx_vertex_t v;
    v.color[0] = color[0]; v.color[1] = color[1];
    v.color[2] = color[2]; v.color[3] = color[3];

    #define SF_VERT(idx) \
        v.position[0] = corners[idx][0]; \
        v.position[1] = corners[idx][1]; \
        v.position[2] = corners[idx][2]; \
        verts[offset++] = v;

    SF_VERT(0); SF_VERT(1); SF_VERT(2);
    SF_VERT(0); SF_VERT(2); SF_VERT(3);
    #undef SF_VERT

    return 6;
}

// --- Explosion Effect ---

#define EXPLOSION_OUTER_COUNT       60      // outer sphere particles
#define EXPLOSION_CORE_COUNT        8       // large center billboards for heft
#define EXPLOSION_CORE_OFFSET       16.0f   // offset multiplier to spread out center billboards
#define EXPLOSION_DURATION          0.8f
#define EXPLOSION_DRIFT_SPEED       0.6f    // fraction of radius per second
#define EXPLOSION_OUTER_HALF_SIZE   5.0f    // outer particle base size
#define EXPLOSION_CORE_HALF_SIZE    18.0f   // center glow base size
#define EXPLOSION_GOLDEN_ANGLE      2.399963f  // PI * (3 - sqrt(5))

void qk_renderer_draw_explosion(f32 x, f32 y, f32 z,
                                 f32 radius, f32 age_seconds,
                                 f32 r, f32 g, f32 b, f32 a)
{
    if (!g_r.fx.initialized) return;
    if (g_r.fx.draw_count >= R_FX_MAX_DRAWS) return;
    if (age_seconds >= EXPLOSION_DURATION) return;
    if (a <= 0.0f) return;

    f32 t = age_seconds / EXPLOSION_DURATION;

    // Auto-emit dynamic light (caller offsets position back along trajectory)
    {
        f32 light_fade = (1.0f - t) * (1.0f - t);
        qk_dynamic_light_t light = {
            .position  = { x, y, z },
            .radius    = radius * 3.0f,
            .color     = { r, g, b },
            .intensity = 2.0f * light_fade * a
        };
        qk_renderer_submit_light(&light);
    }
    f32 fade = (1.0f - t);
    fade = fade * fade;   // quadratic falloff

    // Camera billboard frame for screen-facing quads
    f32 cam_pos[3];
    r_fx_get_cam_pos(cam_pos);
    f32 origin[3] = { x, y, z };
    f32 cam_right[3], cam_up[3];
    r_fx_build_cam_frame(origin, cam_pos, cam_right, cam_up);

    u32 v_start = g_r.fx.vertex_count;
    u32 v_count = 0;
    r_fx_vertex_t *verts = g_r.fx.vertices;

    // --- Core billboards: 8 large overlapping quads at center for heft ---
    for (u32 i = 0; i < EXPLOSION_CORE_COUNT; i++) {
        // Slight offset per core particle so they don't z-fight
        f32 ox = EXPLOSION_CORE_OFFSET * (r_fx_hash(i * 137 + FX_CH_X) - 0.5f);
        f32 oy = EXPLOSION_CORE_OFFSET * (r_fx_hash(i * 251 + FX_CH_Y) - 0.5f);
        f32 oz = EXPLOSION_CORE_OFFSET * (r_fx_hash(i * 379 + FX_CH_Z) - 0.5f);

        f32 center[3] = { x + ox, y + oy, z + oz };

        // Core starts large and bright, fades and shrinks quickly
        f32 core_t = t * 1.5f;
        if (core_t > 1.0f) core_t = 1.0f;
        f32 core_fade = (1.0f - core_t);
        core_fade = core_fade * core_fade;
        f32 half_size = EXPLOSION_CORE_HALF_SIZE * (0.5f + 0.5f * r_fx_hash(i * 89 + FX_CH_SIZE))
                        * (1.0f - core_t * 0.3f);

        /* Random rotation per core particle: rotate cam_right/cam_up
           by a stable random angle so overlapping quads don't align */
        f32 rot_angle = r_fx_hash(i * 457 + FX_CH_ROTATION) * 2.0f * PI;
        f32 rot_sin, rot_cos;
        r_fx_fast_sincos(rot_angle, &rot_sin, &rot_cos);
        f32 rot_right[3] = {
            cam_right[0] * rot_cos + cam_up[0] * rot_sin,
            cam_right[1] * rot_cos + cam_up[1] * rot_sin,
            cam_right[2] * rot_cos + cam_up[2] * rot_sin
        };
        f32 rot_up[3] = {
            -cam_right[0] * rot_sin + cam_up[0] * rot_cos,
            -cam_right[1] * rot_sin + cam_up[1] * rot_cos,
            -cam_right[2] * rot_sin + cam_up[2] * rot_cos
        };

        // Hot white-yellow center color
        f32 hue = r_fx_hash(i * 61 + FX_CH_HUE);
        f32 intensity = core_fade * a;
        f32 color[4] = {
            (0.95f + 0.05f * hue) * intensity,
            (0.7f + 0.25f * hue) * intensity,
            (0.15f + 0.2f * (1.0f - hue)) * intensity,
            intensity
        };

        u32 emitted = r_fx_emit_screen_facing_quad(verts, v_start + v_count,
                                                       center, half_size,
                                                       rot_right, rot_up,
                                                       color);
        v_count += emitted;
    }

    // --- Outer particles: Fibonacci sphere distribution ---
    for (u32 i = 0; i < EXPLOSION_OUTER_COUNT; i++) {
        f32 fi_y = 1.0f - (2.0f * (f32)i + 1.0f) / (f32)EXPLOSION_OUTER_COUNT;
        f32 r_xz = sqrtf(1.0f - fi_y * fi_y);
        f32 theta = EXPLOSION_GOLDEN_ANGLE * (f32)i;
        f32 dir[3] = {
            r_xz * cosf(theta),
            r_xz * sinf(theta),
            fi_y
        };

        // Per-particle random drift factor (stable per particle index)
        f32 drift_var = 0.7f + 0.6f * r_fx_hash(i * 197 + FX_CH_DRIFT);
        f32 dist = radius * (1.0f + t * EXPLOSION_DRIFT_SPEED * drift_var);

        f32 center[3] = {
            x + dir[0] * dist,
            y + dir[1] * dist,
            z + dir[2] * dist
        };

        // Per-particle size variation: shrink as age increases
        f32 size_var = 0.6f + 0.8f * r_fx_hash(i * 311 + FX_CH_SIZE);
        f32 half_size = EXPLOSION_OUTER_HALF_SIZE * size_var * (1.0f - t * 0.5f);

        // Fiery color variation per particle: shift between orange and yellow
        f32 hue_shift = r_fx_hash(i * 73 + FX_CH_HUE);
        f32 pr = r * (0.9f + 0.2f * hue_shift);
        f32 pg = g * (0.5f + 0.5f * hue_shift);
        f32 pb = b * (0.2f + 0.3f * (1.0f - hue_shift));

        // Sparkle: time-varying brightness jitter
        f32 sparkle = 0.75f + 0.25f * r_fx_hash(i * 41 + FX_CH_SPARKLE + (u32)(age_seconds * 30.0f));

        // Per-particle wisp: some particles fade faster for organic look
        f32 wisp = 1.0f - t * 0.5f * r_fx_hash(i * 59 + FX_CH_WISP);

        f32 intensity = fade * sparkle * wisp * a;

        f32 color[4] = {
            pr * intensity,
            pg * intensity,
            pb * intensity,
            intensity
        };

        // Random rotation per outer particle
        f32 orot = r_fx_hash(i * 523 + FX_CH_ROTATION) * 2.0f * PI;
        f32 os, oc;
        r_fx_fast_sincos(orot, &os, &oc);
        f32 orot_right[3] = {
            cam_right[0] * oc + cam_up[0] * os,
            cam_right[1] * oc + cam_up[1] * os,
            cam_right[2] * oc + cam_up[2] * os
        };
        f32 orot_up[3] = {
            -cam_right[0] * os + cam_up[0] * oc,
            -cam_right[1] * os + cam_up[1] * oc,
            -cam_right[2] * os + cam_up[2] * oc
        };

        u32 emitted = r_fx_emit_screen_facing_quad(verts, v_start + v_count,
                                                       center, half_size,
                                                       orot_right, orot_up,
                                                       color);
        v_count += emitted;
    }

    if (v_count == 0) return;

    r_fx_draw_t *draw = &g_r.fx.draws[g_r.fx.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.fx.vertex_count = v_start + v_count;
}

// --- Railgun Impact Sparks ---

#define RAIL_IMPACT_SPARK_COUNT     96      // number of spark particles
#define RAIL_IMPACT_DURATION        1.5f    // seconds until fully faded
#define RAIL_IMPACT_SPREAD          8.0f    // hemisphere spread radius
#define RAIL_IMPACT_DRIFT_SPEED     30.0f   // units/sec outward drift
#define RAIL_IMPACT_SPARK_SIZE      2.0f    // half-size of each spark quad
#define RAIL_IMPACT_JITTER_FREQ     12.5f   // Hz of positional jitter
#define RAIL_IMPACT_JITTER_AMP      2.5f    // max jitter displacement
#define RAIL_IMPACT_REFL_BOOST      15.0f   // extra drift along reflection vector

void qk_renderer_draw_rail_impact(f32 x, f32 y, f32 z,
                                   f32 normal_x, f32 normal_y, f32 normal_z,
                                   f32 in_dir_x, f32 in_dir_y, f32 in_dir_z,
                                   f32 age_seconds, u32 color_rgba)
{
    if (!g_r.fx.initialized) return;
    if (g_r.fx.draw_count >= R_FX_MAX_DRAWS) return;
    if (age_seconds >= RAIL_IMPACT_DURATION) return;

    f32 t = age_seconds / RAIL_IMPACT_DURATION;
    f32 fade = (1.0f - t);

    // Auto-emit dynamic light (offset along surface normal so it's not coplanar)
    {
        const f32 LIGHT_OFFSET = 8.0f;
        f32 light_fade = fade * fade;
        f32 cr = (f32)((color_rgba >> 24) & 0xFF) / 255.0f;
        f32 cg = (f32)((color_rgba >> 16) & 0xFF) / 255.0f;
        f32 cb = (f32)((color_rgba >>  8) & 0xFF) / 255.0f;
        qk_dynamic_light_t light = {
            .position  = { x + normal_x * LIGHT_OFFSET,
                           y + normal_y * LIGHT_OFFSET,
                           z + normal_z * LIGHT_OFFSET },
            .radius    = 128.0f,
            .color     = { cr, cg, cb },
            .intensity = 1.5f * light_fade
        };
        qk_renderer_submit_light(&light);
    }
    fade = fade * fade; // quadratic falloff

    // Surface normal (normalized)
    f32 nrm[3] = { normal_x, normal_y, normal_z };
    r_fx_normalize(nrm);

    /* Compute reflection: R = D - 2(D·N)N
     * At perpendicular impact R ≈ N (full hemisphere off surface).
     * At grazing angles R is nearly parallel to surface, biasing
     * sparks to spray along the wall away from the shooter. */
    f32 in_d[3] = { in_dir_x, in_dir_y, in_dir_z };
    r_fx_normalize(in_d);
    f32 d_dot_n = r_fx_dot(in_d, nrm);
    f32 refl[3] = {
        in_d[0] - 2.0f * d_dot_n * nrm[0],
        in_d[1] - 2.0f * d_dot_n * nrm[1],
        in_d[2] - 2.0f * d_dot_n * nrm[2]
    };
    r_fx_normalize(refl);

    // Build tangent frame from reflection vector
    f32 tangent[3], bitangent[3];
    r_fx_build_perp_frame(refl, tangent, bitangent);

    // Base color from u32 RGBA
    f32 base_r = (f32)((color_rgba >> 24) & 0xFF) / 255.0f;
    f32 base_g = (f32)((color_rgba >> 16) & 0xFF) / 255.0f;
    f32 base_b = (f32)((color_rgba >>  8) & 0xFF) / 255.0f;

    // Camera billboard frame for screen-facing sparks
    f32 cam_pos[3];
    r_fx_get_cam_pos(cam_pos);
    f32 origin[3] = { x, y, z };
    f32 cam_right[3], cam_up[3];
    r_fx_build_cam_frame(origin, cam_pos, cam_right, cam_up);

    u32 v_start = g_r.fx.vertex_count;
    u32 v_count = 0;
    r_fx_vertex_t *verts = g_r.fx.vertices;

    // Position array for indexed access in loops
    f32 pos[3] = { x, y, z };

    // Seed from position for deterministic per-impact variation
    u32 impact_seed = (u32)(x * 7.0f) ^ (u32)(y * 13.0f) ^ (u32)(z * 19.0f);

    for (u32 i = 0; i < RAIL_IMPACT_SPARK_COUNT; i++) {
        /* Hemisphere distribution: each spark has a random direction in the
         * hemisphere defined by the surface normal. Use hash-based random
         * azimuth and elevation. */
        f32 azimuth = r_fx_hash(i * 131 + impact_seed + FX_CH_AZIMUTH) * 2.0f * PI;
        f32 elev_raw = r_fx_hash(i * 199 + impact_seed + FX_CH_ELEV); // 0..1

        /* Bias sparks toward reflection axis: sqrt pulls the distribution
         * toward elev=1, concentrating ~60% of sparks within 45 degrees
         * of the reflection vector for a strong directional ricochet. */
        f32 elev = sqrtf(elev_raw);
        f32 cos_elev = sqrtf(1.0f - elev);
        f32 sin_elev = sqrtf(elev);

        // Direction in world space: hemisphere around reflection vector
        f32 dir[3];
        for (int c = 0; c < 3; c++) {
            dir[c] = refl[c] * sin_elev
                   + tangent[c] * cos_elev * cosf(azimuth)
                   + bitangent[c] * cos_elev * sinf(azimuth);
        }

        // Per-spark speed variation
        f32 speed_var = 0.5f + r_fx_hash(i * 251 + impact_seed + FX_CH_SPEED) * 1.0f;
        f32 dist = age_seconds * RAIL_IMPACT_DRIFT_SPEED * speed_var;

        /* Extra velocity along reflection axis elongates the spray
         * into a visible directional ricochet jet */
        f32 refl_drift = age_seconds * RAIL_IMPACT_REFL_BOOST * speed_var;

        // Spread: sparks fan out from the impact point
        f32 spread = RAIL_IMPACT_SPREAD * r_fx_hash(i * 307 + impact_seed + FX_CH_SPREAD);

        f32 center[3];
        for (int c = 0; c < 3; c++) {
            center[c] = pos[c] + dir[c] * (dist + spread) + refl[c] * refl_drift;
        }

        /* Buzzy jitter: rapid, erratic positional noise.
         * Each spark jitters at high frequency with a phase offset.
         * The hash seed changes with time to create erratic movement. */
        u32 jitter_seed = i * 173 + (u32)(age_seconds * RAIL_IMPACT_JITTER_FREQ);
        f32 jx = r_fx_hash_signed(jitter_seed + FX_CH_X) * RAIL_IMPACT_JITTER_AMP;
        f32 jy = r_fx_hash_signed(jitter_seed + FX_CH_Y) * RAIL_IMPACT_JITTER_AMP;
        f32 jz = r_fx_hash_signed(jitter_seed + FX_CH_Z) * RAIL_IMPACT_JITTER_AMP;
        // Jitter decays as spark fades
        center[0] += jx * fade;
        center[1] += jy * fade;
        center[2] += jz * fade;

        // Spark size: small, shrinks as it fades
        f32 size_var = 0.6f + 0.8f * r_fx_hash(i * 89 + impact_seed + FX_CH_SIZE);
        f32 half_size = RAIL_IMPACT_SPARK_SIZE * size_var * fade;

        // Electrical flickering: rapid on/off brightness changes
        f32 flicker = r_fx_hash(i * 61 + (u32)(age_seconds * 60.0f) + impact_seed + FX_CH_SPARKLE);
        flicker = flicker > 0.3f ? 1.0f : 0.15f; // binary-ish flicker

        /* Hot white core with beam color tint.
         * Early sparks are white-hot, then shift toward beam color as they cool. */
        f32 white_mix = (1.0f - t) * 0.6f;
        f32 pr = base_r * (1.0f - white_mix) + white_mix;
        f32 pg = base_g * (1.0f - white_mix) + white_mix;
        f32 pb = base_b * (1.0f - white_mix) + white_mix;

        f32 intensity = fade * flicker;
        f32 color[4] = {
            pr * intensity,
            pg * intensity,
            pb * intensity,
            intensity
        };

        /* Random rotation per spark (project rule: all new particles must
         * spawn with random rotation, using deterministic hash for seed) */
        f32 rot_angle = r_fx_hash(i * 457 + impact_seed + FX_CH_ROTATION) * 2.0f * PI;
        f32 rot_sin, rot_cos;
        r_fx_fast_sincos(rot_angle, &rot_sin, &rot_cos);
        f32 rot_right[3] = {
            cam_right[0] * rot_cos + cam_up[0] * rot_sin,
            cam_right[1] * rot_cos + cam_up[1] * rot_sin,
            cam_right[2] * rot_cos + cam_up[2] * rot_sin
        };
        f32 rot_up[3] = {
            -cam_right[0] * rot_sin + cam_up[0] * rot_cos,
            -cam_right[1] * rot_sin + cam_up[1] * rot_cos,
            -cam_right[2] * rot_sin + cam_up[2] * rot_cos
        };

        u32 emitted = r_fx_emit_screen_facing_quad(verts, v_start + v_count,
                                                       center, half_size,
                                                       rot_right, rot_up,
                                                       color);
        v_count += emitted;
    }

    if (v_count == 0) return;

    r_fx_draw_t *draw = &g_r.fx.draws[g_r.fx.draw_count++];
    draw->vertex_offset = v_start;
    draw->vertex_count = v_count;
    g_r.fx.vertex_count = v_start + v_count;
}

// --- Command Recording ---

void r_fx_record_commands(VkCommandBuffer cmd, u32 frame_index)
{
    if (!g_r.fx_pipeline.handle || !g_r.fx.initialized) return;
    if (g_r.fx.draw_count == 0) return;

    // Upload vertices to this frame's mapped buffer
    void *dst = g_r.fx.vertex_mapped[frame_index];
    if (!dst) return;
    memcpy(dst, g_r.fx.vertices,
           g_r.fx.vertex_count * sizeof(r_fx_vertex_t));

    r_frame_data_t *frame = &g_r.frames[frame_index];

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_r.fx_pipeline.handle);

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

    // Bind view UBO (set 0)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_r.fx_pipeline.layout, 0, 1,
                            &frame->view_descriptor_set, 0, NULL);

    // Bind vertex buffer
    VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_r.fx.vertex_buffers[frame_index], &vb_offset);

    // Draw all beam batches
    for (u32 i = 0; i < g_r.fx.draw_count; i++) {
        r_fx_draw_t *draw = &g_r.fx.draws[i];
        vkCmdDraw(cmd, draw->vertex_count, 1, draw->vertex_offset, 0);
        g_r.stats_draw_calls++;
        g_r.stats_triangles += draw->vertex_count / 3;
    }
}
