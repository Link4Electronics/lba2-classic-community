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
layout(location = 0) out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
##else
attribute vec2 aPos;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
##endif