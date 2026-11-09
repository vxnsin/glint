#version 450

// A still picture placed by its scaling mode, with the background colour where the picture
// does not reach, drawn at an opacity for crossfades. Colours are linear; the swapchain and
// the texture are sRGB, so the hardware converts on both ends.

layout(set = 0, binding = 0) uniform Frame {
    vec2 resolution;
    float time;
    float delta;
    vec2 pointer;
} frame;

layout(set = 0, binding = 1) uniform sampler2D picture;

layout(push_constant) uniform Push {
    vec2 offset;  // top-left of the picture in target pixels
    vec2 size;    // size of the picture in target pixels
    vec4 color;   // background, linear
    float opacity;
    uint tile;    // 1: the sampler repeats and the picture fills everything
    uint encode;  // 1: the target is not sRGB, encode here
} push;

layout(location = 0) out vec4 out_color;

vec3 encode_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec2 uv = (gl_FragCoord.xy - push.offset) / push.size;
    vec4 texel = texture(picture, uv);
    bool inside = push.tile != 0u || (all(greaterThanEqual(uv, vec2(0.0))) &&
                                      all(lessThanEqual(uv, vec2(1.0))));
    if (!inside) texel = vec4(0.0);
    vec3 rgb = mix(push.color.rgb, texel.rgb, texel.a);
    if (push.encode != 0u) rgb = encode_srgb(rgb);
    out_color = vec4(rgb, push.opacity);
}
