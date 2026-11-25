#version 440

// A photo as a mosaic of other photos (MosaicView.qml). The picture is
// `cells` tiles; `plan` says which photo each is, and how far that photo's
// colour is from the picture's there; `atlas` has the photos, `side` by
// `side` of them, each `texels` square.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    // Columns and rows.
    vec2 cells;
    // 0 the picture itself, 1 the mosaic; between, the tiles grow in, in a
    // wave from the top left.
    float amount;
    // How far each tile is moved to the picture's colour there, 0 to 1.
    float tint;
    // The atlas's mip level a tile is drawn from, as small as it is drawn.
    float lod;
    float side;
    float texels;
    // The line between tiles, in tiles; 0 for none.
    float seam;
};

layout(binding = 1) uniform sampler2D atlas;
layout(binding = 2) uniform sampler2D plan;
layout(binding = 3) uniform sampler2D picture;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2 p = qt_TexCoord0 * cells;
    vec2 cell = min(floor(p), cells - 1.0);
    vec2 f = p - cell;

    // The plan's upper half: the photo's number, low byte in red, high in
    // green. Its lower half: the colour to add, around a half.
    vec2 at = (cell + 0.5) / vec2(cells.x, cells.y * 2.0);
    vec4 which = texture(plan, at);
    vec3 nudge = (texture(plan, at + vec2(0.0, 0.5)).rgb - 0.5) * 2.0;
    float number = floor(which.r * 255.0 + 0.5) + floor(which.g * 255.0 + 0.5) * 256.0;
    vec2 slot = vec2(mod(number, side), floor(number / side));

    // Within the tile, half a texel of this level from its edge: no
    // neighbour in the atlas shows through.
    float edge = min(0.5, 0.5 * exp2(lod) / texels);
    vec3 tile = textureLod(atlas, (slot + clamp(f, edge, 1.0 - edge)) / side, lod).rgb;
    tile = clamp(tile + nudge * tint, 0.0, 1.0);
    vec2 d = min(f, 1.0 - f);
    float line = seam > 0.0 ? 1.0 - smoothstep(0.0, seam, min(d.x, d.y)) : 0.0;
    tile *= 1.0 - 0.5 * line;

    vec3 whole = texture(picture, qt_TexCoord0).rgb;

    // Each tile at its own moment: sooner at the top left, and a little by
    // chance. It grows from its middle over the picture.
    float when = 0.55 * (cell.x / cells.x * 0.7 + cell.y / cells.y * 0.3) + 0.2 * hash(cell);
    float grown = smoothstep(when, when + 0.25, amount);
    vec2 from = abs(f - 0.5) * 2.0;
    float inside = grown > 0.0 ? step(max(from.x, from.y), grown) : 0.0;

    fragColor = vec4(mix(whole, tile, inside), 1.0) * qt_Opacity;
}
