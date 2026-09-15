##ifdef GL_ES
#version 100
##endif
##ifdef GL_CORE
#version 110
##endif
##ifdef SDL3GPU
#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in float aLight;
layout(location = 3) in float aZ;
layout(location = 4) in float aRepMask;
layout(std140, set = 1, binding = 0) uniform ScreenParams { vec2 uScreenScale; };
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec2 vTexCoordOverW;
layout(location = 2) out float vInvW;
layout(location = 3) out float vLight;
layout(location = 4) out float vRepMask;
layout(location = 5) out float vZ;
layout(location = 6) out float vZOverW;
void main() {
    float cx = aPos.x * uScreenScale.x - 1.0;
    float cy = 1.0 - aPos.y * uScreenScale.y;
    gl_Position = vec4(cx, cy, 1.0 - aZ / 65535.0, 1.0);
    float invW = 1.0 / max(aZ, 1.0);
    vTexCoord = aTexCoord;
    vTexCoordOverW = aTexCoord * invW;
    vInvW = invW;
    vLight = aLight;
    vRepMask = aRepMask;
    vZ = aZ;
    vZOverW = aZ * invW;
}
##else
attribute vec2 aPos;
attribute vec2 aTexCoord;
attribute float aLight;
attribute float aZ;
attribute float aRepMask;
uniform vec2 uScreenScale; /* 2.0/resX, 2.0/resY */
/* Perspective-correct interpolation: the SW rasterizer
   (POLYTZF/POLYTEXZ, Z-textured types 16-24) interpolates
   MapU/MapV/ZBuf over W = 1/Z.  w=1.0 in clip space makes varyings
   interpolate affinely in screen space, so premultiply by 1/Z here
   and divide in the fragment shader.  aZ is GET_ZO(Z0) (proportional
   to Z0); the constant of proportionality cancels in the fragment
   division.  uPerspCorrect selects the corrected pair; otherwise the
   raw affine varyings are used (types 8-15, SW POLYTEXT/POLYGTEX). */
varying vec2 vTexCoord;
varying vec2 vTexCoordOverW;
varying float vInvW;
varying float vLight;
varying float vRepMask;
varying float vZ;
varying float vZOverW;
void main() {
    float cx = aPos.x * uScreenScale.x - 1.0;
    float cy = 1.0 - aPos.y * uScreenScale.y;
    gl_Position = vec4(cx, cy, 1.0 - aZ / 65535.0, 1.0);
    float invW = 1.0 / max(aZ, 1.0);
    vTexCoord = aTexCoord;
    vTexCoordOverW = aTexCoord * invW;
    vInvW = invW;
    vLight = aLight;
    vRepMask = aRepMask;
    vZ = aZ;
    vZOverW = aZ * invW;
}
##endif