#version 450

layout(set = 0, binding = 0) uniform sampler2D world_texture;
layout(set = 0, binding = 1) uniform sampler2D bloom_texture;

layout(push_constant) uniform ComposeParams {
    vec4 viewport;      // x_offset, y_offset, width, height (normalized 0-1)
    uint mode;          // 0 = stretch, 1 = aspect fit
    float exposure;     // HDR exposure multiplier (default 1.0)
    float bloom_strength; // bloom mix factor (default 0.04)
} params;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// ACES filmic tone mapping (Narkowicz 2015 fit)
vec3 aces_tonemap(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // Map screen UV to render target UV based on viewport rect
    vec2 render_uv = (uv - params.viewport.xy) / params.viewport.zw;

    vec3 hdr = texture(world_texture, render_uv).rgb;

    // Add bloom
    hdr += texture(bloom_texture, render_uv).rgb * params.bloom_strength;

    // Exposure
    hdr *= params.exposure;

    // Tone map HDR -> LDR
    vec3 ldr = aces_tonemap(hdr);

    // Mask to black outside viewport rect (letterbox/pillarbox), branchless
    float inside = step(0.0, render_uv.x) * step(render_uv.x, 1.0)
                 * step(0.0, render_uv.y) * step(render_uv.y, 1.0);

    // Swapchain is SRGB -- hardware applies gamma. Output linear.
    out_color = vec4(ldr * inside, 1.0);
}
