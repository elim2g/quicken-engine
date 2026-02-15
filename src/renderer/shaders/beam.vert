#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(set = 0, binding = 0) uniform ViewUniforms {
    mat4 view_projection;
    vec3 camera_pos;
    float time;
} view;

layout(location = 0) out vec4 frag_color;

void main() {
    gl_Position = view.view_projection * vec4(in_position, 1.0);
    frag_color = in_color;
}
