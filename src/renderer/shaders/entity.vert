#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
    uint tile_count_x;
    uint screen_width;
    uint screen_height;
    uint _pad;
} view;

layout(push_constant) uniform EntityPush {
    mat4 model;
    vec4 color;
} entity;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec3 frag_world_pos;
layout(location = 2) out vec4 frag_color;

void main() {
    vec4 world_pos = entity.model * vec4(in_position, 1.0);
    gl_Position = view.view_projection * world_pos;
    frag_normal = mat3(entity.model) * in_normal;
    frag_world_pos = world_pos.xyz;
    frag_color = entity.color;
}
