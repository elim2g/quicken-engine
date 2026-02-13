#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_color;

layout(push_constant) uniform UIParams {
    vec2 screen_size;
} params;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

void main() {
    // Convert pixel coordinates to NDC
    vec2 ndc = (in_position / params.screen_size) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    frag_uv = in_uv;

    // Unpack RGBA8 color
    frag_color = vec4(
        float((in_color >>  0) & 0xFFu) / 255.0,
        float((in_color >>  8) & 0xFFu) / 255.0,
        float((in_color >> 16) & 0xFFu) / 255.0,
        float((in_color >> 24) & 0xFFu) / 255.0
    );
}
