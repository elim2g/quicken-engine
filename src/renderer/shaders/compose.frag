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

// AgX tone mapping (Troy Sobotka, Blender 4.0 implementation)
// Attempt to preserve hue through the entire dynamic range rather than
// shifting toward white like ACES does.

// Log2 encoding into AgX "log" space, clamped to [-12.47393, 4.02606]
vec3 agx_default_contrast(vec3 x) {
    // 6th order polynomial fit of the AgX sigmoid (Blender reference)
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}

vec3 agx_tonemap(vec3 color) {
    // AGX "inset" matrix: sRGB -> AgX working space
    const mat3 agx_mat = mat3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104
    );

    // AGX "outset" matrix: AgX working space -> sRGB
    const mat3 agx_mat_inv = mat3(
         1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,  -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116
    );

    const float min_ev = -12.47393;
    const float max_ev =   4.02606;

    // Encode into log2 space
    color = agx_mat * color;
    color = clamp(log2(max(color, vec3(1e-10))), min_ev, max_ev);

    // Normalize to 0-1 range
    color = (color - min_ev) / (max_ev - min_ev);

    // Apply sigmoid contrast curve
    color = agx_default_contrast(color);

    // Convert back to sRGB
    color = agx_mat_inv * color;

    return color;
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
    vec3 ldr = agx_tonemap(hdr);

    // Mask to black outside viewport rect (letterbox/pillarbox), branchless
    float inside = step(0.0, render_uv.x) * step(render_uv.x, 1.0)
                 * step(0.0, render_uv.y) * step(render_uv.y, 1.0);

    // Swapchain is SRGB -- hardware applies gamma. Output linear.
    out_color = vec4(ldr * inside, 1.0);
}
