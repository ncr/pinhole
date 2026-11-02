#version 440
// An HLG or PQ video frame drawn in SDR, or with headroom (videoview.h), in
// the maths of colour.cpp's toneMapToSrgb and sdrvideo.cpp's toneMapFrame:
// Y'CbCr to R'G'B' by its matrix; its light (HLG's inverse OETF and OOTF
// at 1000 nits, PQ's EOTF); BT.2390's EETF on the largest channel, to SDR
// white (203 nits) times the headroom; its primaries to sRGB's; clipped to
// the headroom, and sRGB's curve (extended above 1), or, in the stage's
// scRGB, linear light as it is.

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 matrix;
    // A sample's value times x is its code; Y' offset and scale; Cb and
    // Cr's offset.
    vec4 codes;
    // Cb and Cr's scale; Cr to R', Cb to G', Cr to G'.
    vec4 ycc;
    // Cb to B'; the matrix's kr, kg, kb.
    vec4 ycc2;
    // The primaries to sRGB's, by rows.
    vec4 gamut0;
    vec4 gamut1;
    vec4 gamut2;
    // HLG (1) or PQ (0); the peak in nits; the target (SDR white times
    // the headroom) in nits; the headroom.
    vec4 curve;
    // PQ of the peak; PQ of the target over it; the knee; whether the
    // peak is above the target (else nits clipped at the target).
    vec4 knee;
    // Opacity; the planes (2: Y' and CbCr, 3: Y', Cb, Cr); linear light
    // (1, scRGB) or sRGB's curve (0).
    vec4 more;
};

layout(binding = 1) uniform sampler2D plane1Texture;
layout(binding = 2) uniform sampler2D plane2Texture;
layout(binding = 3) uniform sampler2D plane3Texture;

const float kSdrWhite = 203.0;
const float m1 = 2610.0 / 16384.0;
const float m2 = 2523.0 / 4096.0 * 128.0;
const float c1 = 3424.0 / 4096.0;
const float c2 = 2413.0 / 4096.0 * 32.0;
const float c3 = 2392.0 / 4096.0 * 32.0;
const float hlgA = 0.17883277;
const float hlgB = 0.28466892;
const float hlgC = 0.55991073;

float pqToNits(float e) {
    float p = pow(clamp(e, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float nitsToPq(float nits) {
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// HLG's inverse OETF: a code to scene light from 0 to 1.
float hlgToScene(float e) {
    return e <= 0.5 ? e * e / 3.0 : (exp((e - hlgC) / hlgA) + hlgB) / 12.0;
}

// BT.2390's EETF: nits to the target's, SDR white 1.
float eetf(float nits) {
    if (knee.w < 0.5)
        return min(nits, curve.z) / kSdrWhite;
    float e1 = min(nitsToPq(nits) / knee.x, 1.0);
    float e2 = e1;
    if (e1 >= knee.z) {
        float t = (e1 - knee.z) / (1.0 - knee.z);
        float t2 = t * t;
        float t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * knee.z + (t3 - 2.0 * t2 + t) * (1.0 - knee.z) + (-2.0 * t3 + 3.0 * t2) * knee.y;
    }
    return min(pqToNits(e2 * knee.x), curve.z) / kSdrWhite;
}

float encodeSrgb(float v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

void main() {
    float y = texture(plane1Texture, texCoord).r;
    vec2 c = more.y > 2.5 ? vec2(texture(plane2Texture, texCoord).r, texture(plane3Texture, texCoord).r)
                          : texture(plane2Texture, texCoord).rg;
    float yy = (y * codes.x - codes.y) * codes.z;
    vec2 cc = (c * codes.x - codes.w) * ycc.x;
    vec3 e = clamp(vec3(yy + ycc.y * cc.y, yy + ycc.z * cc.x + ycc.w * cc.y, yy + ycc2.x * cc.x), 0.0, 1.0);
    vec3 light;
    if (curve.x > 0.5) {
        light = vec3(hlgToScene(e.r), hlgToScene(e.g), hlgToScene(e.b));
        // The OOTF: display light from the scene's, by its luminance.
        float ys = clamp(dot(light, ycc2.yzw), 0.0, 1.0);
        light *= curve.y * pow(ys, 0.2);
    } else {
        light = vec3(pqToNits(e.r), pqToNits(e.g), pqToNits(e.b));
    }
    float most = max(light.r, max(light.g, light.b));
    light *= most > 1e-6 ? eetf(min(most, curve.y)) / most : 1.0 / kSdrWhite;
    vec3 rgb = clamp(vec3(dot(gamut0.xyz, light), dot(gamut1.xyz, light), dot(gamut2.xyz, light)), 0.0, curve.w);
    if (more.z > 0.5)
        fragColor = vec4(rgb, 1.0) * more.x;
    else
        fragColor = vec4(encodeSrgb(rgb.r), encodeSrgb(rgb.g), encodeSrgb(rgb.b), 1.0) * more.x;
}
