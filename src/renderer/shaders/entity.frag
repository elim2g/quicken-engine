#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec4 frag_color;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
    uint tile_count_x;
    uint screen_width;
    uint screen_height;
    uint _pad;
} view;

/* Dynamic light SSBOs (set 1 for entity pipeline) */
struct DynamicLight {
    vec3 position;
    float radius;
    vec3 color;
    float intensity;
};

layout(set = 1, binding = 0, std430) readonly buffer LightBuffer {
    uint light_count;
    uint _pad0;
    uint _pad1;
    uint _pad2;
    DynamicLight lights[];
} light_buf;

layout(set = 1, binding = 1, std430) readonly buffer TileBuffer {
    uint data[];
} tile_buf;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 n = normalize(frag_normal);
    float ndotl = max(dot(n, light_dir), 0.0);
    float ambient = 0.25;
    float diffuse = ndotl * 0.75;

    // Dynamic light accumulation from Forward+ tile
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
        float nl = max(dot(n, l), 0.0);

        float falloff = 1.0 - (dist / light.radius);
        float atten = falloff * falloff;

        dynamic_lighting += light.color * light.intensity * nl * atten;
    }

    out_color = vec4(frag_color.rgb * (ambient + diffuse + dynamic_lighting), frag_color.a);
}
