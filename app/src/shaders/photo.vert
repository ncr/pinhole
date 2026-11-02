#version 440
// The stage's photo's quad (stage.h); its block as photo.frag's.

layout(location = 0) in vec4 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 0) out vec2 texCoord;

layout(std140, binding = 0) uniform buf {
    mat4 matrix;
    vec4 toSrgb0;
    vec4 toSrgb1;
    vec4 toSrgb2;
    vec4 more;
    vec4 size;
    vec4 linear[256];
};

void main() {
    texCoord = vertexTexCoord;
    gl_Position = matrix * vertexPosition;
}
