##ifdef GL_ES
#version 100
precision mediump float;
##endif
##ifdef GL_CORE
#version 110
##endif
##ifdef SDL3GPU
#version 450
/* Replicates the SW CLUT pipeline:
   1. Sample atlas  → 8-bit palette index (texel)
   2. CLUT lookup   → texel lit by gouraud intensity
   3. Palette lookup → ARGB color
   The CLUT is the full 64 KB fog table (256×256). SetCLUT selects a
   16-row block within it, but the gouraud value's high byte indexes
   into the correct rows directly. */
layout(set = 2, binding = 0) uniform sampler2D uAtlas;   /* 256×256 LUMINANCE */
layout(set = 2, binding = 1) uniform sampler2D uCLUT;    /* 256×256 LUMINANCE (fog table) */
layout(set = 2, binding = 2) uniform sampler2D uPalette; /* 256×1 ARGB (paletteLUT) */
layout(std140, set = 3, binding = 0) uniform GpuParams {
    int   uPolyMode;      /* 0=texture 1=flat 2=gouraud 3=gouraudTable
                             4=textureNoCLUT 5=fogSmooth 6=sceneShadow
                             7=brickQuad */
    int   uAlphaMode;     /* 0=opaque 1=TRANS(semi) 2=TRAME(stipple) */
    float uFlatColor;     /* palette index (mode 1) / CLUT column (mode 3) */
    int   uBilinear;      /* 0=nearest 1=bilinear CLUT sampling */
    int   uPerspCorrect;  /* 1=perspective-correct UV/Z (types 16-24) */
    int   uChromaKey;     /* 0=off 1=discard palette index 0 (INCRUST) */
    float uCLUTBaseRow;   /* CLUT row offset for gouraud table (mode 3) */
    float uScaledFogNear; /* Fill_ScaledFogNear (fog-smooth distance, mode 5) */
    float uFogRowScale;   /* Fill_Fog_Factor / 65536 (fog-smooth, mode 5) */
    float uBrickW;        /* brick atlas width (mode 7) */
    float uBrickH;        /* brick atlas height (mode 7) */
    float uBrickPad;
};
layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec2 vTexCoordOverW;
layout(location = 2) in float vInvW;
layout(location = 3) in float vLight;
layout(location = 4) in float vRepMask;
layout(location = 5) in float vZ;
layout(location = 6) in float vZOverW;
layout(location = 0) out vec4 FragColor;

/* Look up a palette index through the CLUT, returning the final RGBA
   color.  `palIdx` is the raw atlas texel (palette index) and
   `gouraudRow` the CLUT row selected by the gouraud intensity. */
vec4 clutLookup(float palIdx, float gouraudRow) {
    vec2 cuv = vec2((palIdx + 0.5) / 256.0,
                    (gouraudRow + 0.5) / 256.0);
    float lit = texture(uCLUT, cuv).r * 255.0;
    return texture(uPalette, vec2((lit + 0.5) / 256.0, 0.5));
}
/* Bilinear CLUT sampling: sample 4 atlas texels, CLUT each one, then
   interpolate the resulting RGBA colors.  This avoids the artifact
   where hardware bilinear on palette indices produces garbage colors. */
vec4 bilinearCLUT(vec2 tc, float widthMask, float heightMask,
                  float gouraudRow) {
    float u = mod(tc.x, widthMask + 1.0);
    float v = mod(tc.y, heightMask + 1.0);
    float iu = floor(u);
    float iv = floor(v);
    float fu = u - iu;
    float fv = v - iv;
    /* Wrap each of the 4 texel positions */
    vec2 p00 = vec2(mod(iu, widthMask + 1.0),
                    mod(iv, heightMask + 1.0));
    vec2 p10 = vec2(mod(iu + 1.0, widthMask + 1.0),
                    mod(iv, heightMask + 1.0));
    vec2 p01 = vec2(mod(iu, widthMask + 1.0),
                    mod(iv + 1.0, heightMask + 1.0));
    vec2 p11 = vec2(mod(iu + 1.0, widthMask + 1.0),
                    mod(iv + 1.0, heightMask + 1.0));
    /* Sample atlas → CLUT → palette for each texel */
    float t00 = texture(uAtlas, (p00 + 0.5) / 256.0).r * 255.0;
    float t10 = texture(uAtlas, (p10 + 0.5) / 256.0).r * 255.0;
    float t01 = texture(uAtlas, (p01 + 0.5) / 256.0).r * 255.0;
    float t11 = texture(uAtlas, (p11 + 0.5) / 256.0).r * 255.0;
    /* Chroma key: discard if nearest texel (p00) is index 0.
       Matches SW nearest-neighbor behaviour for INCRUST types. */
    if (uChromaKey != 0 && t00 < 0.5) discard;
    vec4 c00 = clutLookup(t00, gouraudRow);
    vec4 c10 = clutLookup(t10, gouraudRow);
    vec4 c01 = clutLookup(t01, gouraudRow);
    vec4 c11 = clutLookup(t11, gouraudRow);
    return mix(mix(c00, c10, fu), mix(c01, c11, fu), fv);
}
void main() {
    vec4 color;
    float alphaOverride = 0.0;
    /* Select perspective-corrected or affine texture/Z varyings.
       uPerspCorrect is 1 for Z-textured types (16-24), where the SW
       rasterizer divides MapU/MapV/ZBuf by W = 1/Z; types 8-15
       (SW POLYTEXT/POLYGTEX) interpolate affinely. */
    vec2 tc = mix(vTexCoord, vTexCoordOverW / vInvW, float(uPerspCorrect));
    float vZsel = mix(vZ, vZOverW / vInvW, float(uPerspCorrect));
    if (uPolyMode == 0) {
        /* Textured: atlas → CLUT → palette */
        tc = tc / 256.0;
        float widthMask = mod(vRepMask, 256.0);
        float heightMask = floor(vRepMask / 256.0);
        /* SW init: Fill_CurGouraudMin = (Pt_Light << 8) + 0x8000.
           The +128 (0x80) rounding bias shifts the CLUT row by 1 in some
           cases — same bias applied in polyMode 2. */
        float gouraudRow = floor((vLight + 128.0) / 256.0) + uCLUTBaseRow;
        if (uBilinear != 0) {
            color = bilinearCLUT(tc, widthMask, heightMask, gouraudRow);
        } else {
            float wrappedU = mod(tc.x, widthMask + 1.0);
            float wrappedV = mod(tc.y, heightMask + 1.0);
            vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,
                                (floor(wrappedV) + 0.5) / 256.0);
            float texel = texture(uAtlas, atlasUV).r * 255.0;
            /* Chroma key: atlas texel 0 = transparent (skipped by SW rasterizer
               at the Log buffer level).  Discard before CLUT so the fog table
               cannot remap index 0 to an opaque value.
               Only INCRUST types (12-15, 20-23) have chroma key; non-INCRUST
               types (8-11, 16-19, 24) write all texels including index 0. */
            if (uChromaKey != 0 && texel < 0.5) discard;
            color = clutLookup(texel, gouraudRow);
        }
    } else if (uPolyMode == 1) {
        /* Flat solid: single palette color */
        float palIdx = uFlatColor;
        color = texture(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    } else if (uPolyMode == 2) {
        /* Gouraud (types 4/5): palette = (color + (light+128)/256).
           SW init: Fill_CurGouraudMin = (Pt_Light << 8) + 0x8000 + (color << 16).
           The +0x8000 rounding bias means the palette index is
           (color + (Pt_Light + 128) / 256), not (color + Pt_Light / 256). */
        float palIdx = mod(uFlatColor + floor((vLight + 128.0) / 256.0), 256.0);
        color = texture(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    } else if (uPolyMode == 4) {
        /* Texture without CLUT (types 8 and 12):
           SW Filler_Texture / Filler_TextureChromaKey writes texel as palette
           index directly (*line = texel).  No CLUT lookup — the atlas texel
           IS the palette index.  Chroma key controlled by uChromaKey. */
        tc = tc / 256.0;
        float widthMask = mod(vRepMask, 256.0);
        float heightMask = floor(vRepMask / 256.0);
        float wrappedU = mod(tc.x, widthMask + 1.0);
        float wrappedV = mod(tc.y, heightMask + 1.0);
        vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,
                            (floor(wrappedV) + 0.5) / 256.0);
        float palIdx = texture(uAtlas, atlasUV).r * 255.0;
        /* Chroma key: atlas texel 0 = transparent.
           Only INCRUST types have chroma key. */
        if (uChromaKey != 0 && palIdx < 0.5) discard;
        color = texture(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    } else if (uPolyMode == 5) {
        /* Texture Z Fog Smooth (type 24, e.g. sea/sky):
           SW Filler_TextureZFogSmoothZBuf / ...NZW use a fixed CLUT base
           (PtrCLUTFog + 12*256) plus a per-pixel distance-based fog row,
           IGNORING Pt_Light entirely.  Replicating the SW math:
           zBufVal = interpolated Pt_ZO (the batch aZ)
           fogDist = max(zBufVal - Fill_ScaledFogNear, 0)
           fogRow  = (fogDist * Fill_Fog_Factor) >> 8, masked & 0xF000
           CLUT index = PtrCLUTFog + 12*256 + fogRow + texel
           As a CLUT row: (12*256 + fogRow)/256 = 12 + fogRow/256, where
           fogRow/256 = ((fogDist * Fill_Fog_Factor) >> 16) & 0xF0.
           uFogRowScale = Fill_Fog_Factor / 65536, so
           floor(zDist * uFogRowScale) = (fogDist * Fill_Fog_Factor) >> 16. */
        tc = tc / 256.0;
        float widthMask = mod(vRepMask, 256.0);
        float heightMask = floor(vRepMask / 256.0);
        float wrappedU = mod(tc.x, widthMask + 1.0);
        float wrappedV = mod(tc.y, heightMask + 1.0);
        vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,
                            (floor(wrappedV) + 0.5) / 256.0);
        float texel = texture(uAtlas, atlasUV).r * 255.0;
        /* Chroma key: type 24 is non-INCRUST (bit 2 clear), so index 0 is
           written unconditionally — no discard here. */
        float fogDist = max(vZsel - uScaledFogNear, 0.0);
        float fogRow16 = floor(fogDist * uFogRowScale);
        float clutRow = 12.0 + floor(mod(fogRow16, 256.0) / 16.0) * 16.0;
        color = clutLookup(texel, clutRow);
    } else if (uPolyMode == 6) {
        /* Scene shadow span: flat black at uFlatColor alpha. The SW path
            darkens Log through PtrCLUGouraud to (15 - level)/15; alpha =
            level/15 under the alpha blend reproduces that as dst*(1-alpha). */
        color = vec4(0.0, 0.0, 0.0, uFlatColor);
    } else if (uPolyMode == 7) {
        /* Brick atlas (GpuScene): R8G8 page, R = palette index, G = valid.
           AffGraph RLE skips leave G = 0 so the fragment is discarded and
           the FBO keeps whatever was there. NEAREST only. */
        vec2 uv = vTexCoord / vec2(uBrickW, uBrickH);
        vec2 t = texture(uAtlas, uv).rg;
        if (t.y < 0.5) discard;
        color = texture(uPalette, vec2((t.x * 255.0 + 0.5) / 256.0, 0.5));
    } else {
        /* Gouraud table (types 6/7): CLUT lookup, color = column.
            Same +128 rounding bias as polyMode 0. */
        float gouraudRow = floor((vLight + 128.0) / 256.0) + uCLUTBaseRow;
        vec2 clutUV = vec2((uFlatColor + 0.5) / 256.0,
                           (gouraudRow + 0.5) / 256.0);
        float palIdx = texture(uCLUT, clutUV).r * 255.0;
        color = texture(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    }
    /* TRAME stipple: discard every other fragment (50% coverage) */
    if (uAlphaMode == 2) {
        if (mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0) < 1.0)
            discard;
    }
    if (uAlphaMode > 0) {
        color.a = 0.5;
    }
    FragColor = color;
}
##else
/* Replicates the SW CLUT pipeline:
   1. Sample atlas  → 8-bit palette index (texel)
   2. CLUT lookup   → texel lit by gouraud intensity
   3. Palette lookup → ARGB color
   The CLUT is the full 64 KB fog table (256×256). SetCLUT selects a
   16-row block within it, but the gouraud value's high byte indexes
   into the correct rows directly. */
uniform sampler2D uAtlas;   /* 256×256 LUMINANCE */
uniform sampler2D uCLUT;    /* 256×256 LUMINANCE (fog table) */
uniform sampler2D uPalette; /* 256×1 BGRA (paletteLUT) */
uniform int uPolyMode;      /* 0=texture 1=flat 2=gouraud 3=gouraudTable
                               4=textureNoCLUT 5=fogSmooth 6=sceneShadow
                               7=brickQuad */
uniform float uFlatColor;   /* palette index (mode 1) / CLUT column (mode 3) */
uniform float uCLUTBaseRow; /* CLUT row offset for gouraud table (mode 3) */
uniform float uScaledFogNear; /* Fill_ScaledFogNear (fog-smooth distance, mode 5) */
uniform float uFogRowScale;   /* Fill_Fog_Factor / 65536 (fog-smooth distance, mode 5) */
uniform float uBrickW;      /* brick atlas width (mode 7) */
uniform float uBrickH;      /* brick atlas height (mode 7) */
uniform int uAlphaMode;     /* 0=opaque 1=TRANS(semi) 2=TRAME(stipple) */
uniform int uBilinear;      /* 0=nearest 1=bilinear CLUT sampling */
uniform int uChromaKey;     /* 0=no chroma key 1=discard index 0 */
uniform int uPerspCorrect;  /* 1=perspective-correct UV/Z (types 16-24) */
varying vec2 vTexCoord;
varying vec2 vTexCoordOverW;
varying float vInvW;
varying float vLight;
varying float vRepMask;
varying float vZ;
varying float vZOverW;
/* Look up a palette index through the CLUT, returning the final RGBA
   color.  `palIdx` is the raw atlas texel (palette index) and
   `gouraudRow` the CLUT row selected by the gouraud intensity. */
vec4 clutLookup(float palIdx, float gouraudRow) {
    vec2 cuv = vec2((palIdx + 0.5) / 256.0,
                    (gouraudRow + 0.5) / 256.0);
    float lit = texture2D(uCLUT, cuv).r * 255.0;
    return texture2D(uPalette, vec2((lit + 0.5) / 256.0, 0.5));
}
/* Bilinear CLUT sampling: sample 4 atlas texels, CLUT each one, then
   interpolate the resulting RGBA colors.  This avoids the artifact
   where hardware bilinear on palette indices produces garbage colors. */
vec4 bilinearCLUT(vec2 tc, float widthMask, float heightMask,
                  float gouraudRow) {
    float u = mod(tc.x, widthMask + 1.0);
    float v = mod(tc.y, heightMask + 1.0);
    float iu = floor(u);
    float iv = floor(v);
    float fu = u - iu;
    float fv = v - iv;
    /* Wrap each of the 4 texel positions */
    vec2 p00 = vec2(mod(iu, widthMask + 1.0),
                    mod(iv, heightMask + 1.0));
    vec2 p10 = vec2(mod(iu + 1.0, widthMask + 1.0),
                    mod(iv, heightMask + 1.0));
    vec2 p01 = vec2(mod(iu, widthMask + 1.0),
                    mod(iv + 1.0, heightMask + 1.0));
    vec2 p11 = vec2(mod(iu + 1.0, widthMask + 1.0),
                    mod(iv + 1.0, heightMask + 1.0));
    /* Sample atlas → CLUT → palette for each texel */
    float t00 = texture2D(uAtlas, (p00 + 0.5) / 256.0).r * 255.0;
    float t10 = texture2D(uAtlas, (p10 + 0.5) / 256.0).r * 255.0;
    float t01 = texture2D(uAtlas, (p01 + 0.5) / 256.0).r * 255.0;
    float t11 = texture2D(uAtlas, (p11 + 0.5) / 256.0).r * 255.0;
    /* Chroma key: discard if nearest texel (p00) is index 0.
       Matches SW nearest-neighbor behaviour for INCRUST types. */
    if (uChromaKey != 0 && t00 < 0.5) discard;
    vec4 c00 = clutLookup(t00, gouraudRow);
    vec4 c10 = clutLookup(t10, gouraudRow);
    vec4 c01 = clutLookup(t01, gouraudRow);
    vec4 c11 = clutLookup(t11, gouraudRow);
    return mix(mix(c00, c10, fu), mix(c01, c11, fu), fv);
}
void main() {
    vec4 color;
    float alphaOverride = 0.0;
    /* Select perspective-corrected or affine texture/Z varyings.
       uPerspCorrect is 1 for Z-textured types (16-24), where the SW
       rasterizer divides MapU/MapV/ZBuf by W = 1/Z; types 8-15
       (SW POLYTEXT/POLYGTEX) interpolate affinely. */
    vec2 tc = mix(vTexCoord, vTexCoordOverW / vInvW, float(uPerspCorrect));
    float vZsel = mix(vZ, vZOverW / vInvW, float(uPerspCorrect));
    if (uPolyMode == 0) {
        /* Textured: atlas → CLUT → palette */
        tc = tc / 256.0;
        float widthMask = mod(vRepMask, 256.0);
        float heightMask = floor(vRepMask / 256.0);
        /* SW init: Fill_CurGouraudMin = (Pt_Light << 8) + 0x8000.
           The +128 (0x80) rounding bias shifts the CLUT row by 1 in some
           cases — same bias applied in polyMode 2. */
        float gouraudRow = floor((vLight + 128.0) / 256.0) + uCLUTBaseRow;
        if (uBilinear != 0) {
            color = bilinearCLUT(tc, widthMask, heightMask, gouraudRow);
        } else {
            float wrappedU = mod(tc.x, widthMask + 1.0);
            float wrappedV = mod(tc.y, heightMask + 1.0);
            vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,
                                (floor(wrappedV) + 0.5) / 256.0);
            float texel = texture2D(uAtlas, atlasUV).r * 255.0;
            /* Chroma key: atlas texel 0 = transparent (skipped by SW rasterizer
               at the Log buffer level).  Discard before CLUT so the fog table
               cannot remap index 0 to an opaque value.
               Only INCRUST types (12-15, 20-23) have chroma key; non-INCRUST
               types (8-11, 16-19, 24) write all texels including index 0. */
            if (uChromaKey != 0 && texel < 0.5) discard;
            color = clutLookup(texel, gouraudRow);
        }
    } else if (uPolyMode == 1) {
        /* Flat solid: single palette color */
        float palIdx = uFlatColor;
        color = texture2D(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    } else if (uPolyMode == 2) {
        /* Gouraud (types 4/5): palette = (color + (light+128)/256).
           SW init: Fill_CurGouraudMin = (Pt_Light << 8) + 0x8000 + (color << 16).
           The +0x8000 rounding bias means the palette index is
           (color + (Pt_Light + 128) / 256), not (color + Pt_Light / 256). */
        float palIdx = mod(uFlatColor + floor((vLight + 128.0) / 256.0), 256.0);
        color = texture2D(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    } else if (uPolyMode == 4) {
        /* Texture without CLUT (types 8 and 12):
           SW Filler_Texture / Filler_TextureChromaKey writes texel as palette
           index directly (*line = texel).  No CLUT lookup — the atlas texel
           IS the palette index.  Chroma key controlled by uChromaKey. */
        tc = tc / 256.0;
        float widthMask = mod(vRepMask, 256.0);
        float heightMask = floor(vRepMask / 256.0);
        float wrappedU = mod(tc.x, widthMask + 1.0);
        float wrappedV = mod(tc.y, heightMask + 1.0);
        vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,
                            (floor(wrappedV) + 0.5) / 256.0);
        float palIdx = texture2D(uAtlas, atlasUV).r * 255.0;
        /* Chroma key: atlas texel 0 = transparent.
           Only INCRUST types have chroma key. */
        if (uChromaKey != 0 && palIdx < 0.5) discard;
        color = texture2D(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    } else if (uPolyMode == 5) {
        /* Texture Z Fog Smooth (type 24, e.g. sea/sky):
           SW Filler_TextureZFogSmoothZBuf / ...NZW use a fixed CLUT base
           (PtrCLUTFog + 12*256) plus a per-pixel distance-based fog row,
           IGNORING Pt_Light entirely.  Replicating the SW math:
           zBufVal = interpolated Pt_ZO (the batch aZ)
           fogDist = max(zBufVal - Fill_ScaledFogNear, 0)
           fogRow  = (fogDist * Fill_Fog_Factor) >> 8, masked & 0xF000
           CLUT index = PtrCLUTFog + 12*256 + fogRow + texel
           As a CLUT row: (12*256 + fogRow)/256 = 12 + fogRow/256, where
           fogRow/256 = ((fogDist * Fill_Fog_Factor) >> 16) & 0xF0.
           uFogRowScale = Fill_Fog_Factor / 65536, so
           floor(zDist * uFogRowScale) = (fogDist * Fill_Fog_Factor) >> 16. */
        tc = tc / 256.0;
        float widthMask = mod(vRepMask, 256.0);
        float heightMask = floor(vRepMask / 256.0);
        float wrappedU = mod(tc.x, widthMask + 1.0);
        float wrappedV = mod(tc.y, heightMask + 1.0);
        vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,
                            (floor(wrappedV) + 0.5) / 256.0);
        float texel = texture2D(uAtlas, atlasUV).r * 255.0;
        /* Chroma key: type 24 is non-INCRUST (bit 2 clear), so index 0 is
           written unconditionally — no discard here. */
        float fogDist = max(vZsel - uScaledFogNear, 0.0);
        float fogRow16 = floor(fogDist * uFogRowScale);
        float clutRow = 12.0 + floor(mod(fogRow16, 256.0) / 16.0) * 16.0;
        color = clutLookup(texel, clutRow);
    } else if (uPolyMode == 6) {
        /* Scene shadow span: flat black at uFlatColor alpha. The SW path
            darkens Log through PtrCLUGouraud to (15 - level)/15; alpha =
            level/15 under the alpha blend reproduces that as dst*(1-alpha). */
        color = vec4(0.0, 0.0, 0.0, uFlatColor);
    } else if (uPolyMode == 7) {
        /* Brick atlas (GpuScene): R8G8 page, R = palette index, G = valid.
           AffGraph RLE skips leave G = 0 so the fragment is discarded and
           the FBO keeps whatever was there. NEAREST only. */
        vec2 uv = vTexCoord / vec2(uBrickW, uBrickH);
        vec2 t = texture2D(uAtlas, uv).rg;
        if (t.y < 0.5) discard;
        color = texture2D(uPalette, vec2((t.x * 255.0 + 0.5) / 256.0, 0.5));
    } else {
        /* Gouraud table (types 6/7): CLUT lookup, color = column.
            Same +128 rounding bias as polyMode 0. */
        float gouraudRow = floor((vLight + 128.0) / 256.0) + uCLUTBaseRow;
        vec2 clutUV = vec2((uFlatColor + 0.5) / 256.0,
                           (gouraudRow + 0.5) / 256.0);
        float palIdx = texture2D(uCLUT, clutUV).r * 255.0;
        color = texture2D(uPalette,
                 vec2((palIdx + 0.5) / 256.0, 0.5));
    }
    /* TRAME stipple: discard every other fragment (50% coverage) */
    if (uAlphaMode == 2) {
        if (mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0) < 1.0)
            discard;
    }
    if (uAlphaMode > 0) {
        color.a = 0.5;
    }
    gl_FragColor = color;
}
##endif