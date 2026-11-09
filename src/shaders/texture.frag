#version 450

// Copies a texture over the whole target: the first pass of an image layer (the picture into
// its layer, padding cut off) and the last pass of a scene (the scene into the swapchain).
// Wallpaper Engine works without colour management, so when the target is an sRGB swapchain
// the shader decodes first and the hardware's encode gives the scene's values back.

layout(set = 0, binding = 1) uniform sampler2D source;

layout(push_constant) uniform Push {
    vec2 size;      // target size in pixels
    vec2 uv_scale;  // the used part of the source, e.g. the picture inside padding
    float opacity;  // crossfade
    uint decode;    // 1: undo the sRGB encode of the target
} push;

layout(location = 0) out vec4 out_color;

vec3 decode_srgb(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec2 uv = gl_FragCoord.xy / push.size * push.uv_scale;
    vec4 c = texture(source, uv);
    if (push.decode != 0u) c.rgb = decode_srgb(c.rgb);
    out_color = vec4(c.rgb, c.a * push.opacity);
}
