#version 440
// An HDR video frame's quad (videoview.h); its block as hdrvideo.frag's.

layout(location = 0) in vec4 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 0) out vec2 texCoord;

layout(std140, binding = 0) uniform buf {
    mat4 matrix;
    vec4 codes;
    vec4 ycc;
    vec4 ycc2;
    vec4 gamut0;
    vec4 gamut1;
    vec4 gamut2;
    vec4 curve;
    vec4 knee;
    vec4 more;
};

void main() {
    texCoord = vertexTexCoord;
    gl_Position = matrix * vertexPosition;
}
