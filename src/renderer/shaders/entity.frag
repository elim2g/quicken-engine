#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec4 frag_color;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
} view;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 n = normalize(frag_normal);
    float ndotl = max(dot(n, light_dir), 0.0);
    float ambient = 0.25;
    float diffuse = ndotl * 0.75;

    out_color = vec4(frag_color.rgb * (ambient + diffuse), frag_color.a);
}
