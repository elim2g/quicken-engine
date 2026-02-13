#version 450

layout(set = 0, binding = 0) uniform sampler2D world_texture;
layout(set = 0, binding = 1) uniform sampler2D ui_texture;

layout(push_constant) uniform ComposeParams {
    vec4 viewport;      // x_offset, y_offset, width, height (normalized 0-1)
    uint mode;          // 0 = stretch, 1 = aspect fit
} params;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    // Map screen UV to render target UV based on viewport rect
    vec2 render_uv = (uv - params.viewport.xy) / params.viewport.zw;

    // Outside the viewport rect: black (letterbox/pillarbox)
    if (render_uv.x < 0.0 || render_uv.x > 1.0 ||
        render_uv.y < 0.0 || render_uv.y > 1.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 world = texture(world_texture, render_uv);
    vec4 ui = texture(ui_texture, render_uv);

    // Alpha blend UI over world
    out_color = vec4(mix(world.rgb, ui.rgb, ui.a), 1.0);
}
