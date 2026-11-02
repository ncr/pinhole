#version 440
// A photo in the stage (stage.h, docs/hdr.md H3), drawn from its own codes:
// each channel made linear light by its profile's table, then its
// primaries to linear sRGB's. Clipped to sRGB (0 to 1) and filtered as the
// main window filters its sRGB photo (between the codes, in sRGB's curve),
// it is the window's photo (colour.cpp's toSrgb: the same tables and
// matrix); `gamut` mixes in what lies beyond, which an scRGB stage keeps as
// values below 0 and above 1; and its gain map (H4) takes it above SDR
// white, at the weight the stage gives. Written as linear light, premultiplied
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
    // The gain map (docs/hdr.md H4, the maths): its kind (0 none, 1
    // Apple's, 2 ISO 21496-1's), its weight now (0: the SDR base), Apple's
    // headroom (linear), multi-channel (1). ISO's, per channel: the gain's
    // range in log2, 1 / gamma, the base's and the alternate's offsets.
    vec4 gain;
    vec4 gainMin;
    vec4 gainMax;
    vec4 gainGamma;
    vec4 baseOffset;
    vec4 alternateOffset;
    // Linear sRGB back to the photo's primaries, by rows.
    vec4 fromSrgb0;
    vec4 fromSrgb1;
    vec4 fromSrgb2;
    // Each code's linear light, red, green and blue.
    vec4 linear[256];
};

layout(binding = 1) uniform sampler2D photo;
// Its codes, at a quarter to a half of the photo's size: filtered by the
// texture, which is the upscale.
layout(binding = 2) uniform sampler2D gainMap;

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

// ISO's gain with its offsets: not taken below black by them (as
// libultrahdr has it); a colour beyond sRGB's, below 0 already, as it comes.
vec3 offsetGain(vec3 v, vec3 stops) {
    vec3 out_ = (v + baseOffset.rgb) * exp2(stops) - alternateOffset.rgb;
    return mix(out_, max(out_, vec3(0.0)), step(vec3(0.0), v));
}

// The photo in linear sRGB (1 SDR white) above SDR white by its gain map:
// (rgb + base offset) * 2^(gain * weight) - alternate offset, the gain
// from the map's code by its kind. A one-channel map's gain is the same
// in any primaries (they share white); a three-channel one applies in the
// photo's own.
vec3 gained(vec3 rgb) {
    float w = gain.y;
    if (gain.x < 0.5 || w == 0.0)
        return rgb;
    vec3 code = texture(gainMap, texCoord).rgb;
    vec3 g;
    if (gain.x < 1.5) {
        // Apple's: Rec. 709's curve undone, then 1 + (Hi - 1) g.
        float c = code.r;
        float l = c <= 0.081 ? c / 4.5 : pow((c + 0.099) / 1.099, 1.0 / 0.45);
        g = vec3(log2(1.0 + (gain.z - 1.0) * l));
        return rgb * exp2(g * w);
    }
    g = mix(gainMin.rgb, gainMax.rgb, pow(code, gainGamma.rgb));
    if (gain.w > 0.5) {
        vec3 own = vec3(dot(fromSrgb0.xyz, rgb), dot(fromSrgb1.xyz, rgb), dot(fromSrgb2.xyz, rgb));
        own = offsetGain(own, g * w);
        return vec3(dot(toSrgb0.xyz, own), dot(toSrgb1.xyz, own), dot(toSrgb2.xyz, own));
    }
    return offsetGain(rgb, g * w);
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
    vec3 rgb = gained(mix(sdr, wide, more.y));
    if (more.z < 0.5)
        rgb = encodeClipped(rgb);
    fragColor = vec4(rgb * alpha, alpha) * more.x;
}
