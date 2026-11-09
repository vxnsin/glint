#version 450

// A video frame from its three 8-bit planes (Y at full size, U and V at half), or from two
// (Y, and U and V interleaved, as VA-API hands them out), placed by its
// scaling mode like picture.frag, with the background colour around it. The planes hold
// gamma-encoded values; they are converted to RGB, then to linear for the sRGB swapchain.

layout(set = 0, binding = 1) uniform sampler2D plane_y;
layout(set = 0, binding = 2) uniform sampler2D plane_u;
layout(set = 0, binding = 3) uniform sampler2D plane_v;

layout(push_constant) uniform Push {
    vec2 offset;  // top-left of the frame in target pixels
    vec2 size;    // size of the frame in target pixels
    vec4 color;   // background, linear
    float opacity;
    uint tile;
    uint encode;      // 1: the target is not sRGB, encode here
    uint bt709;       // the colour matrix, else BT.601
    uint full_range;  // 0: video range (16 to 235)
    uint interleaved; // plane_u holds U in red and V in green
} push;

layout(location = 0) out vec4 out_color;

vec3 decode_srgb(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 encode_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec2 uv = (gl_FragCoord.xy - push.offset) / push.size;
    float y = texture(plane_y, uv).r;
    float u, v;
    if (push.interleaved != 0u) {
        vec2 c = texture(plane_u, uv).rg;
        u = c.r - 0.5;
        v = c.g - 0.5;
    } else {
        u = texture(plane_u, uv).r - 0.5;
        v = texture(plane_v, uv).r - 0.5;
    }
    if (push.full_range == 0u) {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u *= 255.0 / 224.0;
        v *= 255.0 / 224.0;
    }
    vec3 rgb = push.bt709 != 0u
                   ? vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u)
                   : vec3(y + 1.402 * v, y - 0.3441 * u - 0.7141 * v, y + 1.772 * u);
    rgb = decode_srgb(clamp(rgb, 0.0, 1.0));
    bool inside = push.tile != 0u || (all(greaterThanEqual(uv, vec2(0.0))) &&
                                      all(lessThanEqual(uv, vec2(1.0))));
    if (!inside) rgb = push.color.rgb;
    if (push.encode != 0u) rgb = encode_srgb(rgb);
    out_color = vec4(rgb, push.opacity);
}
