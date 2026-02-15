#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in vec3 frag_world_pos;
layout(location = 3) flat in uint frag_texture_id;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
} view;

layout(set = 1, binding = 0) uniform sampler2D surface_texture;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 tex_color = texture(surface_texture, frag_uv).rgb;

    // Unpack per-surface color from texture_id (RGBA8 packed as uint)
    vec3 surf_color;
    if (frag_texture_id != 0u) {
        surf_color = vec3(
            float((frag_texture_id >> 24u) & 0xFFu) / 255.0,
            float((frag_texture_id >> 16u) & 0xFFu) / 255.0,
            float((frag_texture_id >>  8u) & 0xFFu) / 255.0
        );
        tex_color *= surf_color;
    }

    // Simple directional light + ambient for vertical slice
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    float ndotl = max(dot(normalize(frag_normal), light_dir), 0.0);
    float ambient = 0.2;
    float diffuse = ndotl * 0.8;

    out_color = vec4(tex_color * (ambient + diffuse), 1.0);
}
