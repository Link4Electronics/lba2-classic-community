//──────────────────────────────────────────────────────────────────────────
// GPU renderer: OpenGL / GLES 2.0 backend
//──────────────────────────────────────────────────────────────────────────
// Implements the GPU/GPURENDERER_BACKEND vtable (GpuRendererBackendGL) on the
// GL API. This file is pure drawing: it binds the FBO, uploads the batch,
// sets shader state and draws, plus the window composite. Every behavioural
// decision — polygon-type dispatch, GpuDrawParams resolution, batch flush
// triggers, texture-upload timing, the frame sequence and the readback
// post-process — lives in the common layer (LIB386/COMMON) and arrives here
// precomputed. That layer also owns the SW fallbacks; a triangle reaching
// DrawTriangles is by definition taking the GL path.
//──────────────────────────────────────────────────────────────────────────
#include <GL/RENDER_GL.H>

#include <SYSTEM/LOG.H>
#include <SYSTEM/WINDOW.H>
#include <GPU/GPURENDERER_BACKEND.H>

#include <stdio.h>

/* ── Renderer state shared with the engine (C linkage) ─────────────────────
   Owned by the GL renderer even in software builds (gly is always built, as
   stubs when GPURENDERER is off) so every consumer links against one
   definition. DisplayTexFilter selects bilinear CLUT/overlay sampling in GL
   mode; RenderQuality is the FBO supersample factor; GpuScene gates the
   phased move of scene layers off the software Log path (both GPU backends
   read it, like the two above); AtlasTextureDirty is raised by
   DoTextureAnimation when the 256x256 atlas page is mutated in place. */
extern "C" {
S32 DisplayTexFilter = TRUE; /* bilinear by default in GL mode */
S32 RenderQuality = 1;       /* native resolution by default */
S32 GpuScene = 0;            /* hybrid Log path until phases enable layers */
S32 AtlasTextureDirty = 0;
}

#ifdef USE_GPURENDERER

/* ── GL headers ───────────────────────────────────────────────────────────
   The GL dialect is a build decision: CMake defines USE_GLES2 for Android
   (this file never names a platform, per CODESTYLE "Layers"). USE_GLES2 picks
   the SDL GLES2 header set, otherwise the desktop SDL_opengl.h. */
#ifdef USE_GLES2
#include <SDL3/SDL_opengles2.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL_opengl.h>
#endif

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* paletteLUT builds 0xAARRGGBB which in little-endian memory is
   [B, G, R, A] — use GL_BGRA to match the byte layout. */
#ifdef GL_BGRA
#define FMT_BGRA GL_BGRA
#else
#define FMT_BGRA 0x80E1 /* GL_BGRA_EXT */
#endif

/* ── Dialect capability probes ────────────────────────────────────────────
   GLES 2.0 core has no unpack row stride (it arrives with
   GL_EXT_unpack_subimage, under the *_EXT token) and no BGRA texel source
   (GL_EXT_texture_format_BGRA8888). Desktop GL has both outright, so it
   never probes: the flags stay 1 and every upload below takes the same path
   it always did. A GLES driver missing either gets the CPU-side upload
   instead of a dropped frame — probed once per context. */
static S32 s_capsBgra = 1;      /* BGRA texel sources accepted as-is */
static S32 s_capsRowLength = 1; /* unpack row stride expressible */

#ifdef USE_GLES2
#define UNPACK_ROW_LENGTH GL_UNPACK_ROW_LENGTH_EXT
#else
#define UNPACK_ROW_LENGTH GL_UNPACK_ROW_LENGTH
#endif

/* Source format for a texture whose engine-side bytes are BGRA: native on a
   driver with the extension, GL_RGBA for one that only takes RGBA (the
   callers then swizzle — see SwizzleBGRAtoRGBA). With no data at allocation
   time this only has to be a format the context accepts. */
static GLenum BgraSrcFormat(void) {
    return s_capsBgra ? (GLenum)FMT_BGRA : (GLenum)GL_RGBA;
}

/* 0xAARRGGBB is [B, G, R, A] in memory; GL_RGBA reads that as B and R
   swapped. Rewrites a caller-owned copy — never engine palette state. */
static void SwizzleBGRAtoRGBA(U32 *pixels, U32 count) {
    U32 i;
    for (i = 0; i < count; i++) {
        const U32 v = pixels[i];
        pixels[i] = (v & 0xFF00FF00u) | ((v & 0x000000FFu) << 16) |
                    ((v & 0x00FF0000u) >> 16);
    }
}

/* ── Shader sources (GL 2.1 / GLES 2.0 compatible) ─────────────────────── */

#ifdef USE_GLES2
#define GLSL_VERT_HEADER "#version 100\n"
/* Highp where the fragment stage offers it — mediump is fp16 on real mobile
   GPUs, and fp16 atlas coordinates quantise to whole texels at this atlas
   size. Drivers without highp still get a working mediump shader. */
#define GLSL_FRAG_HEADER                  \
    "#version 100\n"                      \
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
    "precision highp float;\n"            \
    "#else\n"                             \
    "precision mediump float;\n"          \
    "#endif\n"
#else
#define GLSL_VERT_HEADER "#version 110\n"
#define GLSL_FRAG_HEADER "#version 110\n"
#endif

/* ── Present shader (2D overlay quad) ───────────────────────────────────── */

static const char *s_vertSrc =
    GLSL_VERT_HEADER
    "attribute vec2 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

/* Present composite: samples the FBO colour (ARGB path) or the Log overlay
   (either format) and, on the R8 path, resolves colour through the palette
   LUT. uIndexed/uLinear/uOpaqueRect/uSize mirror present.frag.glsl so both
   backends implement the same modal-rect and bilinear rules. */
static const char *s_fragSrc =
    GLSL_FRAG_HEADER
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uPalette;\n"
    "uniform int uIndexed;\n"
    "uniform int uLinear;\n"
    "uniform vec4 uOpaqueRect;\n" /* x0, y0, x1, y1 — x1<=x0 inactive */
    "uniform vec2 uSize;\n"
    "varying vec2 vTexCoord;\n"
    "vec4 paletteLookup(float idx) {\n"
    "    return texture2D(uPalette, vec2((idx + 0.5) / 256.0, 0.5));\n"
    "}\n"
    /* One texel of the R8 index page, resolved through the LUT, then the
       modal full-2D UI rule (patch-then-filter: each source texel is fixed
       up before the four-tap mix, matching a CPU patch of ARGB staging
       before hardware bilinear). */
    "vec4 indexedTexel(vec2 p) {\n"
    "    p = clamp(p, vec2(0.0), uSize - 1.0);\n"
    "    float idx = texture2D(uTex, (p + 0.5) / uSize).r * 255.0;\n"
    "    vec4 color = paletteLookup(idx);\n"
    "    if (uOpaqueRect.z > uOpaqueRect.x && uOpaqueRect.w > uOpaqueRect.y &&\n"
    "        p.x >= uOpaqueRect.x && p.x < uOpaqueRect.z &&\n"
    "        p.y >= uOpaqueRect.y && p.y < uOpaqueRect.w && color.a == 0.0)\n"
    "        return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    return color;\n"
    "}\n"
    "void main() {\n"
    "    vec4 color;\n"
    "    if (uIndexed == 0) {\n"
    "        color = texture2D(uTex, vTexCoord);\n"
    "        vec2 pixel = vTexCoord * uSize;\n"
    "        if (uOpaqueRect.z > uOpaqueRect.x && uOpaqueRect.w > uOpaqueRect.y &&\n"
    "            pixel.x >= uOpaqueRect.x && pixel.x < uOpaqueRect.z &&\n"
    "            pixel.y >= uOpaqueRect.y && pixel.y < uOpaqueRect.w && color.a == 0.0)\n"
    "            color = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    } else if (uLinear != 0) {\n"
    "        vec2 texel = vTexCoord * uSize - 0.5;\n"
    "        vec2 base = floor(texel);\n"
    "        vec2 f = texel - base;\n"
    "        vec4 c00 = indexedTexel(base);\n"
    "        vec4 c10 = indexedTexel(base + vec2(1.0, 0.0));\n"
    "        vec4 c01 = indexedTexel(base + vec2(0.0, 1.0));\n"
    "        vec4 c11 = indexedTexel(base + vec2(1.0, 1.0));\n"
    "        color = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);\n"
    "    } else {\n"
    "        color = indexedTexel(floor(vTexCoord * uSize));\n"
    "    }\n"
    "    gl_FragColor = color;\n"
    "}\n";

/* ── CLUT shader (3D Gouraud-textured polygons) ─────────────────────────── */
/* Replicates the SW CLUT pipeline:
   1. Sample atlas  → 8-bit palette index (texel)
   2. CLUT lookup   → texel lit by gouraud intensity
   3. Palette lookup → ARGB color
   The CLUT is the full 64 KB fog table (256×256). SetCLUT selects a
   16-row block within it, but the gouraud value's high byte indexes
   into the correct rows directly. */

static const char *s_clutVertSrc =
    GLSL_VERT_HEADER
    "attribute vec2 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute float aLight;\n"
    "attribute float aZ;\n"
    "attribute float aRepMask;\n"
    "uniform vec2 uScreenScale;\n" /* 2.0/resX, 2.0/resY */
    /* Perspective-correct interpolation: the SW rasterizer
       (POLYTZF/POLYTEXZ, Z-textured types 16-24) interpolates
       MapU/MapV/ZBuf over W = 1/Z.  w=1.0 in clip space makes varyings
       interpolate affinely in screen space, so premultiply by 1/Z here
       and divide in the fragment shader.  aZ is GET_ZO(Z0) (proportional
       to Z0); the constant of proportionality cancels in the fragment
       division.  uPerspCorrect selects the corrected pair; otherwise the
       raw affine varyings are used (types 8-15, SW POLYTEXT/POLYGTEX). */
    "varying vec2 vTexCoord;\n"
    "varying vec2 vTexCoordOverW;\n"
    "varying float vInvW;\n"
    "varying float vLight;\n"
    "varying float vRepMask;\n"
    "varying float vZ;\n"
    "varying float vZOverW;\n"
    "void main() {\n"
    "    float cx = aPos.x * uScreenScale.x - 1.0;\n"
    "    float cy = 1.0 - aPos.y * uScreenScale.y;\n"
    "    gl_Position = vec4(cx, cy, 1.0 - aZ / 65535.0, 1.0);\n"
    "    float invW = 1.0 / max(aZ, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vTexCoordOverW = aTexCoord * invW;\n"
    "    vInvW = invW;\n"
    "    vLight = aLight;\n"
    "    vRepMask = aRepMask;\n"
    "    vZ = aZ;\n"
    "    vZOverW = aZ * invW;\n"
    "}\n";

static const char *s_clutFragSrc =
    GLSL_FRAG_HEADER
    "uniform sampler2D uAtlas;\n"       /* 256×256 LUMINANCE */
    "uniform sampler2D uCLUT;\n"        /* 256×256 LUMINANCE (fog table) */
    "uniform sampler2D uPalette;\n"     /* 256×1 BGRA (scene palette, alpha = index) */
    "uniform sampler2D uBlendSrcLog;\n" /* LUMINANCE_ALPHA: .r = walked source index, .a = Log index */
    "uniform sampler2D uBlendDst;\n"    /* per-sprite FBO rect copy, A = destination index */
    "uniform sampler2D uTransPal;\n"    /* 256×256 LUMINANCE blend table: row = source, column = destination */
    "uniform int uPolyMode;\n"          /* 0=texture 1=flat 2=gouraud 3=gouraudTable 4=textureNoCLUT 5=fogSmooth 6=sceneShadow 7=brickQuad 8=blendSprite */
    "uniform float uFlatColor;\n"       /* palette index (mode 1) / CLUT column (mode 3) */
    "uniform float uCLUTBaseRow;\n"     /* CLUT row offset for gouraud table (mode 3) */
    "uniform float uFlatRow;\n"         /* mode 0: >= 0 pins the CLUT row, -1 = light-derived */
    "uniform float uScaledFogNear;\n"   /* Fill_ScaledFogNear (fog-smooth distance, mode 5) */
    "uniform float uFogRowScale;\n"     /* Fill_Fog_Factor / 65536 (fog-smooth distance, mode 5) */
    "uniform int uAlphaMode;\n"         /* 0=opaque 1=TRANS(semi) 2=TRAME(stipple) */
    "uniform int uBilinear;\n"          /* 0=nearest 1=bilinear CLUT sampling */
    "uniform int uChromaKey;\n"         /* 0=no chroma key 1=discard index 0 */
    "uniform int uPerspCorrect;\n"      /* 1=perspective-correct UV/Z (types 16-24) */
    "uniform vec2 uBrickSize;\n"        /* brick atlas page size (mode 7) */
    "uniform float uBlendLogW;\n"       /* blend page width in pixels (mode 8) */
    "uniform float uBlendLogH;\n"       /* blend page height in pixels (mode 8) */
    "uniform float uBlendDstW;\n"       /* dst rect width in pixels (mode 8) */
    "uniform float uBlendDstH;\n"       /* dst rect height in pixels (mode 8) */
    "varying vec2 vTexCoord;\n"
    "varying vec2 vTexCoordOverW;\n"
    "varying float vInvW;\n"
    "varying float vLight;\n"
    "varying float vRepMask;\n"
    "varying float vZ;\n"
    "varying float vZOverW;\n"
    /* Look up a palette index through the CLUT, returning the final RGBA
       color.  `palIdx` is the raw atlas texel (palette index) and
       `gouraudRow` the CLUT row selected by the gouraud intensity. */
    "vec4 clutLookup(float palIdx, float gouraudRow) {\n"
    "    vec2 cuv = vec2((palIdx + 0.5) / 256.0,\n"
    "                     (gouraudRow + 0.5) / 256.0);\n"
    "    float lit = texture2D(uCLUT, cuv).r * 255.0;\n"
    "    return texture2D(uPalette, vec2((lit + 0.5) / 256.0, 0.5));\n"
    "}\n"
    /* Bilinear CLUT sampling: sample 4 atlas texels, CLUT each one, then
       interpolate the resulting RGBA colors.  This avoids the artifact
       where hardware bilinear on palette indices produces garbage colors. */
    "vec4 bilinearCLUT(vec2 tc, float widthMask, float heightMask,\n"
    "                   float gouraudRow) {\n"
    "    float u = mod(tc.x, widthMask + 1.0);\n"
    "    float v = mod(tc.y, heightMask + 1.0);\n"
    "    float iu = floor(u);\n"
    "    float iv = floor(v);\n"
    "    float fu = u - iu;\n"
    "    float fv = v - iv;\n"
    "    /* Wrap each of the 4 texel positions */\n"
    "    vec2 p00 = vec2(mod(iu, widthMask + 1.0),\n"
    "                     mod(iv, heightMask + 1.0));\n"
    "    vec2 p10 = vec2(mod(iu + 1.0, widthMask + 1.0),\n"
    "                     mod(iv, heightMask + 1.0));\n"
    "    vec2 p01 = vec2(mod(iu, widthMask + 1.0),\n"
    "                     mod(iv + 1.0, heightMask + 1.0));\n"
    "    vec2 p11 = vec2(mod(iu + 1.0, widthMask + 1.0),\n"
    "                     mod(iv + 1.0, heightMask + 1.0));\n"
    "    /* Sample atlas → CLUT → palette for each texel */\n"
    "    float t00 = texture2D(uAtlas, (p00 + 0.5) / 256.0).r * 255.0;\n"
    "    float t10 = texture2D(uAtlas, (p10 + 0.5) / 256.0).r * 255.0;\n"
    "    float t01 = texture2D(uAtlas, (p01 + 0.5) / 256.0).r * 255.0;\n"
    "    float t11 = texture2D(uAtlas, (p11 + 0.5) / 256.0).r * 255.0;\n"
    /* Chroma key: discard if nearest texel (p00) is index 0.
       Matches SW nearest-neighbor behaviour for INCRUST types. */
    "    if (uChromaKey != 0 && t00 < 0.5) discard;\n"
    "    vec4 c00 = clutLookup(t00, gouraudRow);\n"
    "    vec4 c10 = clutLookup(t10, gouraudRow);\n"
    "    vec4 c01 = clutLookup(t01, gouraudRow);\n"
    "    vec4 c11 = clutLookup(t11, gouraudRow);\n"
    "    return mix(mix(c00, c10, fu), mix(c01, c11, fu), fv);\n"
    "}\n"
    "void main() {\n"
    "    vec4 color;\n"
    "    float alphaOverride = 0.0;\n"
    /* Select perspective-corrected or affine texture/Z varyings.
       uPerspCorrect is 1 for Z-textured types (16-24), where the SW
       rasterizer divides MapU/MapV/ZBuf by W = 1/Z; types 8-15
       (SW POLYTEXT/POLYGTEX) interpolate affinely. */
    "    vec2 tc = mix(vTexCoord, vTexCoordOverW / vInvW, float(uPerspCorrect));\n"
    "    float vZsel = mix(vZ, vZOverW / vInvW, float(uPerspCorrect));\n"
    "    if (uPolyMode == 0) {\n"
    /* Textured: atlas → CLUT → palette */
    "        tc = tc / 256.0;\n"
    "        float widthMask = mod(vRepMask, 256.0);\n"
    "        float heightMask = floor(vRepMask / 256.0);\n"
    /* SW init: Fill_CurGouraudMin = (Pt_Light << 8) + 0x8000.
        The +128 (0x80) rounding bias shifts the CLUT row by 1 in some
        cases — same bias applied in polyMode 2. */
    "        float gouraudRow = floor((vLight + 128.0) / 256.0) + uCLUTBaseRow;\n"
    /* SW flat-textured fillers (Filler_TextureFlat / Filler_TextureZFlat)
       index the CLUT at the batch base row and never read Pt_Light, so
       uFlatRow >= 0 pins the row for those types. */
    "        if (uFlatRow >= 0.0) {\n"
    "            gouraudRow = uFlatRow;\n"
    "        }\n"
    "        if (uBilinear != 0) {\n"
    "            color = bilinearCLUT(tc, widthMask, heightMask, gouraudRow);\n"
    "        } else {\n"
    "            float wrappedU = mod(tc.x, widthMask + 1.0);\n"
    "            float wrappedV = mod(tc.y, heightMask + 1.0);\n"
    "            vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,\n"
    "                                (floor(wrappedV) + 0.5) / 256.0);\n"
    "            float texel = texture2D(uAtlas, atlasUV).r * 255.0;\n"
    /* Chroma key: atlas texel 0 = transparent (skipped by SW rasterizer
       at the Log buffer level).  Discard before CLUT so the fog table
       cannot remap index 0 to an opaque value.
       Only INCRUST types (12-15, 20-23) have chroma key; non-INCRUST
       types (8-11, 16-19, 24) write all texels including index 0. */
    "            if (uChromaKey != 0 && texel < 0.5) discard;\n"
    "            color = clutLookup(texel, gouraudRow);\n"
    "        }\n"
    "    } else if (uPolyMode == 1) {\n"
    /* Flat solid: single palette color */
    "        float palIdx = uFlatColor;\n"
    "        color = texture2D(uPalette,\n"
    "                 vec2((palIdx + 0.5) / 256.0, 0.5));\n"
    "    } else if (uPolyMode == 2) {\n"
    /* Gouraud (types 4/5): palette = (color + (light+128)/256).
       SW init: Fill_CurGouraudMin = (Pt_Light << 8) + 0x8000 + (color << 16).
       The +0x8000 rounding bias means the palette index is
       (color + (Pt_Light + 128) / 256), not (color + Pt_Light / 256). */
    "        float palIdx = mod(uFlatColor + floor((vLight + 128.0) / 256.0), 256.0);\n"
    "        color = texture2D(uPalette,\n"
    "                 vec2((palIdx + 0.5) / 256.0, 0.5));\n"
    "    } else if (uPolyMode == 4) {\n"
    /* Texture without CLUT (types 8 and 12):
       SW Filler_Texture / Filler_TextureChromaKey writes texel as palette
       index directly (*line = texel).  No CLUT lookup — the atlas texel
       IS the palette index.  Chroma key controlled by uChromaKey. */
    "        tc = tc / 256.0;\n"
    "        float widthMask = mod(vRepMask, 256.0);\n"
    "        float heightMask = floor(vRepMask / 256.0);\n"
    "        float wrappedU = mod(tc.x, widthMask + 1.0);\n"
    "        float wrappedV = mod(tc.y, heightMask + 1.0);\n"
    "        vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,\n"
    "                            (floor(wrappedV) + 0.5) / 256.0);\n"
    "        float palIdx = texture2D(uAtlas, atlasUV).r * 255.0;\n"
    /* Chroma key: atlas texel 0 = transparent.
       Only INCRUST types have chroma key. */
    "        if (uChromaKey != 0 && palIdx < 0.5) discard;\n"
    "        color = texture2D(uPalette,\n"
    "                 vec2((palIdx + 0.5) / 256.0, 0.5));\n"
    "    } else if (uPolyMode == 5) {\n"
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
    "        tc = tc / 256.0;\n"
    "        float widthMask = mod(vRepMask, 256.0);\n"
    "        float heightMask = floor(vRepMask / 256.0);\n"
    "        float wrappedU = mod(tc.x, widthMask + 1.0);\n"
    "        float wrappedV = mod(tc.y, heightMask + 1.0);\n"
    "        vec2 atlasUV = vec2((floor(wrappedU) + 0.5) / 256.0,\n"
    "                            (floor(wrappedV) + 0.5) / 256.0);\n"
    "        float texel = texture2D(uAtlas, atlasUV).r * 255.0;\n"
    /* Chroma key: type 24 is non-INCRUST (bit 2 clear), so index 0 is
       written unconditionally — no discard here. */
    "        float fogDist = max(vZsel - uScaledFogNear, 0.0);\n"
    "        float fogRow16 = floor(fogDist * uFogRowScale);\n"
    "        float clutRow = 12.0 + floor(mod(fogRow16, 256.0) / 16.0) * 16.0;\n"
    "        color = clutLookup(texel, clutRow);\n"
    "    } else if (uPolyMode == 6) {\n"
    /* Scene shadow span: flat black alpha-blended at uFlatColor alpha. The
       alpha is the CLUT row's average luminance loss (see Common_ShadowAlpha),
       so blending shadow * (1 - alpha) matches the ShadeBoxBlk darkening. */
    "        color = vec4(0.0, 0.0, 0.0, uFlatColor);\n"
    "    } else if (uPolyMode == 7) {\n"
    /* Brick atlas: R = palette index, G = valid (AffGraph RLE skip).
       NEAREST only; MapU/MapV are absolute texel coords. LUMINANCE_ALPHA
       packs index in .r and valid in .a. The valid byte is 0 or 1 (not
       0 or 255), so after UNORM normalize the flag is 0.0 or 1/255 —
       compare against half a step, not 0.5, or every brick discards. */
    "        vec2 uv = vTexCoord / uBrickSize;\n"
    "        vec2 t = texture2D(uAtlas, uv).ra;\n"
    "        if (t.y < (0.5 / 255.0)) discard;\n"
    "        color = texture2D(uPalette, vec2((t.x * 255.0 + 0.5) / 256.0, 0.5));\n"
    "    } else if (uPolyMode == 8) {\n"
    /* Blend sprite (EXTRA_TRANSPARENT): integer ScaleSpriteTransp
       equivalent. vTexCoord is the local rect pixel coordinate; the CPU
       walk put the sampled source index (.r) and the live Log index (.a)
       in uBlendSrcLog, the scene's pre-draw destination index sits in
       uBlendDst.a (Log wins where the software path read Log), and
       uTransPal is the 64 KB transpTable (row = source, column =
       destination). Palette index 0 is skipped by the software walk —
       discard reproduces that. The result color's alpha carries the
       result index so a chained blend sprite sees it as destination. */
    "        vec2 px = floor(vTexCoord);\n"
    "        vec2 sl = texture2D(uBlendSrcLog,\n"
    "                            (px + 0.5) / vec2(uBlendLogW, uBlendLogH)).ra;\n"
    "        float srcIdx = floor(sl.x * 255.0 + 0.5);\n"
    "        if (srcIdx < 0.5) discard;\n"
    "        float logIdx = floor(sl.y * 255.0 + 0.5);\n"
    "        float dstIdx = floor(texture2D(uBlendDst,\n"
    "                                       (px + 0.5) / vec2(uBlendDstW, uBlendDstH))\n"
    "                              .a * 255.0 + 0.5);\n"
    "        if (logIdx >= 0.5) dstIdx = logIdx;\n"
    "        float result = floor(texture2D(uTransPal,\n"
    "                                       vec2((dstIdx + 0.5) / 256.0,\n"
    "                                            (srcIdx + 0.5) / 256.0))\n"
    "                                 .r * 255.0 + 0.5);\n"
    "        color = texture2D(uPalette, vec2((result + 0.5) / 256.0, 0.5));\n"
    "        color.a = result / 255.0;\n"
    "    } else {\n"
    /* Gouraud table (types 6/7): CLUT lookup, color = column.
       Same +128 rounding bias as polyMode 0. */
    "        float gouraudRow = floor((vLight + 128.0) / 256.0) + uCLUTBaseRow;\n"
    "        vec2 clutUV = vec2((uFlatColor + 0.5) / 256.0,\n"
    "                           (gouraudRow + 0.5) / 256.0);\n"
    "        float palIdx = texture2D(uCLUT, clutUV).r * 255.0;\n"
    "        color = texture2D(uPalette,\n"
    "                 vec2((palIdx + 0.5) / 256.0, 0.5));\n"
    "    }\n"
    /* TRAME stipple: discard every other fragment (50% coverage) */
    "    if (uAlphaMode == 2) {\n"
    "        if (mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0) < 1.0)\n"
    "            discard;\n"
    "    }\n"
    "    if (uAlphaMode > 0) {\n"
    "        color.a = 0.5;\n"
    "    }\n"
    "    gl_FragColor = color;\n"
    "}\n";

/* ── GL state — present shader ──────────────────────────────────────────── */

static GLuint s_program = 0;
static GLuint s_vbo = 0;
static GLuint s_fboVbo = 0;         /* non-flipped UV quad for FBO→screen draw */
static GLuint s_overlayTex = 0;     /* present texture (Log: ARGB or R8 indices) */
static S32 s_overlayIndexed = 0;    /* format of s_overlayTex (recreate on switch) */
static S32 s_overlayFullUpload = 0; /* fresh texture: contents undefined */
static GLint s_uTexLoc = -1;
static GLint s_uPaletteLoc = -1;
static GLint s_uIndexedLoc = -1;
static GLint s_uLinearLoc = -1;
static GLint s_uOpaqueRectLoc = -1;
static GLint s_uSizeLoc = -1;
static GLint s_aPosLoc = -1;
static GLint s_aTexCoordLoc = -1;
static U32 s_texWidth = 0; /* present surface size (engine pixel space) */
static U32 s_texHeight = 0;
static S32 s_initialized = 0;

/* ── GL state — CLUT shader ─────────────────────────────────────────────── */

static GLuint s_clutProgram = 0;
static GLint s_clut_aPos = -1;
static GLint s_clut_aTexCoord = -1;
static GLint s_clut_aLight = -1;
static GLint s_clut_aZ = -1;
static GLint s_clut_aRepMask = -1;
static GLint s_clut_uAtlas = -1;
static GLint s_clut_uCLUT = -1;
static GLint s_clut_uPalette = -1;
static GLint s_clut_uScreenScale = -1;
static GLint s_clut_uPolyMode = -1;
static GLint s_clut_uFlatColor = -1;
static GLint s_clut_uCLUTBaseRow = -1;
static GLint s_clut_uFlatRow = -1;
static GLint s_clut_uScaledFogNear = -1;
static GLint s_clut_uFogRowScale = -1;
static GLint s_clut_uAlphaMode = -1;
static GLint s_clut_uBilinear = -1;
static GLint s_clut_uChromaKey = -1;
static GLint s_clut_uPerspCorrect = -1;
static GLint s_clut_uBrickSize = -1;
static GLint s_clut_uBlendSrcLog = -1;
static GLint s_clut_uBlendDst = -1;
static GLint s_clut_uTransPal = -1;
static GLint s_clut_uBlendLogW = -1;
static GLint s_clut_uBlendLogH = -1;
static GLint s_clut_uBlendDstW = -1;
static GLint s_clut_uBlendDstH = -1;

/* ── GL state — textures ────────────────────────────────────────────────── */

static GLuint s_atlasTex = 0;       /* 256×256 LUMINANCE */
static GLuint s_clutTex = 0;        /* 256×256 LUMINANCE (full fog table) */
static GLuint s_paletteTex = 0;     /* 256×1 BGRA, alpha 0xFF except index 0 (present) */
static GLuint s_clutPaletteTex = 0; /* 256×1 BGRA, alpha = palette index (scene FBO) */
static GLuint s_brickTex = 0;       /* brick atlas, LUMINANCE_ALPHA (GpuScene) */
static U32 s_brickTexW = 0;
static U32 s_brickTexH = 0;
static GLuint s_spriteTex = 0; /* sprite atlas, LUMINANCE_ALPHA (GpuScene) */
static U32 s_spriteTexW = 0;
static U32 s_spriteTexH = 0;
static GLuint s_transPalTex = 0; /* 256×256 LUMINANCE blend table (mode 8) */
static GLuint s_blendLogTex = 0; /* full-screen LUMINANCE_ALPHA src/Log (mode 8) */
static U32 s_blendLogW = 0;
static U32 s_blendLogH = 0;
static GLuint s_blendDstTex = 0; /* full-screen BGRA dst rect copy (mode 8) */
static U32 s_blendDstW = 0;
static U32 s_blendDstH = 0;
static U8 *s_blendReadback = NULL; /* rect staging for TargetCopyColorRect */
static U32 s_blendReadbackBytes = 0;
static U8 *s_uploadScratch = NULL; /* repacked present rows (GLES drivers
                                      without a row stride or BGRA source) */
static U32 s_uploadScratchBytes = 0;

/* ── GL state — batch VBO ───────────────────────────────────────────────── */

static const U32 BATCH_MAX_VERTS = 4096; /* matches the common-layer budget */
static GLuint s_batchVBO = 0;

/* ── GL state — FBO (offscreen target for 3D, SSAA-scaled) ──────────────── */
static GLuint s_fbo = 0;
static GLuint s_fboColorTex = 0;
static GLuint s_fboDepthRbo = 0;
static U32 s_fboWidth = 0;
static U32 s_fboHeight = 0;

/* Terrain snapshot (see GpuRenderer_SnapshotTerrain/RestoreTerrain): colour +
   depth textures the size of the FBO. The colour is copied back over the FBO
   at the head of each AFF_OBJETS exterior frame so the static camera scrubs
   the previous frame's shadow/animated-poly paint; the depth texture keeps the
   stored copy a faithful post-terrain-pass state, even though restore never
   reads it back (the FBO's own depth is unwritten between the two calls). */
static GLuint s_terrainColorTex = 0;
static GLuint s_terrainDepthTex = 0;

/* ── Fullscreen quad: position (x,y) + texcoord (u,v) ───────────────────── */

/* 2D overlay quad — V is flipped because glTexSubImage2D uploads row 0
   (game Y=0, top of screen) at texture V=0 (bottom in OpenGL). Flipping V
   here maps game Y=0 → screen top. */
static const GLfloat s_quadVerts[] = {
    /* pos       texcoord */
    -1.0f, -1.0f, 0.0f, 1.0f, /* bottom-left  (flip Y: texcoord 1.0) */
    1.0f, -1.0f, 1.0f, 1.0f,  /* bottom-right */
    -1.0f, 1.0f, 0.0f, 0.0f,  /* top-left      (flip Y: texcoord 0.0) */
    1.0f, 1.0f, 1.0f, 0.0f,   /* top-right */
};

/* FBO quad — V is NOT flipped. The CLUT vertex shader already maps game
   Y=0 → FBO top (clip Y=+1), so V=1.0 at screen top samples the FBO
   correctly without an extra flip. */
static const GLfloat s_fboQuadVerts[] = {
    /* pos       texcoord */
    -1.0f, -1.0f, 0.0f, 0.0f, /* bottom-left  (V=0.0 = FBO bottom) */
    1.0f, -1.0f, 1.0f, 0.0f,  /* bottom-right */
    -1.0f, 1.0f, 0.0f, 1.0f,  /* top-left      (V=1.0 = FBO top) */
    1.0f, 1.0f, 1.0f, 1.0f,   /* top-right */
};

/* ── Shader helpers ─────────────────────────────────────────────────────── */

static GLuint CompileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        Log_Warn("[GL] glCreateShader failed (%s)",
                 type == GL_VERTEX_SHADER ? "vertex" : "fragment");
        return 0;
    }
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        /* The driver is the only one who knows which line of this dialect's
           source it rejected; without the log a failed context looks like a
           bare "CreateContext failed". */
        char info[512];
        info[0] = '\0';
        glGetShaderInfoLog(shader, sizeof(info), NULL, info);
        Log_Warn("[GL] %s shader compile failed: %s",
                 type == GL_VERTEX_SHADER ? "vertex" : "fragment", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkProgram(GLuint vert, GLuint frag) {
    GLuint prog = glCreateProgram();
    if (prog == 0) {
        return 0;
    }
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[512];
        info[0] = '\0';
        glGetProgramInfoLog(prog, sizeof(info), NULL, info);
        Log_Warn("[GL] program link failed: %s", info);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* ── FBO helpers (SSAA) ─────────────────────────────────────────────────── */

static void GL_DestroyFBO(void) {
    if (s_terrainColorTex) {
        glDeleteTextures(1, &s_terrainColorTex);
        s_terrainColorTex = 0;
    }
    if (s_terrainDepthTex) {
        glDeleteTextures(1, &s_terrainDepthTex);
        s_terrainDepthTex = 0;
    }
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        s_fbo = 0;
    }
    if (s_fboColorTex) {
        glDeleteTextures(1, &s_fboColorTex);
        s_fboColorTex = 0;
    }
    if (s_fboDepthRbo) {
        glDeleteRenderbuffers(1, &s_fboDepthRbo);
        s_fboDepthRbo = 0;
    }
    s_fboWidth = 0;
    s_fboHeight = 0;
}

static void GL_CreateFBO(U32 width, U32 height) {
    GL_DestroyFBO();

    /* Color texture attachment */
    glGenTextures(1, &s_fboColorTex);
    glBindTexture(GL_TEXTURE_2D, s_fboColorTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Depth renderbuffer attachment */
    glGenRenderbuffers(1, &s_fboDepthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, s_fboDepthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          (GLsizei)width, (GLsizei)height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    /* Framebuffer object */
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s_fboColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, s_fboDepthRbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        GL_DestroyFBO();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_fboWidth = width;
    s_fboHeight = height;

    /* Terrain snapshot textures at the FBO size. The depth texture needs
       depth-texture support (GL 2.0 core on desktop, GL_OES_depth_texture on
       GLES — universal on Android); a device without it fails the depth copy
       only, which costs the stored depth, never the colour restore. */
    glGenTextures(1, &s_terrainColorTex);
    glBindTexture(GL_TEXTURE_2D, s_terrainColorTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

#ifndef USE_GLES2
    /* GLES 2.0 has no depth copy — glCopyTexSubImage2D reads the colour
       attachment there — so a depth texture would be allocated and never
       written. Nothing samples it (restore paints colour only), so the
       dialect simply skips it; the s_terrainDepthTex test in the snapshot
       keeps the copy out. */
    glGenTextures(1, &s_terrainDepthTex);
    glBindTexture(GL_TEXTURE_2D, s_terrainDepthTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, (GLsizei)width,
                 (GLsizei)height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
}

/* ── Context lifecycle ──────────────────────────────────────────────────── */

#ifdef USE_GLES2
/* Whole-token search of the context's extension string: a bare strstr would
   report GL_FOO_bar when asked for GL_FOO. */
static S32 GL_ExtensionPresent(const char *name) {
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    const size_t len = strlen(name);
    const char *p = exts;

    if (p == NULL || len == 0) {
        return 0;
    }
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, name, len) == 0 && (p[len] == ' ' || p[len] == '\0')) {
            return 1;
        }
        while (*p != '\0' && *p != ' ') {
            p++;
        }
    }
    return 0;
}
#endif

static S32 GL_CreateContext(U32 resX, U32 resY) {
    if (s_initialized) {
        return TRUE;
    }

    if (!Window_CreateGLContext()) {
        return FALSE;
    }
    if (!Window_GLMakeCurrent()) {
        return FALSE;
    }

    Log_Info("[GL] Initializing renderer (%ux%u)...", resX, resY);

#ifdef USE_GLES2
    /* Probed with the context current, before the first upload consults them.
       Both features have a CPU-side substitute below; the log says which one
       this driver will run. */
    s_capsBgra = GL_ExtensionPresent("GL_EXT_texture_format_BGRA8888");
    s_capsRowLength = GL_ExtensionPresent("GL_EXT_unpack_subimage");
    Log_Info("[GL] GLES 2.0 dialect: BGRA sources %s, row-stride uploads %s",
             s_capsBgra ? "native" : "CPU swizzle",
             s_capsRowLength ? "native" : "CPU repack");
    Log_Info("[GL] GLES 2.0 dialect: no depth readback (glReadPixels takes "
             "colour only) and no terrain depth snapshot (glCopyTexSubImage2D "
             "reads colour only)");
#endif

    /* ── Compile present shader ── */
    GLuint vert = CompileShader(GL_VERTEX_SHADER, s_vertSrc);
    GLuint frag = CompileShader(GL_FRAGMENT_SHADER, s_fragSrc);
    if (vert == 0 || frag == 0) {
        return FALSE;
    }

    s_program = LinkProgram(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (s_program == 0) {
        return FALSE;
    }

    s_uTexLoc = glGetUniformLocation(s_program, "uTex");
    s_uPaletteLoc = glGetUniformLocation(s_program, "uPalette");
    s_uIndexedLoc = glGetUniformLocation(s_program, "uIndexed");
    s_uLinearLoc = glGetUniformLocation(s_program, "uLinear");
    s_uOpaqueRectLoc = glGetUniformLocation(s_program, "uOpaqueRect");
    s_uSizeLoc = glGetUniformLocation(s_program, "uSize");
    s_aPosLoc = glGetAttribLocation(s_program, "aPos");
    s_aTexCoordLoc = glGetAttribLocation(s_program, "aTexCoord");

    /* ── Compile CLUT shader ── */
    GLuint clutVert = CompileShader(GL_VERTEX_SHADER, s_clutVertSrc);
    GLuint clutFrag = CompileShader(GL_FRAGMENT_SHADER, s_clutFragSrc);
    if (clutVert == 0 || clutFrag == 0) {
        return FALSE;
    }

    s_clutProgram = LinkProgram(clutVert, clutFrag);
    glDeleteShader(clutVert);
    glDeleteShader(clutFrag);
    if (s_clutProgram == 0) {
        return FALSE;
    }

    s_clut_aPos = glGetAttribLocation(s_clutProgram, "aPos");
    s_clut_aTexCoord = glGetAttribLocation(s_clutProgram, "aTexCoord");
    s_clut_aLight = glGetAttribLocation(s_clutProgram, "aLight");
    s_clut_aZ = glGetAttribLocation(s_clutProgram, "aZ");
    s_clut_aRepMask = glGetAttribLocation(s_clutProgram, "aRepMask");
    s_clut_uPerspCorrect = glGetUniformLocation(s_clutProgram, "uPerspCorrect");
    s_clut_uAtlas = glGetUniformLocation(s_clutProgram, "uAtlas");
    s_clut_uCLUT = glGetUniformLocation(s_clutProgram, "uCLUT");
    s_clut_uPalette = glGetUniformLocation(s_clutProgram, "uPalette");
    s_clut_uScreenScale = glGetUniformLocation(s_clutProgram, "uScreenScale");
    s_clut_uPolyMode = glGetUniformLocation(s_clutProgram, "uPolyMode");
    s_clut_uFlatColor = glGetUniformLocation(s_clutProgram, "uFlatColor");
    s_clut_uCLUTBaseRow = glGetUniformLocation(s_clutProgram, "uCLUTBaseRow");
    s_clut_uFlatRow = glGetUniformLocation(s_clutProgram, "uFlatRow");
    s_clut_uScaledFogNear = glGetUniformLocation(s_clutProgram, "uScaledFogNear");
    s_clut_uFogRowScale = glGetUniformLocation(s_clutProgram, "uFogRowScale");
    s_clut_uAlphaMode = glGetUniformLocation(s_clutProgram, "uAlphaMode");
    s_clut_uBilinear = glGetUniformLocation(s_clutProgram, "uBilinear");
    s_clut_uChromaKey = glGetUniformLocation(s_clutProgram, "uChromaKey");
    s_clut_uBrickSize = glGetUniformLocation(s_clutProgram, "uBrickSize");
    s_clut_uBlendSrcLog = glGetUniformLocation(s_clutProgram, "uBlendSrcLog");
    s_clut_uBlendDst = glGetUniformLocation(s_clutProgram, "uBlendDst");
    s_clut_uTransPal = glGetUniformLocation(s_clutProgram, "uTransPal");
    s_clut_uBlendLogW = glGetUniformLocation(s_clutProgram, "uBlendLogW");
    s_clut_uBlendLogH = glGetUniformLocation(s_clutProgram, "uBlendLogH");
    s_clut_uBlendDstW = glGetUniformLocation(s_clutProgram, "uBlendDstW");
    s_clut_uBlendDstH = glGetUniformLocation(s_clutProgram, "uBlendDstH");

    Log_Info("[GL] CLUT shader compiled OK (aPos=%d aTexCoord=%d aLight=%d "
             "aZ=%d aRepMask=%d uAtlas=%d uCLUT=%d uPalette=%d uScreenScale=%d).",
             s_clut_aPos, s_clut_aTexCoord, s_clut_aLight, s_clut_aZ,
             s_clut_aRepMask,
             s_clut_uAtlas, s_clut_uCLUT, s_clut_uPalette,
             s_clut_uScreenScale);

    /* ── Create VBOs ── */
    /* Present quad VBO (static) — flipped UVs for 2D overlay */
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_quadVerts), s_quadVerts,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* FBO quad VBO (static) — non-flipped UVs for FBO→screen */
    glGenBuffers(1, &s_fboVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_fboVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_fboQuadVerts), s_fboQuadVerts,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* Batch VBO for 3D triangles (dynamic) */
    glGenBuffers(1, &s_batchVBO);
    glBindBuffer(GL_ARRAY_BUFFER, s_batchVBO);
    glBufferData(GL_ARRAY_BUFFER, BATCH_MAX_VERTS * 14, NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* ── Create fixed-role textures ── */
    /* Atlas texture (256×256, 8-bit LUMINANCE) */
    glGenTextures(1, &s_atlasTex);
    glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, 256, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* CLUT texture (256×256, 8-bit LUMINANCE — full fog table) */
    glGenTextures(1, &s_clutTex);
    glBindTexture(GL_TEXTURE_2D, s_clutTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, 256, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Palette texture (256×1, BGRA — paletteLUT). Present overlay: alpha
       0xFF except index 0 (the Log transparency hole). */
    glGenTextures(1, &s_paletteTex);
    glBindTexture(GL_TEXTURE_2D, s_paletteTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, BgraSrcFormat(),
                 GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Scene palette (256×1, BGRA — alpha byte = palette index). The clut
       program resolves every opaque mode through this copy so the FBO's
       alpha channel carries the destination index mode 8 blends against;
       present keeps the copy above because its overlay blend treats
       alpha 0 as the Log transparency hole. */
    glGenTextures(1, &s_clutPaletteTex);
    glBindTexture(GL_TEXTURE_2D, s_clutPaletteTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, BgraSrcFormat(),
                 GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* TransPal blend table (256×256 LUMINANCE): created on first upload. */

    /* Brick atlas: created on first TexUpdateBricks (size varies). */

    /* Present texture: allocated on first upload, at the present size. */

    /* GL state */
    glViewport(0, 0, (GLsizei)resX, (GLsizei)resY);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    /* Clear to black so the window is never transparent/garbage before the
       first Present call.  Depth clear to 0.0 matches GL_GEQUAL convention
       (closer objects have higher depth values). */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
#ifdef USE_GLES2
    glClearDepthf(0.0f);
#else
    glClearDepth(0.0);
#endif
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    Window_GL_SwapWindow();

    s_texWidth = resX;
    s_texHeight = resY;
    s_initialized = 1;
    Log_Info("[GL] Renderer initialized OK.");
    return TRUE;
}

static void GL_DestroyContext(void) {
    if (!s_initialized) {
        return;
    }

    if (s_overlayTex) {
        glDeleteTextures(1, &s_overlayTex);
        s_overlayTex = 0;
    }
    s_overlayIndexed = 0;
    if (s_atlasTex) {
        glDeleteTextures(1, &s_atlasTex);
        s_atlasTex = 0;
    }
    if (s_clutTex) {
        glDeleteTextures(1, &s_clutTex);
        s_clutTex = 0;
    }
    if (s_paletteTex) {
        glDeleteTextures(1, &s_paletteTex);
        s_paletteTex = 0;
    }
    if (s_clutPaletteTex) {
        glDeleteTextures(1, &s_clutPaletteTex);
        s_clutPaletteTex = 0;
    }
    if (s_transPalTex) {
        glDeleteTextures(1, &s_transPalTex);
        s_transPalTex = 0;
    }
    if (s_blendLogTex) {
        glDeleteTextures(1, &s_blendLogTex);
        s_blendLogTex = 0;
        s_blendLogW = 0;
        s_blendLogH = 0;
    }
    if (s_blendDstTex) {
        glDeleteTextures(1, &s_blendDstTex);
        s_blendDstTex = 0;
        s_blendDstW = 0;
        s_blendDstH = 0;
    }
    if (s_blendReadback) {
        free(s_blendReadback);
        s_blendReadback = NULL;
        s_blendReadbackBytes = 0;
    }
    if (s_brickTex) {
        glDeleteTextures(1, &s_brickTex);
        s_brickTex = 0;
        s_brickTexW = 0;
        s_brickTexH = 0;
    }
    if (s_spriteTex) {
        glDeleteTextures(1, &s_spriteTex);
        s_spriteTex = 0;
        s_spriteTexW = 0;
        s_spriteTexH = 0;
    }
    if (s_vbo) {
        glDeleteBuffers(1, &s_vbo);
        s_vbo = 0;
    }
    if (s_fboVbo) {
        glDeleteBuffers(1, &s_fboVbo);
        s_fboVbo = 0;
    }
    if (s_batchVBO) {
        glDeleteBuffers(1, &s_batchVBO);
        s_batchVBO = 0;
    }
    if (s_program) {
        glDeleteProgram(s_program);
        s_program = 0;
    }
    if (s_clutProgram) {
        glDeleteProgram(s_clutProgram);
        s_clutProgram = 0;
    }

    GL_DestroyFBO();

    Window_DestroyGLContext();
    s_initialized = 0;
    s_texWidth = 0;
    s_texHeight = 0;
    Log_Info("[GL] Renderer shut down.");
}

/* ── Offscreen target (FBO) ─────────────────────────────────────────────── */

static S32 GL_TargetCreate(U32 width, U32 height) {
    GL_CreateFBO(width, height);
    return (s_fbo != 0) ? TRUE : FALSE;
}

static void GL_TargetDestroy(void) {
    GL_DestroyFBO();
}

static S32 GL_TargetIsValid(void) {
    return (s_fbo != 0) ? TRUE : FALSE;
}

static void GL_TargetBindClear(U32 rgb) {
    if (!s_initialized) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    if (s_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, (GLsizei)s_fboWidth, (GLsizei)s_fboHeight);
    } else {
        glViewport(0, 0, (GLsizei)s_texWidth, (GLsizei)s_texHeight);
    }

    /* Scene clear RGB (exterior FogCoul): sky gaps outside the drawn
       horizon must match the software Cls fill, not black. Alpha clears to
       0 = palette index 0, the same value the software Cls leaves in Log
       and the value a blend sprite blends against over sky. */
    glClearColor((float)((rgb >> 16) & 0xFF) / 255.0f,
                 (float)((rgb >> 8) & 0xFF) / 255.0f,
                 (float)(rgb & 0xFF) / 255.0f, 0.0f);
    /* Depth clear to 0.0 matches GL_GEQUAL (closer objects higher). */
#ifdef USE_GLES2
    glClearDepthf(0.0f);
#else
    glClearDepth(0.0);
#endif
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static U32 GL_TargetWidth(void) {
    return s_fboWidth;
}

static U32 GL_TargetHeight(void) {
    return s_fboHeight;
}

/* Raw depth readback. Rows come back bottom-up; the common layer flips them
   and applies the aZ inverse. Readable whenever the FBO has a depth buffer,
   so the result is FALSE only when there is none to read — or, on GLES, when
   the API has no depth readback to give. */
static S32 GL_TargetReadDepth(void *depthDst, U32 width, U32 height) {
#ifdef USE_GLES2
    /* GLES 2.0's glReadPixels accepts colour formats only. */
    (void)depthDst;
    (void)width;
    (void)height;
    return FALSE;
#else
    if (!s_initialized || !s_fbo || depthDst == NULL) {
        return FALSE;
    }
    if (!Window_GLMakeCurrent()) {
        return FALSE;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_SHORT, depthDst);
    return TRUE;
#endif
}

/* Raw colour readback: U32 0x00RRGGBB, bottom-up (GL origin). The common
   layer flips to top-down and box-downsamples when SSAA is on. */
static S32 GL_TargetReadColor(void *colorDst, U32 width, U32 height) {
    if (!s_initialized || !s_fbo || colorDst == NULL) {
        return FALSE;
    }
    if (!Window_GLMakeCurrent()) {
        return FALSE;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    /* GL_RGBA bytes are R,G,B,A; swizzle into 0x00RRGGBB per pixel. GL
       bottom-up rows already match the TargetReadColor contract. */
    static U8 *s_rgbaScratch = NULL;
    static U32 s_rgbaScratchBytes = 0;
    const U32 need = width * height * 4;
    if (s_rgbaScratchBytes < need) {
        free(s_rgbaScratch);
        s_rgbaScratch = (U8 *)malloc(need);
        s_rgbaScratchBytes = s_rgbaScratch ? need : 0;
    }
    if (s_rgbaScratch == NULL) {
        return FALSE;
    }
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA,
                 GL_UNSIGNED_BYTE, s_rgbaScratch);
    const U32 count = width * height;
    U32 *dst = (U32 *)colorDst;
    for (U32 i = 0; i < count; i++) {
        const U8 *p = s_rgbaScratch + i * 4;
        dst[i] = ((U32)p[0] << 16) | ((U32)p[1] << 8) | (U32)p[2];
    }
    return TRUE;
}

/* Copies the offscreen target into the persistent terrain target. Both are
   glCopyTexSubImage2D reads from the bound main FBO, so no intermediate quad
   or second framebuffer is needed. */
static S32 GL_TargetSnapshotTerrain(void) {
    if (!s_initialized || !s_fbo || !s_terrainColorTex) {
        return FALSE;
    }
    if (!Window_GLMakeCurrent()) {
        return FALSE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    /* 16-bit depth rows need a 1-byte unpack alignment (and colour is 4 bytes
       anyway); restore the default after the pair of copies. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, s_terrainColorTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                        (GLsizei)s_fboWidth, (GLsizei)s_fboHeight);
    /* Absent when the dialect has no depth copy (see GL_CreateFBO): the
       colour restore below is the one that scrubs the frame. */
    if (s_terrainDepthTex) {
        glBindTexture(GL_TEXTURE_2D, s_terrainDepthTex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                            (GLsizei)s_fboWidth, (GLsizei)s_fboHeight);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return TRUE;
}

/* Repaints the stored terrain colour over the whole offscreen target with the
   pass-through present program. Depth stays the FBO's own — nothing writes it
   between the snapshot and this call — so the coming shadow spans still
   depth-test against the terrain. */
static void GL_TargetRestoreTerrain(void) {
    if (!s_initialized || !s_fbo || !s_terrainColorTex) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, (GLsizei)s_fboWidth, (GLsizei)s_fboHeight);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    /* Same non-flipped FBO quad the composite uses, so the stored rows land
       exactly where the terrain drew them. */
    glUseProgram(s_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_terrainColorTex);
    glUniform1i(s_uTexLoc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_paletteTex);
    glUniform1i(s_uPaletteLoc, 1);
    glUniform1i(s_uIndexedLoc, 0); /* terrain colour is already ARGB */

    glBindBuffer(GL_ARRAY_BUFFER, s_fboVbo);
    glEnableVertexAttribArray(s_aPosLoc);
    glVertexAttribPointer(s_aPosLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), (const void *)0);
    glEnableVertexAttribArray(s_aTexCoordLoc);
    glVertexAttribPointer(s_aTexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat),
                          (const void *)(2 * sizeof(GLfloat)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(s_aPosLoc);
    glDisableVertexAttribArray(s_aTexCoordLoc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);
}

/* Present pacing for the GL swapchain (the backend shares SDL's window, so
   the interval belongs to the context GL_MakeCurrent selected). */
static void GL_SetVSync(S32 enabled) {
    if (Window_GLMakeCurrent()) {
        SDL_GL_SetSwapInterval(enabled ? 1 : 0);
    }
}

/* ── Fixed-role texture uploads ────────────────────────────────────────────
   Dumb "copy from this pointer" slots: the common layer decides when the
   data changed and calls them.  Atlas refeeds on PtrMap change (tracked by
   COMMON), CLUT/palette reupload whenever a batch starts — the game edits
   the fog table and palette in place. */

static void GL_TexUpdateAtlas(const void *lum) {
    if (lum == NULL) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, lum);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void GL_TexUpdateCLUT(const void *lum) {
    if (lum == NULL) {
        return;
    }
    /* Upload the full 64 KB fog table (256×256) once. SetCLUT selects a
       16-row block within it; the gouraud value's high byte indexes into
       the correct rows directly via the shader. */
    glBindTexture(GL_TEXTURE_2D, s_clutTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, lum);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void GL_TexUpdatePalette(const void *argb) {
    const U32 *lut;
    U32 paletteCopy[256];
    U32 sceneCopy[256];
    S32 i;

    if (argb == NULL) {
        return;
    }
    /* Present copy: alpha 0xFF everywhere except palette index 0 (the
       transparent colour in the Log buffer — sprites/UI use colour 0 as
       chroma key, and the overlay's blend lets the scene show through
       those pixels). Scene copy: the alpha byte carries the palette index,
       which is how the FBO records the destination index for mode 8. */
    lut = (const U32 *)argb;
    memcpy(paletteCopy, lut, sizeof(paletteCopy));
    paletteCopy[0] &= 0x00FFFFFF; /* alpha → 0 */
    for (i = 0; i < 256; i++) {
        sceneCopy[i] = (lut[i] & 0x00FFFFFFu) | ((U32)i << 24);
    }

    /* A driver without the BGRA extension reads RGBA instead: swap the two
       colour bytes in the copies above (never in the engine's LUT). */
    if (!s_capsBgra) {
        SwizzleBGRAtoRGBA(paletteCopy, 256);
        SwizzleBGRAtoRGBA(sceneCopy, 256);
    }
    glBindTexture(GL_TEXTURE_2D, s_paletteTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
                    s_capsBgra ? (GLenum)FMT_BGRA : (GLenum)GL_RGBA,
                    GL_UNSIGNED_BYTE, paletteCopy);
    glBindTexture(GL_TEXTURE_2D, s_clutPaletteTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1,
                    s_capsBgra ? (GLenum)FMT_BGRA : (GLenum)GL_RGBA,
                    GL_UNSIGNED_BYTE, sceneCopy);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Brick atlas: LUMINANCE_ALPHA, byte0 = palette index, byte1 = valid.
   Created/resized on size change; NEAREST + CLAMP so mode 7's absolute
   texel UVs never bleed across shelf-pack gutters. */
static void GL_TexUpdateBricks(const void *rg, U32 width, U32 height) {
    if (rg == NULL || width == 0 || height == 0) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        Log_Warn("[GL] TexUpdateBricks: no context");
        return;
    }
    if (s_brickTex == 0) {
        glGenTextures(1, &s_brickTex);
    }
    glBindTexture(GL_TEXTURE_2D, s_brickTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s_brickTexW != width || s_brickTexH != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei)width,
                     (GLsizei)height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                     rg);
        s_brickTexW = width;
        s_brickTexH = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width,
                        (GLsizei)height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                        rg);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Sprite atlas: same LUMINANCE_ALPHA layout as the brick page, separate
   texture so a frame can hold both a room and its sprites. */
static void GL_TexUpdateSprites(const void *rg, U32 width, U32 height) {
    if (rg == NULL || width == 0 || height == 0) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        Log_Warn("[GL] TexUpdateSprites: no context");
        return;
    }
    if (s_spriteTex == 0) {
        glGenTextures(1, &s_spriteTex);
    }
    glBindTexture(GL_TEXTURE_2D, s_spriteTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s_spriteTexW != width || s_spriteTexH != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei)width,
                     (GLsizei)height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                     rg);
        s_spriteTexW = width;
        s_spriteTexH = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width,
                        (GLsizei)height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                        rg);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Blend table (mode 8): the 64 KB transpTable as a 256×256 LUMINANCE page,
   row = source index, column = destination index. */
static void GL_TexUpdateTransPal(const void *lum) {
    if (lum == NULL) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        Log_Warn("[GL] TexUpdateTransPal: no context");
        return;
    }
    if (s_transPalTex == 0) {
        glGenTextures(1, &s_transPalTex);
        glBindTexture(GL_TEXTURE_2D, s_transPalTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, 256, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    } else {
        glBindTexture(GL_TEXTURE_2D, s_transPalTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_LUMINANCE,
                        GL_UNSIGNED_BYTE, lum);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Blend source/Log page (mode 8): LUMINANCE_ALPHA rect at (0,0) — byte0 =
   walked source index (.r), byte1 = Log index (.a). The page is the rect
   size, which is also what GpuDrawParams.blendLogW/H carries, so the
   shader's (px + 0.5) / blendLog lands on texel px for every sprite. */
static void GL_TexUpdateBlendLog(const void *rg, U32 width, U32 height) {
    if (rg == NULL || width == 0 || height == 0) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        Log_Warn("[GL] TexUpdateBlendLog: no context");
        return;
    }
    if (s_blendLogTex == 0) {
        glGenTextures(1, &s_blendLogTex);
    }
    glBindTexture(GL_TEXTURE_2D, s_blendLogTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s_blendLogW != width || s_blendLogH != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei)width,
                     (GLsizei)height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                     NULL);
        s_blendLogW = width;
        s_blendLogH = height;
    }
    /* Rows are width*2 bytes; an odd width is not 4-byte aligned, so the
       default unpack stride would shift every row after the first. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                    GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, rg);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Blend destination rect (mode 8): read the offscreen colour rect (engine
   coordinates, top-down) and store it at (0,0) of the dst page with
   engine-top-down rows. GL framebuffer rows run bottom-up, so the rect is
   read at the flipped origin and the rows are reversed on the CPU. The
   SSAA scale between the present surface and the FBO is applied to both
   the source rect and the flip origin; the shader normalises by engine
   pixels and lands on each block's centre texel. */
static S32 GL_TargetCopyColorRect(U32 x, U32 y, U32 width, U32 height) {
    U32 scale, needW, needH, rowBytes, row;
    U8 *tmp;

    if (!s_initialized || !s_fbo || width == 0 || height == 0) {
        return FALSE;
    }
    if (!Window_GLMakeCurrent()) {
        return FALSE;
    }
    scale = (s_texWidth != 0) ? (s_fboWidth / s_texWidth) : 1;
    if (scale == 0) {
        scale = 1;
    }
    if (x * scale + width * scale > s_fboWidth ||
        y * scale + height * scale > s_fboHeight) {
        return FALSE;
    }

    rowBytes = width * scale * 4;
    needW = width * scale;
    needH = height * scale;
    if (s_blendDstTex == 0) {
        glGenTextures(1, &s_blendDstTex);
    }
    glBindTexture(GL_TEXTURE_2D, s_blendDstTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s_blendDstW != needW || s_blendDstH != needH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)needW,
                     (GLsizei)needH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s_blendDstW = needW;
        s_blendDstH = needH;
    }

    {
        const U32 need = rowBytes * (height * scale);
        if (s_blendReadbackBytes < need) {
            free(s_blendReadback);
            s_blendReadback = (U8 *)malloc(need);
            s_blendReadbackBytes = s_blendReadback ? need : 0;
        }
        if (s_blendReadback == NULL) {
            glBindTexture(GL_TEXTURE_2D, 0);
            return FALSE;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glReadPixels((GLint)(x * scale),
                 (GLint)(s_fboHeight - ((y + height) * scale)),
                 (GLsizei)(width * scale), (GLsizei)(height * scale),
                 GL_RGBA, GL_UNSIGNED_BYTE, s_blendReadback);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Rows came up from the flipped origin: row 0 is the rect's bottom
       engine row. Reverse them so the page reads top-down. */
    tmp = (U8 *)malloc(rowBytes);
    if (tmp == NULL) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return FALSE;
    }
    for (row = 0; row < (height * scale) / 2; row++) {
        U8 *a = s_blendReadback + row * rowBytes;
        U8 *b = s_blendReadback + ((height * scale) - 1 - row) * rowBytes;
        memcpy(tmp, a, rowBytes);
        memcpy(a, b, rowBytes);
        memcpy(b, tmp, rowBytes);
    }
    free(tmp);

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)(width * scale),
                    (GLsizei)(height * scale), GL_RGBA, GL_UNSIGNED_BYTE,
                    s_blendReadback);
    glBindTexture(GL_TEXTURE_2D, 0);
    return TRUE;
}

static void GL_TexUpdatePresent(const void *pixels, U32 pitch, U32 width,
                                U32 height, S32 rectX, S32 rectY, S32 rectW,
                                S32 rectH, S32 linear, S32 indexed) {
    if (pixels == NULL) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    /* Allocate the overlay texture at the present size (it changes on
       video-mode switches); a new size or a format switch recreates. The R8
       path stores raw palette indices (1 byte/pixel); the ARGB path keeps the
       console/touch staging surface. Always NEAREST on the index page so a
       texel fetch is one LUT input — bilinear is four taps in the shader. */
    if (s_overlayTex == 0 || s_texWidth != width || s_texHeight != height ||
        s_overlayIndexed != indexed) {
        if (s_overlayTex == 0) {
            glGenTextures(1, &s_overlayTex);
        } else {
            glBindTexture(GL_TEXTURE_2D, s_overlayTex);
            glDeleteTextures(1, &s_overlayTex);
            glGenTextures(1, &s_overlayTex);
        }
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (indexed) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (GLsizei)width,
                         (GLsizei)height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                         NULL);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                            linear ? GL_LINEAR : GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width,
                         (GLsizei)height, 0, BgraSrcFormat(),
                         GL_UNSIGNED_BYTE, NULL);
        }
        s_texWidth = width;
        s_texHeight = height;
        s_overlayIndexed = indexed;
        /* glTexImage2D's NULL data left the contents undefined: this call
           uploads the full surface whatever rect the caller planned. */
        s_overlayFullUpload = 1;
    } else {
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
    }

    /* ARGB: hardware bilinear on the converted pixels. R8: NEAREST always —
       the shader's four-tap path replaces it when DisplayTexFilter is on. */
    if (!indexed) {
        GLenum filter = linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }

    if (s_overlayFullUpload) {
        rectX = 0;
        rectY = 0;
        rectW = (S32)width;
        rectH = (S32)height;
        s_overlayFullUpload = 0;
    }

    if (rectW > 0 && rectH > 0) {
        /* Rows of the source are pitch apart and the rect's x origin is
           arbitrary, so no row start is guaranteed 4-byte aligned: upload
           byte-aligned and restore the GL default afterwards (same pairing
           the atlas uploads use). */
        const U32 bpp = indexed ? 1u : 4u;
        const U8 *src =
            (const U8 *)pixels + (size_t)rectY * pitch + (size_t)rectX * bpp;
        const U32 rowBytes = (U32)rectW * bpp;
        const U32 rows = (U32)rectH;
        const GLenum format =
            indexed ? (GLenum)GL_LUMINANCE
                    : (s_capsBgra ? (GLenum)FMT_BGRA : (GLenum)GL_RGBA);
        GLint rowLength = (GLint)(pitch / bpp);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (!s_capsRowLength || (!indexed && !s_capsBgra)) {
            /* No row stride to express, or a BGRA source the driver will not
               take: repack the rect tight (and swap R/B) into one buffer and
               upload that — one glTexSubImage2D either way. */
            const U32 need = rowBytes * rows;
            U32 row;
            if (s_uploadScratchBytes < need) {
                free(s_uploadScratch);
                s_uploadScratch = (U8 *)malloc(need);
                s_uploadScratchBytes = s_uploadScratch ? need : 0;
            }
            if (s_uploadScratch == NULL) {
                rowLength = -1; /* nothing to upload; state still restored */
            } else {
                for (row = 0; row < rows; row++) {
                    const U8 *srow = src + (size_t)row * pitch;
                    U8 *drow = s_uploadScratch + (size_t)row * rowBytes;
                    if (!indexed && !s_capsBgra) {
                        /* The format chosen for this upload is GL_RGBA, so
                           the copy is where the B and R bytes trade places;
                           a BGRA-capable driver only loses the stride and
                           keeps the source layout. */
                        U32 x;
                        for (x = 0; x + 3 < rowBytes; x += 4) {
                            drow[x + 0] = srow[x + 2];
                            drow[x + 1] = srow[x + 1];
                            drow[x + 2] = srow[x + 0];
                            drow[x + 3] = srow[x + 3];
                        }
                    } else {
                        memcpy(drow, srow, rowBytes);
                    }
                }
                src = s_uploadScratch;
                rowLength = 0;
            }
        }
        if (rowLength > 0) {
            glPixelStorei(UNPACK_ROW_LENGTH, rowLength);
        }
        if (rowLength >= 0) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, rectX, rectY, rectW, rectH,
                            format, GL_UNSIGNED_BYTE, src);
        }
        if (rowLength > 0) {
            glPixelStorei(UNPACK_ROW_LENGTH, 0);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ── 3D triangles ─────────────────────────────────────────────────────────
   Draws one pre-resolved batch.  The common layer already picked the polygon
   mode, colour, depth rules and screen scale; this function only translates
   GpuDrawParams into uniforms and draws the packed vertices. */

static void GL_DrawTriangles(const GpuVertex *verts, U32 vertexCount,
                             const GpuDrawParams *params) {
    if (vertexCount == 0 || s_clutProgram == 0) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    /* Draw into the offscreen target (the common layer cleared it); without
       a valid FBO (broken setup) fall back to the default framebuffer. */
    if (s_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
        glViewport(0, 0, (GLsizei)s_fboWidth, (GLsizei)s_fboHeight);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, (GLsizei)s_texWidth, (GLsizei)s_texHeight);
    }

    /* Upload batch vertices to VBO (14 bytes per packed vertex) */
    glBindBuffer(GL_ARRAY_BUFFER, s_batchVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * 14, verts);

    glUseProgram(s_clutProgram);

    /* Map GpuDrawParams onto the CLUT shader uniforms. */
    glUniform2f(s_clut_uScreenScale, params->screenScale[0],
                params->screenScale[1]);
    glUniform1i(s_clut_uPolyMode, params->polyMode);
    glUniform1f(s_clut_uFlatColor, params->flatColor);
    glUniform1i(s_clut_uAlphaMode, params->alphaMode);
    glUniform1i(s_clut_uBilinear, params->bilinear);
    glUniform1i(s_clut_uPerspCorrect, params->perspCorrect);
    glUniform1i(s_clut_uChromaKey, params->chromaKey);
    glUniform1f(s_clut_uCLUTBaseRow, params->clutBaseRow);
    glUniform1f(s_clut_uFlatRow, params->flatRow);
    glUniform1f(s_clut_uScaledFogNear, params->scaledFogNear);
    glUniform1f(s_clut_uFogRowScale, params->fogRowScale);
    if (params->polyMode == 7) {
        if (params->spriteAtlas) {
            glUniform2f(s_clut_uBrickSize, (float)s_spriteTexW,
                        (float)s_spriteTexH);
        } else {
            glUniform2f(s_clut_uBrickSize, (float)s_brickTexW,
                        (float)s_brickTexH);
        }
    }
    if (params->polyMode == 8) {
        glUniform1f(s_clut_uBlendLogW, params->blendLogW);
        glUniform1f(s_clut_uBlendLogH, params->blendLogH);
        glUniform1f(s_clut_uBlendDstW, params->blendDstW);
        glUniform1f(s_clut_uBlendDstH, params->blendDstH);
    }

    /* TRANS/TRAME (blend) batches alpha-blend; everything else is opaque.
       INCRUST chroma-keying is a shader discard, not blending, so Z-write
       stays enabled there (the common layer already resolved that).
       The alpha channel of the target carries the destination palette
       index (the scene palette's alpha byte), so the alpha equation must
       keep it: dst alpha = dst alpha, regardless of the fragment's own. */
    if (params->blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                            GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }

    /* Bind textures to units 0/1/2. Mode 7 samples the brick or sprite
       RG8 page through uAtlas; every other mode samples the 256×256 page.
       Mode 8 additionally binds its three lookup pages on units 3/4/5
       (LUMINANCE_ALPHA src/Log, BGRA dst rect, LUMINANCE table). */
    glActiveTexture(GL_TEXTURE0);
    if (params->polyMode == 7) {
        glBindTexture(GL_TEXTURE_2D,
                      params->spriteAtlas ? s_spriteTex : s_brickTex);
    } else {
        glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    }
    glUniform1i(s_clut_uAtlas, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_clutTex);
    glUniform1i(s_clut_uCLUT, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_clutPaletteTex);
    glUniform1i(s_clut_uPalette, 2);

    if (params->polyMode == 8) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, s_blendLogTex);
        glUniform1i(s_clut_uBlendSrcLog, 3);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, s_blendDstTex);
        glUniform1i(s_clut_uBlendDst, 4);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, s_transPalTex);
        glUniform1i(s_clut_uTransPal, 5);
    }

    glActiveTexture(GL_TEXTURE0); /* restore default */

    /* Set up vertex attributes (14 bytes per vertex) */
    /* aPos: 2 × GL_SHORT, stride=14, offset=0 */
    glEnableVertexAttribArray(s_clut_aPos);
    glVertexAttribPointer(s_clut_aPos, 2, GL_SHORT, GL_FALSE, 14,
                          (const void *)0);

    /* aTexCoord: 2 × GL_UNSIGNED_SHORT, stride=14, offset=4 */
    glEnableVertexAttribArray(s_clut_aTexCoord);
    glVertexAttribPointer(s_clut_aTexCoord, 2, GL_UNSIGNED_SHORT, GL_FALSE, 14,
                          (const void *)4);

    /* aLight: 1 × GL_UNSIGNED_SHORT, stride=14, offset=8 */
    glEnableVertexAttribArray(s_clut_aLight);
    glVertexAttribPointer(s_clut_aLight, 1, GL_UNSIGNED_SHORT, GL_FALSE, 14,
                          (const void *)8);

    /* aZ: 1 × GL_UNSIGNED_SHORT, stride=14, offset=10 */
    glEnableVertexAttribArray(s_clut_aZ);
    glVertexAttribPointer(s_clut_aZ, 1, GL_UNSIGNED_SHORT, GL_FALSE, 14,
                          (const void *)10);

    /* aRepMask: 1 × GL_UNSIGNED_SHORT, stride=14, offset=12 */
    glEnableVertexAttribArray(s_clut_aRepMask);
    glVertexAttribPointer(s_clut_aRepMask, 1, GL_UNSIGNED_SHORT, GL_FALSE, 14,
                          (const void *)12);

    /* Depth rules resolved by the common layer (see Common_BuildDrawParams):
       GEQUAL for the terrain/bodies, ALWAYS for NZW coplanar animated polys,
       and no Z-write for test-only or blended batches. */
    switch (params->depthTest) {
    case 1:
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        break;
    case 2:
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        break;
    default:
        glDisable(GL_DEPTH_TEST);
        break;
    }
    glDepthMask(params->depthWrite ? GL_TRUE : GL_FALSE);

    /* Scissor to the software Fill_Poly clip window (engine pixels mapped
       through screenScale into target pixels). GL's origin is bottom-left,
       so the rect's top-down engine Y is flipped against the target height.
       The mapped edges are not integers (screenScale is a float ratio), and
       a truncated edge loses the top or right pixel row of the clip window —
       visible as the topmost row of every clip-scissored batch never
       drawing.
       Floor the low edge and ceil the high edge so the scissor covers the
       whole window; the rasterizer still bounds the draw to the quad. */
    {
        const float engW = 2.0f / params->screenScale[0];
        const float engH = 2.0f / params->screenScale[1];
        const float sx = (float)s_fboWidth / engW;
        const float sy = (float)s_fboHeight / engH;
        const float devX0 = floorf((float)params->clipX0 * sx);
        const float devX1 = ceilf((float)(params->clipX1 + 1) * sx);
        const float devY0 = floorf((float)s_fboHeight -
                                   (float)(params->clipY1 + 1) * sy);
        const float devY1 = ceilf((float)s_fboHeight -
                                  (float)params->clipY0 * sy);
        glEnable(GL_SCISSOR_TEST);
        if (devX1 <= devX0 || devY1 <= devY0) {
            glScissor(0, 0, 0, 0);
        } else {
            glScissor((GLint)devX0, (GLint)devY0, (GLsizei)(devX1 - devX0),
                      (GLsizei)(devY1 - devY0));
        }
    }

    /* Draw all batched triangles */
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertexCount);

    /* Restore depth state so subsequent batches set their own */
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);

    /* Clean up */
    glDisableVertexAttribArray(s_clut_aPos);
    glDisableVertexAttribArray(s_clut_aTexCoord);
    glDisableVertexAttribArray(s_clut_aLight);
    glDisableVertexAttribArray(s_clut_aZ);
    glDisableVertexAttribArray(s_clut_aRepMask);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

/* ── Window composite ──────────────────────────────────────────────────────
   The common layer sequences one host frame as:
       FlushBatch → TexUpdatePresent → ClearBackbuffer
                   → DrawPresentQuad(0) [offscreen colour]
                   → DrawPresentQuad(1) [2D overlay + swap]
   ClearBackbuffer sets the letterbox viewport that both quad draws use. */

static void GL_ClearBackbuffer(void) {
    if (!s_initialized) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    /* Switch back to the default framebuffer and drop depth state.
       Scissor only governs the offscreen 3D pass; the present quads draw
       the whole letterbox content. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    /* Compute letterbox viewport to maintain the game's aspect ratio,
       matching the SW renderer's SDL_LOGICAL_PRESENTATION_LETTERBOX. */
    SDL_Window *win = Window_GetSDLWindow();
    int winW = (int)s_texWidth, winH = (int)s_texHeight;
    if (win) {
        SDL_GetWindowSize(win, &winW, &winH);
    }
    const U32 width = s_texWidth;
    const U32 height = s_texHeight;

    /* Clear the default framebuffer to black (for letterbox bars). */
    glViewport(0, 0, winW, winH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Compute the largest centred rectangle that preserves aspect ratio. */
    int vpX, vpY, vpW, vpH;
    {
        double windowAspect = (double)winW / (double)winH;
        double contentAspect = (double)width / (double)height;
        if (windowAspect > contentAspect) {
            /* Window wider than content → pillarbox */
            vpH = winH;
            vpW = (int)((double)winH * contentAspect + 0.5);
            vpW = (vpW + 1) & ~1;
            if (vpW > winW)
                vpW = winW;
            vpX = (winW - vpW) / 2;
            vpY = 0;
        } else {
            /* Window taller than content → letterbox */
            vpW = winW;
            vpH = (int)((double)winW / contentAspect + 0.5);
            vpH = (vpH + 1) & ~1;
            if (vpH > winH)
                vpH = winH;
            vpX = 0;
            vpY = (winH - vpH) / 2;
        }
    }
    glViewport(vpX, vpY, vpW, vpH);
}

static void GL_DrawPresentQuad(S32 which, S32 linear, S32 opaqueX0,
                               S32 opaqueY0, S32 opaqueX1, S32 opaqueY1) {
    if (!s_initialized) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    /* The FBO source is always LINEAR, matching the reference driver. The
       overlay's effective filter was applied at TexUpdatePresent (ARGB) or
       is selected by uLinear (R8 four-tap path). */
    (void)linear;

    if (which == 0) {
        /* Offscreen target colour → window. The letterbox viewport set by
            ClearBackbuffer stays active. */
        if (!s_fbo) {
            return;
        }
        glDisable(GL_BLEND);
        glUseProgram(s_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_fboColorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(s_uTexLoc, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_paletteTex);
        glUniform1i(s_uPaletteLoc, 1);
        glUniform1i(s_uIndexedLoc, 0);
        glUniform1i(s_uLinearLoc, 0);
        glUniform4f(s_uOpaqueRectLoc, 0.0f, 0.0f, 0.0f, 0.0f);
        glUniform2f(s_uSizeLoc, (float)s_texWidth, (float)s_texHeight);

        glBindBuffer(GL_ARRAY_BUFFER, s_fboVbo);
        glEnableVertexAttribArray(s_aPosLoc);
        glVertexAttribPointer(s_aPosLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (const void *)0);
        glEnableVertexAttribArray(s_aTexCoordLoc);
        glVertexAttribPointer(s_aTexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat),
                              (const void *)(2 * sizeof(GLfloat)));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(s_aPosLoc);
        glDisableVertexAttribArray(s_aTexCoordLoc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);
    } else {
        /* 2D overlay (Log as ARGB staging, or raw indices on R8 resolved
           through uPalette). Alpha blending lets palette index 0 (alpha=0)
           be transparent so the 3D scene shows through underneath; inside
           uOpaqueRect it presents opaque black like the software present.
           Swap inside this call. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(s_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
        glUniform1i(s_uTexLoc, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_paletteTex);
        glUniform1i(s_uPaletteLoc, 1);
        glUniform1i(s_uIndexedLoc, s_overlayIndexed);
        glUniform1i(s_uLinearLoc, linear);
        glUniform4f(s_uOpaqueRectLoc, (float)opaqueX0, (float)opaqueY0,
                    (float)opaqueX1, (float)opaqueY1);
        glUniform2f(s_uSizeLoc, (float)s_texWidth, (float)s_texHeight);

        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(s_aPosLoc);
        glVertexAttribPointer(s_aPosLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), (const void *)0);
        glEnableVertexAttribArray(s_aTexCoordLoc);
        glVertexAttribPointer(s_aTexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat),
                              (const void *)(2 * sizeof(GLfloat)));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(s_aPosLoc);
        glDisableVertexAttribArray(s_aTexCoordLoc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);

        Window_GL_SwapWindow();
    }
}

/* ── Backend vtable ─────────────────────────────────────────────────────── */

const GpuRendererBackend GpuRendererBackendGL = {
    "opengl",
    GL_CreateContext,
    GL_DestroyContext,
    GL_TargetCreate,
    GL_TargetDestroy,
    GL_TargetIsValid,
    GL_TargetBindClear,
    GL_TargetWidth,
    GL_TargetHeight,
    GL_TargetReadDepth,
    GL_TargetReadColor,
    GL_TargetSnapshotTerrain,
    GL_TargetRestoreTerrain,
    GL_SetVSync,
    GL_TexUpdatePresent,
    GL_TexUpdateAtlas,
    GL_TexUpdateCLUT,
    GL_TexUpdatePalette,
    GL_TexUpdateBricks,
    GL_TexUpdateSprites,
    GL_TexUpdateTransPal,
    GL_TexUpdateBlendLog,
    GL_TargetCopyColorRect,
    GL_DrawTriangles,
    GL_ClearBackbuffer,
    GL_DrawPresentQuad,
};

#endif /* USE_GPURENDERER */