#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uint in_texture_id;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
} view;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) flat out uint frag_texture_id;

void main() {
    gl_Position = view.view_projection * vec4(in_position, 1.0);
    frag_normal = in_normal;
    frag_uv = in_uv;
    frag_world_pos = in_position;
    frag_texture_id = in_texture_id;
}
