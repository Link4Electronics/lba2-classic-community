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
layout(set = 2, binding = 1) uniform sampler2D uPalette;
layout(std140, set = 3, binding = 0) uniform PresentParams {
    int   uIndexed;   /* 0 = ARGB staging, 1 = R8 palette indices */
    int   uLinear;    /* 0 = nearest, 1 = bilinear (R8: 4 taps + LUT) */
    float uOpaqueX0;  /* modal full-2D UI rect, engine pixels */
    float uOpaqueY0;
    float uOpaqueX1;  /* x1 <= x0 or y1 <= y0 → inactive */
    float uOpaqueY1;
    float uSizeX;     /* present texture size in engine pixels */
    float uSizeY;
};
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

vec4 paletteLookup(float idx) {
    return texture(uPalette, vec2((idx + 0.5) / 256.0, 0.5));
}

/* One texel of the R8 index page, resolved through the LUT, then the modal
   full-2D UI rule: index 0 / alpha 0 inside the rect presents opaque black
   like the software present (patch-then-filter, matching a CPU patch of the
   ARGB staging before hardware bilinear). */
vec4 indexedTexel(ivec2 p) {
    float idx = texelFetch(uTex, p, 0).r * 255.0;
    vec4 color = paletteLookup(idx);
    if (uOpaqueX1 > uOpaqueX0 && uOpaqueY1 > uOpaqueY0 &&
        float(p.x) >= uOpaqueX0 && float(p.x) < uOpaqueX1 &&
        float(p.y) >= uOpaqueY0 && float(p.y) < uOpaqueY1 && color.a == 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    return color;
}

void main() {
    vec4 color;
    if (uIndexed == 0) {
        color = texture(uTex, vTexCoord);
        /* ARGB path: CPU no longer patches staging; apply the modal rect
           here so the console/touch frame still gets opaque-black-on-alpha-0
           before the blend (nearest path; bilinear ARGB still filters first —
           that path is the rare needsArgb fallback). */
        vec2 pixel = vTexCoord * vec2(uSizeX, uSizeY);
        if (uOpaqueX1 > uOpaqueX0 && uOpaqueY1 > uOpaqueY0 &&
            pixel.x >= uOpaqueX0 && pixel.x < uOpaqueX1 &&
            pixel.y >= uOpaqueY0 && pixel.y < uOpaqueY1 && color.a == 0.0)
            color = vec4(0.0, 0.0, 0.0, 1.0);
    } else if (uLinear != 0) {
        vec2 texel = vTexCoord * vec2(uSizeX, uSizeY) - 0.5;
        ivec2 base = ivec2(floor(texel));
        vec2 f = fract(texel);
        vec4 c00 = indexedTexel(base);
        vec4 c10 = indexedTexel(base + ivec2(1, 0));
        vec4 c01 = indexedTexel(base + ivec2(0, 1));
        vec4 c11 = indexedTexel(base + ivec2(1, 1));
        color = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    } else {
        ivec2 p = ivec2(floor(vTexCoord * vec2(uSizeX, uSizeY)));
        color = indexedTexel(p);
    }
    FragColor = color;
}
##else
uniform sampler2D uTex;
uniform sampler2D uPalette;
uniform int uIndexed;
uniform int uLinear;
uniform vec4 uOpaqueRect; /* x0, y0, x1, y1 — x1<=x0 or y1<=y0 inactive */
uniform vec2 uSize;
varying vec2 vTexCoord;

vec4 paletteLookup(float idx) {
    return texture2D(uPalette, vec2((idx + 0.5) / 256.0, 0.5));
}

vec4 indexedTexel(vec2 p) {
    p = clamp(p, vec2(0.0), uSize - 1.0);
    float idx = texture2D(uTex, (p + 0.5) / uSize).r * 255.0;
    vec4 color = paletteLookup(idx);
    if (uOpaqueRect.z > uOpaqueRect.x && uOpaqueRect.w > uOpaqueRect.y &&
        p.x >= uOpaqueRect.x && p.x < uOpaqueRect.z &&
        p.y >= uOpaqueRect.y && p.y < uOpaqueRect.w && color.a == 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    return color;
}

void main() {
    vec4 color;
    if (uIndexed == 0) {
        color = texture2D(uTex, vTexCoord);
        vec2 pixel = vTexCoord * uSize;
        if (uOpaqueRect.z > uOpaqueRect.x && uOpaqueRect.w > uOpaqueRect.y &&
            pixel.x >= uOpaqueRect.x && pixel.x < uOpaqueRect.z &&
            pixel.y >= uOpaqueRect.y && pixel.y < uOpaqueRect.w && color.a == 0.0)
            color = vec4(0.0, 0.0, 0.0, 1.0);
    } else if (uLinear != 0) {
        vec2 texel = vTexCoord * uSize - 0.5;
        vec2 base = floor(texel);
        vec2 f = texel - base;
        vec4 c00 = indexedTexel(base);
        vec4 c10 = indexedTexel(base + vec2(1.0, 0.0));
        vec4 c01 = indexedTexel(base + vec2(0.0, 1.0));
        vec4 c11 = indexedTexel(base + vec2(1.0, 1.0));
        color = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    } else {
        color = indexedTexel(floor(vTexCoord * uSize));
    }
    gl_FragColor = color;
}
##endif
