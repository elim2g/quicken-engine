#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) flat in uint frag_texture_id;
layout(location = 4) in vec2 frag_lm_uv;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
    uint tile_count_x;
    uint screen_width;
    uint screen_height;
    uint _pad;
} view;

layout(set = 1, binding = 0) uniform sampler2D textures[256];

/* Dynamic light SSBOs (set 2) */
struct DynamicLight {
    vec3 position;
    float radius;
    vec3 color;
    float intensity;
};

layout(set = 2, binding = 0, std430) readonly buffer LightBuffer {
    uint light_count;
    uint _pad0;
    uint _pad1;
    uint _pad2;
    DynamicLight lights[];
} light_buf;

layout(set = 2, binding = 1, std430) readonly buffer TileBuffer {
    uint data[];
} tile_buf;

layout(push_constant) uniform PC {
    uint texture_index;
    uint lightmap_index;
    float overbright;
    float ambient;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 tex_color = texture(textures[nonuniformEXT(pc.texture_index)], frag_uv).rgb;

    // Unpack per-surface color from texture_id (RGBA8 packed as uint)
    if (frag_texture_id != 0u) {
        vec3 surf_color = vec3(
            float((frag_texture_id >> 24u) & 0xFFu) / 255.0,
            float((frag_texture_id >> 16u) & 0xFFu) / 255.0,
            float((frag_texture_id >>  8u) & 0xFFu) / 255.0
        );
        tex_color *= surf_color;
    }

    // Lightmap sampling or fallback directional light
    vec3 lighting;
    if (pc.lightmap_index != 0u) {
        vec3 lm = texture(textures[nonuniformEXT(pc.lightmap_index)], frag_lm_uv).rgb;
        lighting = max(lm * pc.overbright, vec3(pc.ambient));
    } else {
        vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
        float ndotl = max(dot(normalize(frag_normal), light_dir), 0.0);
        lighting = vec3(0.2 + ndotl * 0.8);
    }

    // Dynamic light accumulation from Forward+ tile
    vec3 n = normalize(frag_normal);
    ivec2 tile = ivec2(gl_FragCoord.xy) / 16;
    uint tile_idx = uint(tile.y) * view.tile_count_x + uint(tile.x);
    uint base = tile_idx * 65u;
    uint n_lights = tile_buf.data[base];

    vec3 dynamic_lighting = vec3(0.0);
    for (uint i = 0u; i < n_lights; i++) {
        uint light_idx = tile_buf.data[base + 1u + i];
        DynamicLight light = light_buf.lights[light_idx];

        vec3 to_light = light.position - frag_world_pos;
        float dist = length(to_light);
        if (dist >= light.radius) continue;

        vec3 l = to_light / max(dist, 0.001);
        float ndotl = max(dot(n, l), 0.0);

        // Smooth quadratic attenuation
        float falloff = 1.0 - (dist / light.radius);
        float atten = falloff * falloff;

        dynamic_lighting += light.color * light.intensity * ndotl * atten;
    }

    out_color = vec4(tex_color * (lighting + dynamic_lighting), 1.0);
}
