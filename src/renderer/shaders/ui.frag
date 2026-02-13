#version 450

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D ui_texture;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex = texture(ui_texture, frag_uv);
    out_color = tex * frag_color;
}
