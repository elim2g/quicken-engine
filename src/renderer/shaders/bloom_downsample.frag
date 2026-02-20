#version 450

layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(push_constant) uniform PC {
    vec2 texel_size;
    float threshold;
    float intensity;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    // 13-tap box filter (dual-filter kawase style)
    vec3 a = texture(source_texture, uv + vec2(-2.0, -2.0) * pc.texel_size).rgb;
    vec3 b = texture(source_texture, uv + vec2( 0.0, -2.0) * pc.texel_size).rgb;
    vec3 c = texture(source_texture, uv + vec2( 2.0, -2.0) * pc.texel_size).rgb;
    vec3 d = texture(source_texture, uv + vec2(-1.0, -1.0) * pc.texel_size).rgb;
    vec3 e = texture(source_texture, uv).rgb;
    vec3 f = texture(source_texture, uv + vec2( 1.0, -1.0) * pc.texel_size).rgb;
    vec3 g = texture(source_texture, uv + vec2(-2.0,  0.0) * pc.texel_size).rgb;
    vec3 h = texture(source_texture, uv + vec2( 2.0,  0.0) * pc.texel_size).rgb;
    vec3 i = texture(source_texture, uv + vec2(-1.0,  1.0) * pc.texel_size).rgb;
    vec3 j = texture(source_texture, uv + vec2( 1.0,  1.0) * pc.texel_size).rgb;
    vec3 k = texture(source_texture, uv + vec2(-2.0,  2.0) * pc.texel_size).rgb;
    vec3 l = texture(source_texture, uv + vec2( 0.0,  2.0) * pc.texel_size).rgb;
    vec3 m = texture(source_texture, uv + vec2( 2.0,  2.0) * pc.texel_size).rgb;

    vec3 color = e * 0.125;
    color += (d + f + i + j) * 0.125;
    color += (a + b + g + k) * 0.03125;
    color += (b + c + h + m) * 0.03125;
    color += (g + h + k + l) * 0.03125;
    color += (l + m + h + c) * 0.03125;

    // Soft threshold on first pass (threshold > 0 only on mip 0)
    if (pc.threshold > 0.0) {
        float brightness = max(color.r, max(color.g, color.b));
        float soft = brightness - pc.threshold + 0.5;
        soft = clamp(soft, 0.0, 1.0);
        soft = soft * soft;
        float contribution = max(soft, brightness - pc.threshold) / max(brightness, 0.00001);
        color *= contribution;
    }

    out_color = vec4(color, 1.0);
}
