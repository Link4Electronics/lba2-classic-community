##ifdef GL_ES
#version 100
precision mediump float;
##endif
##ifdef GL_CORE
#version 110
##endif
##ifdef SDL3GPU
#version 450
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vTexCoord);
}
##else
uniform sampler2D uTex;
varying vec2 vTexCoord;
void main() {
    gl_FragColor = texture2D(uTex, vTexCoord);
}
##endif