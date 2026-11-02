#version 440
// A photo in the stage (stage.h, docs/hdr.md H3), drawn from its own codes:
// each channel made linear light by its profile's table, then its
// primaries to linear sRGB's. Clipped to sRGB (0 to 1) and filtered as the
// main window filters its sRGB photo (between the codes, in sRGB's curve),
// it is the window's photo (colour.cpp's toSrgb: the same tables and
// matrix); `gamut` mixes in what lies beyond, which an scRGB stage keeps as
// values below 0 and above 1. Written as linear light, premultiplied
// (scRGB); or, on a stage without scRGB (forced, for tests), clipped and
// in sRGB's curve.

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 matrix;
    // The matrix to linear sRGB, by rows.
    vec4 toSrgb0;
    vec4 toSrgb1;
    vec4 toSrgb2;
    // Opacity; gamut (0: clipped to sRGB, 1: all of the photo's); linear
    // (1: scRGB, 0: sRGB's curve); the texture's own filtering (1: its
    // mipmaps, for the full size up close) or the window's (0).
    vec4 more;
    // The texture's size in texels, and its inverse.
    vec4 size;
    // Each code's linear light, red, green and blue.
    vec4 linear[256];
};

layout(binding = 1) uniform sampler2D photo;

// A code from 0 to 1 (filtered, so between two) by the table.
float lookUp(float code, int channel) {
    float x = clamp(code, 0.0, 1.0) * 255.0;
    int i = int(floor(x));
    int j = min(i + 1, 255);
    return mix(linear[i][channel], linear[j][channel], x - float(i));
}

float encodeSrgb(float v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
float decodeSrgb(float e) {
    return e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4);
}
// sRGB's curve extended beyond 0 and 1, mirrored below 0.
vec3 encodeExtended(vec3 v) {
    return sign(v) * vec3(encodeSrgb(abs(v.r)), encodeSrgb(abs(v.g)), encodeSrgb(abs(v.b)));
}
vec3 decodeExtended(vec3 e) {
    return sign(e) * vec3(decodeSrgb(abs(e.r)), decodeSrgb(abs(e.g)), decodeSrgb(abs(e.b)));
}
vec3 encodeClipped(vec3 v) {
    v = clamp(v, 0.0, 1.0);
    return vec3(encodeSrgb(v.r), encodeSrgb(v.g), encodeSrgb(v.b));
}

// A texel's colour in linear sRGB, unclipped.
vec3 lightOf(vec4 c) {
    // Premultiplied by Qt's upload: its colour as it was.
    vec3 codes = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
    vec3 light = vec3(lookUp(codes.r, 0), lookUp(codes.g, 1), lookUp(codes.b, 2));
    return vec3(dot(toSrgb0.xyz, light), dot(toSrgb1.xyz, light), dot(toSrgb2.xyz, light));
}

void main() {
    vec3 sdr, wide;
    float alpha;
    if (more.w > 0.5) {
        // Mipmapped: filtered by the texture, as the window's is.
        vec4 c = texture(photo, texCoord);
        wide = lightOf(c);
        sdr = clamp(wide, 0.0, 1.0);
        alpha = c.a;
    } else {
        // Bilinear by hand, between the four texels converted: the clipped
        // in sRGB's curve, as the window filters its sRGB texture.
        // Each texel read at its middle (exactly it); past the edges, the
        // edge's (the sampler clamps).
        vec2 p = texCoord * size.xy - 0.5;
        vec2 f = fract(p);
        vec2 t0 = (floor(p) + 0.5) * size.zw;
        vec4 c00 = texture(photo, t0);
        vec4 c10 = texture(photo, t0 + vec2(size.z, 0.0));
        vec4 c01 = texture(photo, t0 + vec2(0.0, size.w));
        vec4 c11 = texture(photo, t0 + size.zw);
        vec3 l00 = lightOf(c00), l10 = lightOf(c10), l01 = lightOf(c01), l11 = lightOf(c11);
        vec3 s = mix(mix(encodeClipped(l00) * c00.a, encodeClipped(l10) * c10.a, f.x),
                     mix(encodeClipped(l01) * c01.a, encodeClipped(l11) * c11.a, f.x), f.y);
        vec3 w = mix(mix(encodeExtended(l00) * c00.a, encodeExtended(l10) * c10.a, f.x),
                     mix(encodeExtended(l01) * c01.a, encodeExtended(l11) * c11.a, f.x), f.y);
        alpha = mix(mix(c00.a, c10.a, f.x), mix(c01.a, c11.a, f.x), f.y);
        vec3 unpremultiplied = alpha > 0.0 ? s / alpha : vec3(0.0);
        sdr = vec3(decodeSrgb(unpremultiplied.r), decodeSrgb(unpremultiplied.g), decodeSrgb(unpremultiplied.b));
        wide = decodeExtended(alpha > 0.0 ? w / alpha : vec3(0.0));
    }
    vec3 rgb = mix(sdr, wide, more.y);
    if (more.z < 0.5)
        rgb = encodeClipped(rgb);
    fragColor = vec4(rgb * alpha, alpha) * more.x;
}
