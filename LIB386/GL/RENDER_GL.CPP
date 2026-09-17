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

/* ── Renderer state shared with the engine (C linkage) ─────────────────────
   Owned by the GL renderer even in software builds (gly is always built, as
   stubs when GPURENDERER is off) so every consumer links against one
   definition. DisplayTexFilter selects bilinear CLUT/overlay sampling in GL
   mode; RenderQuality is the FBO supersample factor; AtlasTextureDirty is
   raised by DoTextureAnimation when the 256x256 atlas page is mutated in
   place. */
extern "C" {
S32 DisplayTexFilter = TRUE; /* bilinear by default in GL mode */
S32 RenderQuality = 1;       /* native resolution by default */
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
#include <string.h>

/* paletteLUT builds 0xAARRGGBB which in little-endian memory is
   [B, G, R, A] — use GL_BGRA to match the byte layout. */
#ifdef GL_BGRA
#define FMT_BGRA GL_BGRA
#else
#define FMT_BGRA 0x80E1 /* GL_BGRA_EXT */
#endif

/* ── Shader sources (GL 2.1 / GLES 2.0 compatible) ─────────────────────── */

#ifdef USE_GLES2
#define GLSL_VERT_HEADER "#version 100\n"
#define GLSL_FRAG_HEADER "#version 100\nprecision mediump float;\n"
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

static const char *s_fragSrc =
    GLSL_FRAG_HEADER
    "uniform sampler2D uTex;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(uTex, vTexCoord);\n"
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
    "uniform sampler2D uAtlas;\n"   /* 256×256 LUMINANCE */
    "uniform sampler2D uCLUT;\n"    /* 256×256 LUMINANCE (fog table) */
    "uniform sampler2D uPalette;\n" /* 256×1 BGRA (paletteLUT) */
    "uniform int uPolyMode;\n"      /* 0=texture 1=flat 2=gouraud 3=gouraudTable 4=textureNoCLUT 5=fogSmooth */
    "uniform float uFlatColor;\n"   /* palette index (mode 1) / CLUT column (mode 3) */
    "uniform float uCLUTBaseRow;\n" /* CLUT row offset for gouraud table (mode 3) */
    "uniform float uScaledFogNear;\n" /* Fill_ScaledFogNear (fog-smooth distance, mode 5) */
    "uniform float uFogRowScale;\n"   /* Fill_Fog_Factor / 65536 (fog-smooth distance, mode 5) */
    "uniform int uAlphaMode;\n"     /* 0=opaque 1=TRANS(semi) 2=TRAME(stipple) */
    "uniform int uBilinear;\n"      /* 0=nearest 1=bilinear CLUT sampling */
    "uniform int uChromaKey;\n"     /* 0=no chroma key 1=discard index 0 */
    "uniform int uPerspCorrect;\n"  /* 1=perspective-correct UV/Z (types 16-24) */
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
static GLuint s_fboVbo = 0; /* non-flipped UV quad for FBO→screen draw */
static GLuint s_overlayTex = 0; /* present texture (Log buffer ARGB) */
static GLint s_uTexLoc = -1;
static GLint s_aPosLoc = -1;
static GLint s_aTexCoordLoc = -1;
static U32 s_texWidth = 0;  /* present surface size (engine pixel space) */
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
static GLint s_clut_uScaledFogNear = -1;
static GLint s_clut_uFogRowScale = -1;
static GLint s_clut_uAlphaMode = -1;
static GLint s_clut_uBilinear = -1;
static GLint s_clut_uChromaKey = -1;
static GLint s_clut_uPerspCorrect = -1;

/* ── GL state — textures ────────────────────────────────────────────────── */

static GLuint s_atlasTex = 0;   /* 256×256 LUMINANCE */
static GLuint s_clutTex = 0;    /* 256×256 LUMINANCE (full fog table) */
static GLuint s_paletteTex = 0; /* 256×1 BGRA */

/* ── GL state — batch VBO ───────────────────────────────────────────────── */

static const U32 BATCH_MAX_VERTS = 4096; /* matches the common-layer budget */
static GLuint s_batchVBO = 0;

/* ── GL state — FBO (offscreen target for 3D, SSAA-scaled) ──────────────── */
static GLuint s_fbo = 0;
static GLuint s_fboColorTex = 0;
static GLuint s_fboDepthRbo = 0;
static U32 s_fboWidth = 0;
static U32 s_fboHeight = 0;

/* ── Fullscreen quad: position (x,y) + texcoord (u,v) ───────────────────── */

/* 2D overlay quad — V is flipped because glTexSubImage2D uploads row 0
   (game Y=0, top of screen) at texture V=0 (bottom in OpenGL). Flipping V
   here maps game Y=0 → screen top. */
static const GLfloat s_quadVerts[] = {
    /* pos       texcoord */
    -1.0f, -1.0f,  0.0f, 1.0f,  /* bottom-left  (flip Y: texcoord 1.0) */
     1.0f, -1.0f,  1.0f, 1.0f,  /* bottom-right */
    -1.0f,  1.0f,  0.0f, 0.0f,  /* top-left      (flip Y: texcoord 0.0) */
     1.0f,  1.0f,  1.0f, 0.0f,  /* top-right */
};

/* FBO quad — V is NOT flipped. The CLUT vertex shader already maps game
   Y=0 → FBO top (clip Y=+1), so V=1.0 at screen top samples the FBO
   correctly without an extra flip. */
static const GLfloat s_fboQuadVerts[] = {
    /* pos       texcoord */
    -1.0f, -1.0f,  0.0f, 0.0f,  /* bottom-left  (V=0.0 = FBO bottom) */
     1.0f, -1.0f,  1.0f, 0.0f,  /* bottom-right */
    -1.0f,  1.0f,  0.0f, 1.0f,  /* top-left      (V=1.0 = FBO top) */
     1.0f,  1.0f,  1.0f, 1.0f,  /* top-right */
};

/* ── Shader helpers ─────────────────────────────────────────────────────── */

static GLuint CompileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
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
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* ── FBO helpers (SSAA) ─────────────────────────────────────────────────── */

static void GL_DestroyFBO(void) {
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
}

/* ── Context lifecycle ──────────────────────────────────────────────────── */

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
    s_clut_uScaledFogNear = glGetUniformLocation(s_clutProgram, "uScaledFogNear");
    s_clut_uFogRowScale = glGetUniformLocation(s_clutProgram, "uFogRowScale");
    s_clut_uAlphaMode = glGetUniformLocation(s_clutProgram, "uAlphaMode");
    s_clut_uBilinear = glGetUniformLocation(s_clutProgram, "uBilinear");
    s_clut_uChromaKey = glGetUniformLocation(s_clutProgram, "uChromaKey");

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

    /* Palette texture (256×1, BGRA — paletteLUT) */
    glGenTextures(1, &s_paletteTex);
    glBindTexture(GL_TEXTURE_2D, s_paletteTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, FMT_BGRA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

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

static void GL_TargetBindClear(void) {
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

/* Raw readbacks. Rows come back bottom-up; the common layer flips them and
   applies the aZ inverse / ARGB→palette-index conversion. */
static S32 GL_TargetReadDepth(void *dst, U32 width, U32 height) {
    if (!s_initialized || !s_fbo || dst == NULL) {
        return FALSE;
    }
    if (!Window_GLMakeCurrent()) {
        return FALSE;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_SHORT, dst);
    return TRUE;
}

static void GL_TargetReadColor(void *dst, U32 width, U32 height) {
    if (!s_initialized || !s_fbo || dst == NULL) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    /* Read as BGRA: paletteLUT is 0xAARRGGBB = [B,G,R,A] in LE memory,
       and GL_BGRA reads bytes in B,G,R,A order → U32 = ARGB. */
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, FMT_BGRA,
                 GL_UNSIGNED_BYTE, dst);
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
    if (argb == NULL) {
        return;
    }
    /* Copy palette and set alpha=0 for palette index 0 (transparent colour
       in the Log buffer — sprites/UI use colour 0 as chroma key). */
    U32 paletteCopy[256];
    memcpy(paletteCopy, argb, sizeof(paletteCopy));
    paletteCopy[0] &= 0x00FFFFFF; /* alpha → 0 */

    glBindTexture(GL_TEXTURE_2D, s_paletteTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, FMT_BGRA,
                    GL_UNSIGNED_BYTE, paletteCopy);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void GL_TexUpdatePresent(const void *argb, U32 pitch, U32 width,
                                U32 height, S32 linear) {
    if (argb == NULL) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    /* Allocate the overlay texture at the present size (it changes on
       video-mode switches); a new size or a dirty flag resizes. */
    if (s_overlayTex == 0 || s_texWidth != width || s_texHeight != height) {
        if (s_overlayTex == 0) {
            glGenTextures(1, &s_overlayTex);
        }
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                     0, FMT_BGRA, GL_UNSIGNED_BYTE, NULL);
        s_texWidth = width;
        s_texHeight = height;
    } else {
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
    }

    /* Set filter mode (bilinear vs nearest) */
    GLenum filter = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                    FMT_BGRA, GL_UNSIGNED_BYTE, argb);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
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
    glUniform1f(s_clut_uScaledFogNear, params->scaledFogNear);
    glUniform1f(s_clut_uFogRowScale, params->fogRowScale);

    /* TRANS/TRAME (blend) batches alpha-blend; everything else is opaque.
       INCRUST chroma-keying is a shader discard, not blending, so Z-write
       stays enabled there (the common layer already resolved that). */
    if (params->blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    /* Bind textures to units 0/1/2 */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glUniform1i(s_clut_uAtlas, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_clutTex);
    glUniform1i(s_clut_uCLUT, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_paletteTex);
    glUniform1i(s_clut_uPalette, 2);

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

    /* Switch back to the default framebuffer and drop depth state. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);

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
            if (vpW > winW) vpW = winW;
            vpX = (winW - vpW) / 2;
            vpY = 0;
        } else {
            /* Window taller than content → letterbox */
            vpW = winW;
            vpH = (int)((double)winW / contentAspect + 0.5);
            vpH = (vpH + 1) & ~1;
            if (vpH > winH) vpH = winH;
            vpX = 0;
            vpY = (winH - vpH) / 2;
        }
    }
    glViewport(vpX, vpY, vpW, vpH);
}

static void GL_DrawPresentQuad(S32 which, S32 linear) {
    if (!s_initialized) {
        return;
    }
    if (!Window_GLMakeCurrent()) {
        return;
    }

    /* The sampler filter for the overlay was applied at TexUpdatePresent; the
       FBO source is always LINEAR, matching the reference driver. */
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
        /* 2D overlay (the Log buffer as ARGB, uploaded by TexUpdatePresent).
           Alpha blending lets palette index 0 (alpha=0) be transparent so
           the 3D scene shows through underneath. Swap inside this call. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(s_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_overlayTex);
        glUniform1i(s_uTexLoc, 0);

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
    GL_TexUpdatePresent,
    GL_TexUpdateAtlas,
    GL_TexUpdateCLUT,
    GL_TexUpdatePalette,
    GL_DrawTriangles,
    GL_ClearBackbuffer,
    GL_DrawPresentQuad,
};

#endif /* USE_GPURENDERER */