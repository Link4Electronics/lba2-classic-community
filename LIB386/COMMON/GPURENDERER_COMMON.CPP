//──────────────────────────────────────────────────────────────────────────
// GPU renderer: driver-agnostic common layer
//──────────────────────────────────────────────────────────────────────────
// Implements the engine-facing API (GPU/GPURENDERER.H) on top of one backend
// vtable (GPU/GPURENDERER_BACKEND.H). Everything that does not need a specific
// GPU API lives here: polygon-type dispatch and GpuDrawParams resolution,
// vertex packing, batch flush triggers, the frame composite and the readback
// post-process (Y-flip, aZ inverse, SSAA downscale, ARGB→palette-index).
//
// The logic mirrors LIB386/GL/RENDER_GL.CPP, the reference driver. Semantics
// that are behaviour-visible in the game must not drift: poly-mode mapping,
// depth-test rules, and the SW fallbacks (body polys outside the engine's 3D
// pass, NZW polys before the first terrain pass, menu plasma type-8 with
// RepMask 0x3F3F).
//──────────────────────────────────────────────────────────────────────────
#ifdef USE_GPURENDERER

#include <GPU/GPURENDERER.H>
#include <GPU/GPURENDERER_BACKEND.H>
#include <POLYGON/POLY.H>
#include <SVGA/SDL.H> /* SetGpuPresentHooks: the present seam SDL.CPP exposes */
#include <SYSTEM/LOG.H>

#include <stdlib.h>
#include <string.h>

//──────────────────────────────────────────────────────────────────────────
// Engine globals the renderer reads (defined in game / SVGA code, same set
// the GL driver referenced). See the batch/enqueue section for why each is
// read at the moment it is.
//──────────────────────────────────────────────────────────────────────────
extern "C" {
extern PTR_U8 PtrCLUTFog;      /* 256×256 CLUT fog table (SetCLUT target)  */
extern PTR_U8 PtrCLUTGouraud;  /* current CLUT block within PtrCLUTFog      */
extern PTR_U8 PtrMap;          /* current 256×256 atlas page                */
extern S32 RepMask;            /* texture repeat mask (per polygon)         */
extern U8 Fill_Flag_ZBuffer;   /* Z-buffered fill active (terrain passes)   */
extern U8 Fill_Flag_NZW;       /* "no Z write" path (animated polys)        */
extern U8 Fill_Flag_3DPass;    /* engine 3D pass open (body draw in AffScene) */
extern U32 Fill_Fog_Factor;    /* fog-smooth scale (POLY_TEXTURE_Z_FOG)     */
extern U32 Fill_ScaledFogNear; /* fog-smooth near plane (type 24)           */
extern S32 AtlasTextureDirty;  /* in-place atlas mutation (same pointer)    */
extern S32 DisplayTexFilter;   /* bilinear toggle, read once per frame      */
extern U16 *PtrZBuffer;        /* readback targets (SW Z-buffer / Log)      */
extern void *Screen;
extern const U32 *GL_GetPaletteLUT(void);
}

//──────────────────────────────────────────────────────────────────────────
// Backend table. Built from the symbols the two backend libraries export.
//──────────────────────────────────────────────────────────────────────────
typedef struct BackendSlot {
    GpuRendererBackendId id;
    const GpuRendererBackend *backend;
} BackendSlot;

static const BackendSlot s_backends[] = {
    { GPU_RENDERER_BACKEND_OPENGL, &GpuRendererBackendGL },
    { GPU_RENDERER_BACKEND_SDL3GPU, &GpuRendererBackendSDL3GPU },
};
static const U32 s_backendCount =
    (U32)(sizeof(s_backends) / sizeof(s_backends[0]));

static const GpuRendererBackend *Common_TableLookup(GpuRendererBackendId id) {
    for (U32 i = 0; i < s_backendCount; i++) {
        if (s_backends[i].id == id) {
            return s_backends[i].backend;
        }
    }
    return NULL;
}

//──────────────────────────────────────────────────────────────────────────
// Renderer state
//──────────────────────────────────────────────────────────────────────────
static const GpuRendererBackend *s_backend = NULL;
static GpuRendererBackendId s_backendId = GPU_RENDERER_BACKEND_NONE;
static S32 s_active = 0;
static S32 s_vsyncWanted = 1; /* present pacing, default on */
static U32 s_resX = 0;  /* present surface size (engine pixel space) */
static U32 s_resY = 0;
static U32 s_renderQuality = 1; /* 1 = native, 2 = 2x, 4 = 4x */

/* Persistent FBO-has-terrain flag (see ClearFBO/BeginFrame). */
static S32 s_fboHasTerrain = 0;

/* The persistent terrain target holds a post-terrain-pass copy of the FBO
   (see SnapshotTerrain/RestoreTerrain). Valid only once a snapshot landed,
   and the restore is a no-op before that so the first AFF_OBJETS frame of a
   scene cannot composite stale geometry. */
static S32 s_terrainValid = 0;

/* Modal full-2D UI frames (inventory when opened mid-game): within the
   modal's region the Log overlay owns the frame, so palette index 0 there
   presents as the software path's opaque black instead of a window onto the
   offscreen 3D target. Outside the region the normal transparent rule holds,
   so the frozen 3D scene still shows around the modal. */
static S32 s_overlayOpaqueX0 = 0;
static S32 s_overlayOpaqueY0 = 0;
static S32 s_overlayOpaqueX1 = 0;
static S32 s_overlayOpaqueY1 = 0;

/* ── Batch buffer ──────────────────────────────────────────────────────── */

static const U32 BATCH_MAX_VERTS = 4096;
static U8 s_batchVerts[BATCH_MAX_VERTS * 14]; /* 14 bytes per vertex */
static U32 s_batchVertCount = 0;
static U32 s_batchTriCount = 0;

/* Batch start state (captured per enqueue; flush-trigger comparisons and
   uniform resolution read the copy, never live globals). */
static U32 s_currentTypePoly = 0;
static U32 s_currentColorPoly = 0;
static const void *s_batchAtlas = NULL;
static float s_batchCLUTBaseRow = 0.0f;
static float s_batchScaledFogNear = 0.0f;
static float s_batchFogRowScale = 0.0f;
static const void *s_batchCLUTGouraud = NULL;
static U8 s_batchZBuffer = 0;
static U8 s_batchNZW = 0;
static U8 s_batch3DPass = 0;

static S32 s_bilinearCLUT = 0; /* bilinear CLUT sampling flag for 3D */

/* ── Scene-shadow span buffer ───────────────────────────────────────────── */
/* Shadows are flat black quads, one 2-triangle strip per scanline span. The
   level is the engine's ShadowLevel (0..15): the SW ShadeBoxBlk darkens a
   pixel through the CLUT row (15 - level)*256 of the current gouraud block,
   so the equivalent alpha is that same row averaged against the palette,
   falling back to level/15 when the table is unavailable. Spans depth-test
   against the FBO the terrain left behind (ground aZ is at-or-farther than
   the shadow's own plane; nearer terrain — a risen ridge between camera and
   object — culls it). Kept apart from the polygon batch so a shadow can be
   drawn mid-frame without disturbing the batch's captured CLUT/type state. */
#define SHADOW_MAX_VERTS 4096
#define SHADOW_LEVEL_DIVISOR 15.0f
static GpuVertex s_shadowVerts[SHADOW_MAX_VERTS];
static U32 s_shadowVertCount = 0;
static float s_shadowAlpha = 0.0f;
static S32 s_shadowPlaneAZ = 32768; /* clip-Z of the shadow's ground plane */
static S32 s_shadowActive = 0;

/* Uploaded-pointer tracking. */
static const void *s_uploadedAtlas = NULL;

/* ── Screen-space mapping ───────────────────────────────────────────────── */

static float s_screenScale[2] = { 0.0f, 0.0f };

static void Common_UpdateScreenScale(void) {
    s_screenScale[0] = 2.0f / (float)s_resX;
    s_screenScale[1] = 2.0f / (float)s_resY;
}

//──────────────────────────────────────────────────────────────────────────
// Front texture uploads. Upload tracking lives here (not in the backend) so
// the backend's TexUpdate* slots stay dumb "copy from this pointer" calls,
// and the decision of *when* the data changed is engine-knowledge, not
// driver-knowledge.
//──────────────────────────────────────────────────────────────────────────
static void Common_UploadAtlas(void) {
    if (PtrMap == NULL) {
        return;
    }
    if (AtlasTextureDirty) {
        s_uploadedAtlas = NULL;
        AtlasTextureDirty = 0;
    }
    if (s_uploadedAtlas != (const void *)PtrMap) {
        s_backend->TexUpdateAtlas(PtrMap);
        s_uploadedAtlas = PtrMap;
    }
}

static void Common_UploadCLUT(void) {
    /* Re-upload every enqueue: the game edits the fog table in place between
       scenes (same PtrCLUTFog pointer, different data). Cheap relative to the
       per-poly draw that follows. */
    if (PtrCLUTFog != NULL) {
        s_backend->TexUpdateCLUT(PtrCLUTFog);
    }
}

static void Common_UploadPalette(void) {
    const U32 *lut = GL_GetPaletteLUT();
    if (lut != NULL) {
        s_backend->TexUpdatePalette(lut);
    }
}

//──────────────────────────────────────────────────────────────────────────
// GpuDrawParams resolution — the flush-time state translation. This is the
// poly-mode / alpha / depth contract of the reference driver; see
// GPURENDERER_BACKEND.H for the field semantics.
//──────────────────────────────────────────────────────────────────────────
static void Common_BuildDrawParams(GpuDrawParams *out) {
    memset(out, 0, sizeof *out);

    /* Poly mode + flat colour + alpha mode. */
    if (s_currentTypePoly < POLY_TEXTURE) {
        if (s_currentTypePoly <= 1) {
            out->polyMode = 1; /* SOLID / FLAT */
            out->flatColor = (float)(s_currentColorPoly & 0xFF);
        } else if (s_currentTypePoly == 2) {
            out->polyMode = 1; /* TRANS → flat colour + blending */
            out->alphaMode = 1;
            out->flatColor = (float)(s_currentColorPoly & 0xFF);
        } else if (s_currentTypePoly == 3) {
            out->polyMode = 1; /* TRAME → flat colour + stipple */
            out->alphaMode = 2;
            out->flatColor = (float)(s_currentColorPoly & 0xFF);
        } else if (s_currentTypePoly <= 5) {
            out->polyMode = 2; /* GOURAUD / DITHER */
            out->flatColor = (float)(s_currentColorPoly & 0xFF);
        } else {
            out->polyMode = 3; /* GOURAUD_TABLE / DITHER_TABLE */
            out->flatColor = (float)(s_currentColorPoly & 0xFF);
        }
    } else if (s_currentTypePoly == POLY_TEXTURE ||
               s_currentTypePoly == POLY_TEXTURE_INCRUST) {
        /* Type 8 / 12: the SW filler writes the texel as a palette index
           directly (no CLUT lookup); the shader replicates that. */
        out->polyMode = 4;
    } else if (s_currentTypePoly == POLY_TEXTURE_Z_FOG) {
        /* Type 24: per-pixel Z→CLUT-row fog; shader replicates it. */
        out->polyMode = 5;
    } else {
        out->polyMode = 0; /* textured (CLUT-based) */
    }

    /* Perspective correctness only for Z-textured types (16-24); type 25 is
       a Z-marker polygon, not textured. */
    out->perspCorrect =
        (s_currentTypePoly >= POLY_TEXTURE_Z &&
         s_currentTypePoly <= POLY_TEXTURE_Z_FOG) ? 1 : 0;

    /* Chroma key: only INCRUST types (12-15, 20-23; bit 2 of the type)
       discard palette index 0. */
    if (s_currentTypePoly >= POLY_TEXTURE &&
        s_currentTypePoly <= POLY_TEXTURE_Z_FOG) {
        out->chromaKey = (s_currentTypePoly & 4) ? 1 : 0;
    }

    /* Blending for TRANS/TRAME (alphaMode 1/2). INCRUST chroma-keying is a
       discard, not blending, so Z-write stays enabled there. */
    out->blend = (out->alphaMode != 0);

    out->clutBaseRow = s_batchCLUTBaseRow;
    out->scaledFogNear = s_batchScaledFogNear;
    out->fogRowScale = s_batchFogRowScale;
    out->bilinear = s_bilinearCLUT;

    /* Depth rules (see the reference driver's flush for the reasoning):
       - types 0-24 depth-test GEQUAL, the mirror of the SW fillers'
         per-pixel `PtrZBuffer[offset] >= zInt` test: a batch occluded by
         closer geometry is skipped, so NZW body polys do not overpaint
         terrain nearer to the camera (and show through walls).
       - Z-write: types 16-24 always, types 0-15 only when the batch ran
         under a Z-buffered filler (terrain); suppressed for NZW and for
         blended (TRANS/TRAME) batches; types 0-15 without a Z-buffered
         filler Z-test only (matches SW ZBufBoxOverWrite2). NZW never
         writes depth, so coplanar polys compare only against the stored
         terrain depth and cannot fight one another. 3D-pass batches
         (bodies) test only: the software filler they run under writes no
         Z, and depth must stay terrain-owned or a shadow span's
         ground-plane GEQUAL culls against body depth that outlives the
         frame that wrote it. */
    if (s_currentTypePoly <= POLY_TEXTURE_Z_FOG) {
        out->depthTest = 1; /* GEQUAL */
        if (s_batchZBuffer || s_currentTypePoly >= POLY_TEXTURE_Z) {
            out->depthWrite =
                (!s_batchNZW && !out->blend && !s_batch3DPass) ? 1 : 0;
        } else {
            out->depthWrite = 0;
        }
    } else {
        out->depthTest = 0;
        out->depthWrite = 0;
    }

    out->screenScale[0] = s_screenScale[0];
    out->screenScale[1] = s_screenScale[1];
}

//──────────────────────────────────────────────────────────────────────────
// Batch lifecycle
//──────────────────────────────────────────────────────────────────────────
void GpuRenderer_FlushBatch(void) {
    if (!s_active || s_batchVertCount == 0) {
        return;
    }

    GpuDrawParams params;
    Common_BuildDrawParams(&params);
    s_backend->DrawTriangles((const GpuVertex *)s_batchVerts,
                             s_batchVertCount, &params);

    s_batchVertCount = 0;
    s_batchTriCount = 0;
}

//──────────────────────────────────────────────────────────────────────────
// The engine's 3D pass (see GpuRenderer_Begin3DPass in GPU/GPURENDERER.H).
//──────────────────────────────────────────────────────────────────────────
void GpuRenderer_Begin3DPass(void) {
    if (!s_active) {
        return;
    }
    Fill_Poly_Set3DPass(1);
}

void GpuRenderer_End3DPass(void) {
    if (!s_active) {
        return;
    }
    Fill_Poly_Set3DPass(0);
}

//──────────────────────────────────────────────────────────────────────────
// Scene-shadow spans (see GpuRenderer_ShadowBegin in GPU/GPURENDERER.H).
//──────────────────────────────────────────────────────────────────────────
/* Shadow alpha derived from the exact CLUT row ShadeBoxBlk uses, so both fill
   paths agree per palette and per fog level. Pairs the row's target index to
   its source colour through the present LUT and averages the luminance loss:
   a row that dims a colour to 85% yields alpha 0.15. Falls back to the
   linear level/15 model when the table chain is unavailable. */
static float Common_ShadowAlpha(S32 level) {
    const U32 *lut;
    const U8 *row;
    S32 c;
    U32 count = 0;
    double ratioSum = 0.0;

    if (PtrCLUTGouraud == NULL) {
        return (float)level / SHADOW_LEVEL_DIVISOR;
    }
    lut = GL_GetPaletteLUT();
    if (lut == NULL) {
        return (float)level / SHADOW_LEVEL_DIVISOR;
    }
    level = (level < 0) ? 0 : ((level > SHADOW_LEVEL_DIVISOR) ? 15 : level);
    row = PtrCLUTGouraud + (S32)((SHADOW_LEVEL_DIVISOR - (float)level) * 256.0f);
    for (c = 0; c < 256; c++) {
        U32 src = lut[c];
        U32 dst = lut[row[c]];
        double lumSrc = (double)(src >> 16 & 0xFF) * 0.299 +
                        (double)(src >> 8 & 0xFF) * 0.587 +
                        (double)(src & 0xFF) * 0.114;
        if (lumSrc < 0.5) {
            continue; /* index 0 / transparent: no ratio to take */
        }
        ratioSum += 1.0 - ((double)(dst >> 16 & 0xFF) * 0.299 +
                           (double)(dst >> 8 & 0xFF) * 0.587 +
                           (double)(dst & 0xFF) * 0.114) / lumSrc;
        count++;
    }
    if (count == 0) {
        return (float)level / SHADOW_LEVEL_DIVISOR;
    }
    ratioSum /= (double)count;
    if (ratioSum < 0.0) {
        ratioSum = 0.0;
    } else if (ratioSum > 1.0) {
        ratioSum = 1.0;
    }
    return (float)ratioSum;
}

static void Common_ShadowVertex(GpuVertex *v, S32 x, S32 y) {
    v->XE = (S16)x;
    v->YE = (S16)y;
    v->MapU = 0;
    v->MapV = 0;
    v->Light = 0;
    /* Ground-plane clip-Z, in the same GET_ZO space the terrain tiles carry:
       spans pass the GEQUAL depth test where the stored surface is at-or-
       farther, and are culled where nearer terrain already owns the pixel. */
    v->ZO = (U16)s_shadowPlaneAZ;
    v->RepMask = 0;
}

/* Draws the accumulated spans and resets the buffer. One flat-black quad per
   span: vertex pairs at x0 and x1+1 so the rasterizer covers pixel columns
   x0..x1 inclusive. Depth-tests (GEQUAL) against the FBO the terrain left
   behind so a risen ridge or wall in front of the shadow culls it, mirroring
   the SW DrawRecover occlusion; depth-write stays off so later blends are not
   consumed. */
static void Common_FlushShadowSpans(void) {
    GpuDrawParams params;

    if (!s_active || s_shadowVertCount == 0) {
        s_shadowVertCount = 0;
        return;
    }

    memset(&params, 0, sizeof params);
    params.polyMode = 6;
    params.alphaMode = 0;
    params.flatColor = s_shadowAlpha;
    params.blend = 1;
    params.depthTest = 1;
    params.depthWrite = 0;
    params.screenScale[0] = s_screenScale[0];
    params.screenScale[1] = s_screenScale[1];

    s_backend->DrawTriangles((const GpuVertex *)s_shadowVerts,
                             s_shadowVertCount, &params);
    s_shadowVertCount = 0;
}

void GpuRenderer_ShadowBegin(S32 level, S32 zPlane) {
    if (!s_active) {
        return;
    }
    /* Land the pending polygons first so the shadow composites over them. */
    GpuRenderer_FlushBatch();
    s_shadowVertCount = 0;
    s_shadowAlpha = Common_ShadowAlpha(level);
    s_shadowPlaneAZ = zPlane;
    s_shadowActive = 1;
}

void GpuRenderer_ShadowSpan(S32 x0, S32 y, S32 x1) {
    GpuVertex *v;
    S32 x0c;
    S32 x1c;

    if (!s_shadowActive) {
        return;
    }
    if (y < 0 || y >= (S32)s_resY) {
        return;
    }
    if (x0 > x1) {
        S32 t = x0;
        x0 = x1;
        x1 = t;
    }
    x0c = (x0 < 0) ? 0 : x0;
    x1c = (x1 >= (S32)s_resX) ? (S32)s_resX - 1 : x1;
    if (x0c > x1c) {
        return;
    }

    if (s_shadowVertCount + 6 > SHADOW_MAX_VERTS) {
        Common_FlushShadowSpans();
    }

    v = &s_shadowVerts[s_shadowVertCount];
    Common_ShadowVertex(&v[0], x0c, y);
    Common_ShadowVertex(&v[1], x1c + 1, y);
    Common_ShadowVertex(&v[2], x1c + 1, y + 1);
    Common_ShadowVertex(&v[3], x0c, y);
    Common_ShadowVertex(&v[4], x1c + 1, y + 1);
    Common_ShadowVertex(&v[5], x0c, y + 1);
    s_shadowVertCount += 6;
}

void GpuRenderer_ShadowEnd(void) {
    if (!s_shadowActive) {
        return;
    }
    s_shadowActive = 0;
    Common_FlushShadowSpans();
}

static void Common_PackVertex(GpuVertex *dst, const Struc_Point *src,
                              U16 repMask) {
    /* XE, YE, MapU, MapV, Light, ZO are the first 12 bytes of both records;
       Pt_W (S32) is engine-side only and never travels. */
    memcpy(dst, src, 12);
    dst->RepMask = repMask;
}

//──────────────────────────────────────────────────────────────────────────
// Polygon intake. A polygon is handed here by the Fill_Poly intercept; the
// return value tells the caller whether the GPU path took it (TRUE) or it
// must rasterize itself (FALSE = SW fallback).
//──────────────────────────────────────────────────────────────────────────
S32 GpuRenderer_RenderTriangleList(S32 typePoly, S32 colorPoly,
                                   S32 nbPoints, Struc_Point *ptrPoints) {
    if (!s_active) {
        return 0;
    }

    /* Types 0-7 and 8-24 are handled; anything else falls back to SW. */
    if (typePoly >= 0 && typePoly <= 7) {
        /* SOLID/FLAT/TRANS/TRAME/GOURAUD/DITHER (+_TABLE), all handled. */
    } else if (typePoly < POLY_TEXTURE || typePoly > POLY_TEXTURE_Z_FOG) {
        return 0;
    }

    /* Non-Z-buffered body polys join the FBO only while the engine holds the
       3D pass open around AffScene's object display (GpuRenderer_Begin3DPass):
       there they carry the GEQUAL depth-test and no depth-write that types
       0-15 already resolve to, at the target's render-scaled resolution.
       Outside the pass — menus' spinning models, holomap, credits — they
       rasterize into the Log, which the present composites over the target,
       and ShadeBoxBlk keeps real pixels to darken there. Without terrain in
       the target the Log path stays authoritative, mirroring the NZW rule. */
    if (!Fill_Flag_ZBuffer) {
        if (!Fill_Flag_3DPass || !s_fboHasTerrain) {
            return 0;
        }
    }

    /* NZW animated polys (flowers, water) take the GPU batch only once the
       FBO holds valid terrain depth, so they depth-test against it; before
       that (menus, modal loops) they follow the SW path. */
    if (Fill_Flag_NZW && !s_fboHasTerrain) {
        return 0;
    }

    if (nbPoints < 3) {
        return 0;
    }

    /* Menu plasma/fire bars reuse POLY_TEXTURE (type 8) with RepMask
       0x3F3F but belong to the 2D overlay, not the 3D FBO. Let the SW
       rasterizer place them. */
    if (typePoly == POLY_TEXTURE && (RepMask & 0xFFFF) == 0x3F3F) {
        return 0;
    }

    /* Flush on any change that alters the draw state: type, colour, atlas
       page, CLUT fog row, Z-buffer mode, or 3D-pass mode. SetCLUT runs
       per terrain tile with a different fog distance, so a tile boundary
       must flush or neighbouring tiles would render at the wrong
       brightness. */
    if (s_batchVertCount > 0) {
        if (s_currentTypePoly != (U32)typePoly ||
            s_currentColorPoly != (U32)colorPoly ||
            (typePoly >= POLY_TEXTURE &&
             s_batchAtlas != (const void *)PtrMap) ||
            s_batchCLUTGouraud != (const void *)PtrCLUTGouraud ||
            s_batchZBuffer != Fill_Flag_ZBuffer ||
            s_batchNZW != Fill_Flag_NZW ||
            s_batch3DPass != Fill_Flag_3DPass) {
            GpuRenderer_FlushBatch();
        }
    }

    /* Capture state at batch start. */
    s_currentTypePoly = (U32)typePoly;
    s_currentColorPoly = (U32)colorPoly;
    if (typePoly >= POLY_TEXTURE) {
        s_batchAtlas = (const void *)PtrMap;
    }
    s_batchCLUTGouraud = (const void *)PtrCLUTGouraud;
    s_batchZBuffer = Fill_Flag_ZBuffer;
    s_batchNZW = Fill_Flag_NZW;
    s_batch3DPass = Fill_Flag_3DPass;

    /* CLUT base row: SetCLUT(fog) per terrain tile sets PtrCLUTGouraud
       within PtrCLUTFog; the batch captured on a CLUT change keeps the row
       it started with so per-tile fog brightness stays consistent. */
    s_batchCLUTBaseRow = 0.0f;
    if (PtrCLUTFog != NULL && PtrCLUTGouraud != NULL) {
        s_batchCLUTBaseRow = (float)(((const U8 *)PtrCLUTGouraud -
                                      (const U8 *)PtrCLUTFog) / 256);
    }

    /* Fog-smooth constants: SetFog runs once per scene load, stable across
       the whole batch. */
    s_batchScaledFogNear = (float)Fill_ScaledFogNear;
    s_batchFogRowScale = (float)((double)Fill_Fog_Factor / 65536.0);

    /* Textures. CLUT and palette upload every batch (fog table and palette
       mutate in place); the atlas only when its page changed. */
    Common_UploadCLUT();
    if (typePoly >= POLY_TEXTURE) {
        Common_UploadAtlas();
    }
    Common_UploadPalette();

    /* Fan the polygon into the batch. The repeat mask travels per-vertex so
       a flush mid-batch does not smear one poly's mask over another. */
    const U16 polyRepMask = (U16)(RepMask & 0xFFFF);
    for (S32 i = 1; i < nbPoints - 1; i++) {
        if (s_batchVertCount + 3 > BATCH_MAX_VERTS) {
            GpuRenderer_FlushBatch();
        }
        GpuVertex *dst = (GpuVertex *)(s_batchVerts + s_batchVertCount * 14);
        Common_PackVertex(dst + 0, &ptrPoints[0], polyRepMask);
        Common_PackVertex(dst + 1, &ptrPoints[i], polyRepMask);
        Common_PackVertex(dst + 2, &ptrPoints[i + 1], polyRepMask);
        s_batchVertCount += 3;
        s_batchTriCount++;
    }

    return 1; /* handled */
}

//──────────────────────────────────────────────────────────────────────────
// Frame control
//──────────────────────────────────────────────────────────────────────────
void GpuRenderer_BeginFrame(void) {
    /* Read the texture filter once per frame so a mid-frame FlushBatch uses
       the correct bilinear/nearest mode for every batch. */
    s_bilinearCLUT = DisplayTexFilter;
}

void GpuRenderer_ClearFBO(void) {
    if (!s_active) {
        return;
    }
    /* Land anything batched since the last present (a camera-cut restart
       re-enters here mid-frame) so the clear wipes it: flushing after the
       clear would draw that stale batch over the fresh target. */
    GpuRenderer_FlushBatch();
    s_backend->TargetBindClear();
    /* The FBO now holds valid terrain depth for the rest of the frame (and
       stays valid across frames on no-terrain frames — ClearFBO runs at the
       start of the terrain pass only). */
    s_fboHasTerrain = 1;
}

void GpuRenderer_SetOverlayOpaqueRect(S32 x0, S32 y0, S32 x1, S32 y1) {
    s_overlayOpaqueX0 = x0;
    s_overlayOpaqueY0 = y0;
    s_overlayOpaqueX1 = x1;
    s_overlayOpaqueY1 = y1;
}

void GpuRenderer_Present(const void *argbPixels, U32 pitch, U32 width,
                         U32 height, S32 filterLinear) {
    if (!s_active || argbPixels == NULL) {
        return;
    }

    /* The overlay sampler filter is the player's Bilinear toggle, the same
       DisplayTexFilter the CLUT sampling reads at BeginFrame — not the seam's
       filterLinear argument, whose only caller passes FALSE (the svg layer
       must not reference renderer-owned state). Bilinear here smooths the Log
       layer — bodies, sprites, HUD — wherever the present scales it. */
    filterLinear = DisplayTexFilter;

    /* Flush any 3D geometry batched during the frame into the offscreen
       target before the composite reads it. */
    GpuRenderer_FlushBatch();

    if (s_overlayOpaqueX1 > s_overlayOpaqueX0 &&
        s_overlayOpaqueY1 > s_overlayOpaqueY0) {
        /* Modal full-2D UI region: Log's palette index 0 (alpha 0) there
           presents as opaque black like the software present. Texture
           uploads copy immediately, so patch before the upload. */
        U32 xEnd = (U32)s_overlayOpaqueX1;
        U32 yEnd = (U32)s_overlayOpaqueY1;
        if (xEnd > width) {
            xEnd = width;
        }
        if (yEnd > height) {
            yEnd = height;
        }
        for (U32 y = (U32)s_overlayOpaqueY0; y < yEnd; y++) {
            U32 *px = (U32 *)((U8 *)argbPixels + (size_t)y * pitch);
            for (U32 x = (U32)s_overlayOpaqueX0; x < xEnd; x++) {
                if ((px[x] & 0xFF000000u) == 0) {
                    px[x] = 0xFF000000u;
                }
            }
        }
    }

    /* The 2D overlay (Log buffer as ARGB) is the topmost layer. */
    s_backend->TexUpdatePresent(argbPixels, pitch, width, height, filterLinear);

    /* ClearBackbuffer: window-level clear + letterbox viewport setup.
       DrawPresentQuad: 0 = offscreen colour into the window, then
       1 = the 2D overlay on top. The backend presents inside the last
       call (swap / submit). */
    s_backend->ClearBackbuffer();
    s_backend->DrawPresentQuad(0, filterLinear);
    s_backend->DrawPresentQuad(1, filterLinear);
}

// Refills PtrZBuffer from the GPU depth target (converted to the engine's aZ
// space) so the object pass's ZBufBoxOverWrite2 tests against the terrain the
// GPU drew. A backend without depth readback leaves the software Z-buffer.
//──────────────────────────────────────────────────────────────────────────
void GpuRenderer_ReadbackDepthBuffer(void) {
    if (!s_active || PtrZBuffer == NULL) {
        return;
    }

    /* Flush pending geometry so the target holds the latest depth. */
    GpuRenderer_FlushBatch();

    const U32 fboW = s_backend->TargetWidth();
    const U32 fboH = s_backend->TargetHeight();
    if (fboW == 0 || fboH == 0) {
        return;
    }

    const U32 totalPixels = fboW * fboH;
    static U16 *s_depthBuf = NULL;
    static U32 s_depthBufSize = 0;
    if (s_depthBufSize < totalPixels) {
        free(s_depthBuf);
        s_depthBuf = (U16 *)malloc(totalPixels * sizeof(U16));
        s_depthBufSize = s_depthBuf ? totalPixels : 0;
    }
    if (s_depthBuf == NULL) {
        return;
    }

    if (!s_backend->TargetReadDepth(s_depthBuf, fboW, fboH)) {
        return; /* backend without depth readback: software Z-buffer stays */
    }

    const U32 scrW = s_resX;
    const U32 scrH = s_resY;

    /* The GL vertex shader outputs Z = 1.0 - aZ/65535; the viewport
       transform stores depth = (2.0 - aZ/65535)/2. As DEPTH_COMPONENT16:
       gl_depth_16 = (131070 - aZ)/2. Inverse: aZ = 131070 - 2*gl_depth_16.
       Cleared depth 0 maps to aZ 131070, larger than any valid value, so
       ZBufBoxOverWrite2 always keeps shadows there. */
    if (fboW == scrW && fboH == scrH) {
        for (U32 sy = 0; sy < scrH; sy++) {
            const U16 *srcRow = s_depthBuf + (scrH - 1 - sy) * scrW;
            U16 *dstRow = PtrZBuffer + sy * scrW;
            for (U32 sx = 0; sx < scrW; sx++) {
                U32 glDepth = srcRow[sx];
                dstRow[sx] = (U16)(131070u - 2u * glDepth);
            }
        }
    } else {
        /* SSAA: downsample, keeping the closest (minimum aZ) sample per
           screen-pixel block. */
        const U32 scaleX = fboW / scrW;
        const U32 scaleY = fboH / scrH;
        for (U32 sy = 0; sy < scrH; sy++) {
            U16 *dstRow = PtrZBuffer + sy * scrW;
            for (U32 sx = 0; sx < scrW; sx++) {
                U16 minAZ = 0x7FFF;
                const U32 fx0 = sx * scaleX;
                const U32 fy0 = (fboH - 1 - sy * scaleY) - scaleY + 1;
                for (U32 fy = fy0; fy < fy0 + scaleY; fy++) {
                    const U16 *srcRow = s_depthBuf + fy * fboW;
                    for (U32 fx = fx0; fx < fx0 + scaleX; fx++) {
                        U32 glDepth = srcRow[fx];
                        U16 az = (U16)(131070u - 2u * glDepth);
                        if (az < minAZ) {
                            minAZ = az;
                        }
                    }
                }
                dstRow[sx] = minAZ;
            }
        }
    }
}

//──────────────────────────────────────────────────────────────────────────
// Terrain snapshot/restore (see GpuRenderer_SnapshotTerrain in
// GPU/GPURENDERER.H). End of the exterior terrain pass: the common layer pins
// a copy of the target it just drew so a later AFF_OBJETS frame can scrub the
// shadow/body paint the paused target would otherwise smear. Restore repaints
// that terrain before the batch queued ahead of it, so this frame's animated
// polys land on top of the scrubbed terrain exactly as BoxClean +
// DrawAnimatedPolys do on Log.
//──────────────────────────────────────────────────────────────────────────
void GpuRenderer_SnapshotTerrain(void) {
    if (!s_active) {
        return;
    }
    /* Land the terrain tiles still in the batch so the target holds the whole
       pass before it is copied. */
    GpuRenderer_FlushBatch();
    s_terrainValid = s_backend->TargetSnapshotTerrain() ? 1 : 0;
}

void GpuRenderer_RestoreTerrain(void) {
    if (!s_active || !s_terrainValid) {
        return;
    }
    /* Composite the stored terrain first, then land whatever the AFF_OBJETS
       head already queued (that frame's animated polys): the FLUSH comes after
       so those polys draw over the scrubbed terrain, not under it. */
    s_backend->TargetRestoreTerrain();
    GpuRenderer_FlushBatch();
}

//──────────────────────────────────────────────────────────────────────────
// Lifecycle
//──────────────────────────────────────────────────────────────────────────
S32 GpuRenderer_Init(GpuRendererBackendId backend, U32 resX, U32 resY) {
    /* NONE is the software rasterizer, which this layer never runs. */
    if (backend == GPU_RENDERER_BACKEND_NONE ||
        backend >= GPU_RENDERER_BACKEND_COUNT) {
        return FALSE;
    }

    if (s_active) {
        GpuRenderer_Shutdown();
    }

    const GpuRendererBackend *be = Common_TableLookup(backend);
    if (be == NULL) {
        Log_Warn("[GPU] backend %d not built into this binary", (int)backend);
        return FALSE;
    }

    if (!be->CreateContext(resX, resY)) {
        Log_Warn("[GPU] backend '%s' CreateContext failed", be->identifier);
        return FALSE;
    }

    s_backend = be;
    s_backendId = backend;
    s_resX = resX;
    s_resY = resY;
    s_renderQuality = 1;
    Common_UpdateScreenScale();
    s_active = 1;
    s_fboHasTerrain = 0;

    if (!be->TargetCreate(resX * s_renderQuality, resY * s_renderQuality)) {
        Log_Warn("[GPU] backend '%s' TargetCreate failed", be->identifier);
        GpuRenderer_Shutdown();
        return FALSE;
    }

    /* Wire the engine seams to this renderer once the backend is live. The
       present seam (SDL.CPP) and the polygon fill seam (POLY.CPP) stay NULL
       on the software path, which the ASM equivalence tests link without any
       GPU dependency — GpuRenderer_Init is what the tests never reach. */
    SetGpuPresentHooks(GpuRenderer_Present, GpuRenderer_IsActive);
    Fill_Poly_SetHook(GpuRenderer_RenderTriangleList);
    Fill_Poly_Set3DPass(0);

    /* Registers the pacing setter and replays the value chosen before init
       (the config reaches Window_SetVSync before the renderer exists). */
    SetGpuVSyncHook(GpuRenderer_SetVSync);

    Log_Info("[GPU] renderer active: %s (%ux%u)", be->identifier, resX, resY);
    return TRUE;
}

void GpuRenderer_Shutdown(void) {
    if (!s_active) {
        return;
    }

    /* Drop the seams first so a torn-down renderer never drives a frame. */
    SetGpuPresentHooks(NULL, NULL);
    SetGpuVSyncHook(NULL);
    Fill_Poly_SetHook(NULL);
    Fill_Poly_Set3DPass(0);

    s_backend->TargetDestroy();
    s_backend->DestroyContext();
    s_backend = NULL;
    s_backendId = GPU_RENDERER_BACKEND_NONE;
    s_active = 0;
    s_fboHasTerrain = 0;
    s_terrainValid = 0;
    s_batchVertCount = 0;
    s_batchTriCount = 0;
    s_batchCLUTBaseRow = 0.0f;
    s_batchScaledFogNear = 0.0f;
    s_batchFogRowScale = 0.0f;
    s_batchCLUTGouraud = NULL;
    s_batchZBuffer = 0;
    s_batchNZW = 0;
    s_batch3DPass = 0;
    s_uploadedAtlas = NULL;
    s_resX = 0;
    s_resY = 0;
}

void GpuRenderer_Resize(U32 resX, U32 resY) {
    if (!s_active) {
        return;
    }

    s_resX = resX;
    s_resY = resY;
    Common_UpdateScreenScale();

    /* A different present size invalidates whatever terrain depth the target
       held, and the terrain target is recreated alongside. */
    s_fboHasTerrain = 0;
    s_terrainValid = 0;

    s_backend->TargetDestroy();
    if (!s_backend->TargetCreate(resX * s_renderQuality,
                                 resY * s_renderQuality)) {
        Log_Warn("[GPU] TargetCreate failed after resize to %ux%u",
                 resX, resY);
    }
}

void GpuRenderer_SetRenderQuality(U32 quality) {
    if (quality == 0) {
        quality = 1;
    }
    if (quality > 4) {
        quality = 4;
    }
    if (s_renderQuality == quality) {
        return;
    }
    Log_Info("[GPU] render quality %u -> %u (target %ux%u)", s_renderQuality,
             quality, s_resX * quality, s_resY * quality);
    s_renderQuality = quality;
    if (!s_active) {
        return;
    }

    /* A new render scale rebuilds the target; terrain depth is lost. */
    s_fboHasTerrain = 0;
    s_terrainValid = 0;
    s_backend->TargetDestroy();
    if (!s_backend->TargetCreate(s_resX * s_renderQuality,
                                 s_resY * s_renderQuality)) {
        Log_Warn("[GPU] TargetCreate failed at quality %u", quality);
    }
}

void GpuRenderer_SetVSync(S32 enabled) {
    s_vsyncWanted = enabled ? 1 : 0;
    if (s_active) {
        s_backend->SetVSync(s_vsyncWanted);
    }
}

//──────────────────────────────────────────────────────────────────────────
// Queries
//──────────────────────────────────────────────────────────────────────────
S32 GpuRenderer_IsActive(void) {
    return s_active ? TRUE : FALSE;
}

GpuRendererBackendId GpuRenderer_ActiveBackend(void) {
    return s_active ? s_backendId : GPU_RENDERER_BACKEND_NONE;
}

const char *GpuRenderer_BackendName(GpuRendererBackendId id) {
    switch (id) {
    case GPU_RENDERER_BACKEND_OPENGL: return "opengl";
    case GPU_RENDERER_BACKEND_SDL3GPU: return "sdl3gpu";
    default: return "none";
    }
}

#else /* !USE_GPURENDERER */

/* Stubs when the GPU renderer libraries are compiled out. */

#include <GPU/GPURENDERER.H>
#include <SYSTEM/ADELINE_TYPES.H>

S32 GpuRenderer_Init(GpuRendererBackendId backend, U32 resX, U32 resY) {
    (void)backend; (void)resX; (void)resY;
    return FALSE;
}
void GpuRenderer_Shutdown(void) {}
void GpuRenderer_Resize(U32 resX, U32 resY) {
    (void)resX; (void)resY;
}
S32 GpuRenderer_IsActive(void) { return FALSE; }
GpuRendererBackendId GpuRenderer_ActiveBackend(void) {
    return GPU_RENDERER_BACKEND_NONE;
}
const char *GpuRenderer_BackendName(GpuRendererBackendId id) {
    (void)id;
    return "none";
}
void GpuRenderer_BeginFrame(void) {}
void GpuRenderer_ClearFBO(void) {}
void GpuRenderer_FlushBatch(void) {}
void GpuRenderer_Present(const void *argbPixels, U32 pitch, U32 width,
                         U32 height, S32 filterLinear) {
    (void)argbPixels; (void)pitch; (void)width; (void)height;
    (void)filterLinear;
}
S32 GpuRenderer_RenderTriangleList(S32 typePoly, S32 colorPoly,
                                   S32 nbPoints, Struc_Point *ptrPoints) {
    (void)typePoly; (void)colorPoly; (void)nbPoints; (void)ptrPoints;
    return 0;
}
void GpuRenderer_SetRenderQuality(U32 quality) { (void)quality; }
void GpuRenderer_ReadbackDepthBuffer(void) {}
void GpuRenderer_SetVSync(S32 enabled) { (void)enabled; }
void GpuRenderer_SetOverlayOpaqueRect(S32 x0, S32 y0, S32 x1, S32 y1) {
    (void)x0; (void)y0; (void)x1; (void)y1;
}
void GpuRenderer_ShadowBegin(S32 level, S32 zPlane) {
    (void)level; (void)zPlane;
}
void GpuRenderer_ShadowSpan(S32 x0, S32 y, S32 x1) {
    (void)x0; (void)y; (void)x1;
}
void GpuRenderer_ShadowEnd(void) {}
void GpuRenderer_SnapshotTerrain(void) {}
void GpuRenderer_RestoreTerrain(void) {}
void GpuRenderer_Begin3DPass(void) {}
void GpuRenderer_End3DPass(void) {}

#endif /* USE_GPURENDERER */