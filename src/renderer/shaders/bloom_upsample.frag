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
    // 9-tap tent filter for smooth upsampling
    vec3 a = texture(source_texture, uv + vec2(-1.0, -1.0) * pc.texel_size).rgb;
    vec3 b = texture(source_texture, uv + vec2( 0.0, -1.0) * pc.texel_size).rgb;
    vec3 c = texture(source_texture, uv + vec2( 1.0, -1.0) * pc.texel_size).rgb;
    vec3 d = texture(source_texture, uv + vec2(-1.0,  0.0) * pc.texel_size).rgb;
    vec3 e = texture(source_texture, uv).rgb;
    vec3 f = texture(source_texture, uv + vec2( 1.0,  0.0) * pc.texel_size).rgb;
    vec3 g = texture(source_texture, uv + vec2(-1.0,  1.0) * pc.texel_size).rgb;
    vec3 h = texture(source_texture, uv + vec2( 0.0,  1.0) * pc.texel_size).rgb;
    vec3 i = texture(source_texture, uv + vec2( 1.0,  1.0) * pc.texel_size).rgb;

    // Tent filter weights (sum to 1.0):
    //   1/16  2/16  1/16
    //   2/16  4/16  2/16
    //   1/16  2/16  1/16
    vec3 color = e * (4.0 / 16.0);
    color += (b + d + f + h) * (2.0 / 16.0);
    color += (a + c + g + i) * (1.0 / 16.0);

    out_color = vec4(color * pc.intensity, 1.0);
}
