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
// depth-test rules, and the SW fallbacks (body polys when Fill_Flag_ZBuffer is
// clear, NZW polys before the first terrain pass, menu plasma type-8 with
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
static U32 s_resX = 0;  /* present surface size (engine pixel space) */
static U32 s_resY = 0;
static U32 s_renderQuality = 1; /* 1 = native, 2 = 2x, 4 = 4x */

/* Persistent FBO-has-terrain flag (see ClearFBO/BeginFrame). */
static S32 s_fboHasTerrain = 0;

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

static S32 s_bilinearCLUT = 0; /* bilinear CLUT sampling flag for 3D */

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
       - types 0-24 depth-test; NZW batches at ALWAYS (coplanar animated
         polys would z-fight at GEQUAL on 16-bit depth), others GEQUAL so
         bodies/sprites are occluded by closer terrain.
       - Z-write: types 16-24 always, types 0-15 only when the batch ran
         under a Z-buffered filler (terrain); suppressed for NZW and for
         blended (TRANS/TRAME) batches; types 0-15 without a Z-buffered
         filler Z-test only (matches SW ZBufBoxOverWrite2). */
    if (s_currentTypePoly <= POLY_TEXTURE_Z_FOG) {
        out->depthTest = s_batchNZW ? 2 : 1; /* ALWAYS : GEQUAL */
        if (s_batchZBuffer || s_currentTypePoly >= POLY_TEXTURE_Z) {
            out->depthWrite = (!s_batchNZW && !out->blend) ? 1 : 0;
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

    /* Body polygons (non-Z-buffered) rasterize into the Log buffer, not the
       FBO: the FBO holds only what ClearFBO started — terrain depth. Bodies
       drawn to it outside a terrain pass (menus' spinning models) would
       ghost across frames. Log keeps them in the same compositing layer as
       shadows and the 2D overlay, so ShadeBoxBlk has real pixels to
       darken. */
    if (!Fill_Flag_ZBuffer) {
        return 0;
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
       page, CLUT fog row, or Z-buffer mode. SetCLUT runs per terrain tile
       with a different fog distance, so a tile boundary must flush or
       neighbouring tiles would render at the wrong brightness. */
    if (s_batchVertCount > 0) {
        if (s_currentTypePoly != (U32)typePoly ||
            s_currentColorPoly != (U32)colorPoly ||
            (typePoly >= POLY_TEXTURE &&
             s_batchAtlas != (const void *)PtrMap) ||
            s_batchCLUTGouraud != (const void *)PtrCLUTGouraud ||
            s_batchZBuffer != Fill_Flag_ZBuffer ||
            s_batchNZW != Fill_Flag_NZW) {
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
    s_backend->TargetBindClear();
    /* The FBO now holds valid terrain depth for the rest of the frame (and
       stays valid across frames on no-terrain frames — ClearFBO runs at the
       start of the terrain pass only). */
    s_fboHasTerrain = 1;
}

void GpuRenderer_Present(const void *argbPixels, U32 pitch, U32 width,
                         U32 height, S32 filterLinear) {
    if (!s_active || argbPixels == NULL) {
        return;
    }

    /* Flush any 3D geometry batched during the frame into the offscreen
       target before the composite reads it. */
    GpuRenderer_FlushBatch();

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

//──────────────────────────────────────────────────────────────────────────
static S32 Common_ARGBToIndex(const U32 *lut, U32 argb, U8 *bestIdxOut) {
    struct RevEntry {
        U32 argb;
        U8 idx;
    };

    /* paletteLUT has 256 unique entries; sort by ARGB once per readback. */
    static RevEntry s_revLut[256];
    static RevEntry s_sortedLut[256];
    static U8 s_sortedReady = 0;
    if (!s_sortedReady) {
        for (U32 i = 0; i < 256; i++) {
            s_revLut[i].argb = lut[i];
            s_revLut[i].idx = (U8)i;
        }
        memcpy(s_sortedLut, s_revLut, sizeof s_revLut);
        for (U32 i = 1; i < 256; i++) {
            RevEntry key = s_sortedLut[i];
            S32 j = (S32)i - 1;
            while (j >= 0 && s_sortedLut[j].argb > key.argb) {
                s_sortedLut[j + 1] = s_sortedLut[j];
                j--;
            }
            s_sortedLut[j + 1] = key;
        }
        s_sortedReady = 1;
    }

    S32 lo = 0, hi = 255;
    while (lo <= hi) {
        S32 mid = (lo + hi) >> 1;
        U32 midArgb = s_sortedLut[mid].argb;
        if (midArgb == argb) {
            *bestIdxOut = s_sortedLut[mid].idx;
            return 1;
        } else if (midArgb < argb) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    /* No exact match — nearest by Euclidean distance over the full table. */
    {
        U32 bestDist = 0xFFFFFFFF;
        U8 bestIdx = 0;
        for (U32 j = 0; j < 256; j++) {
            U32 c = lut[j];
            S32 dr = (S32)((c >> 16) & 0xFF) - (S32)((argb >> 16) & 0xFF);
            S32 dg = (S32)((c >> 8) & 0xFF) - (S32)((argb >> 8) & 0xFF);
            S32 db = (S32)(c & 0xFF) - (S32)(argb & 0xFF);
            U32 dist = (U32)(dr * dr + dg * dg + db * db);
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = (U8)j;
            }
        }
        *bestIdxOut = bestIdx;
    }
    return 0;
}

// Refills the software buffers the object pass reads from the GPU target:
// Screen (terrain colour, converted from ARGB to palette indices) and
// PtrZBuffer (depth, converted to the engine's aZ space). One backend round
// trip fills both; a backend without depth readback leaves PtrZBuffer alone.
//──────────────────────────────────────────────────────────────────────────
void GpuRenderer_ReadbackSceneBuffers(void) {
    if (!s_active || Screen == NULL) {
        return;
    }

    /* Flush pending geometry so the target holds the latest colour/depth. */
    GpuRenderer_FlushBatch();

    const U32 fboW = s_backend->TargetWidth();
    const U32 fboH = s_backend->TargetHeight();
    if (fboW == 0 || fboH == 0) {
        return;
    }

    const U32 totalPixels = fboW * fboH;
    static U32 *s_colorBuf = NULL;
    static U32 s_colorBufSize = 0;
    if (s_colorBufSize < totalPixels) {
        free(s_colorBuf);
        s_colorBuf = (U32 *)malloc(totalPixels * sizeof(U32));
        s_colorBufSize = s_colorBuf ? totalPixels : 0;
    }
    if (s_colorBuf == NULL) {
        return;
    }

    const S32 wantDepth = (PtrZBuffer != NULL) ? 1 : 0;
    static U16 *s_depthBuf = NULL;
    static U32 s_depthBufSize = 0;
    if (wantDepth) {
        if (s_depthBufSize < totalPixels) {
            free(s_depthBuf);
            s_depthBuf = (U16 *)malloc(totalPixels * sizeof(U16));
            s_depthBufSize = s_depthBuf ? totalPixels : 0;
        }
        if (s_depthBuf == NULL) {
            return;
        }
    }

    const S32 haveDepth = s_backend->TargetReadColorDepth(
        s_colorBuf, wantDepth ? s_depthBuf : NULL, fboW, fboH);

    const U32 scrW = s_resX;
    const U32 scrH = s_resY;

    /* Convert ARGB pixels to palette indices, Y-flipping (target rows are
       bottom-up, Screen top-down). */
    const U32 *lut = GL_GetPaletteLUT();
    if (lut != NULL) {
        U8 *dst = (U8 *)Screen;
        if (fboW == scrW && fboH == scrH) {
            for (U32 sy = 0; sy < scrH; sy++) {
                U8 *dstRow = dst + sy * scrW;
                const U32 *srcRow = s_colorBuf + (fboH - 1 - sy) * fboW;
                for (U32 sx = 0; sx < scrW; sx++) {
                    Common_ARGBToIndex(lut, srcRow[sx], &dstRow[sx]);
                }
            }
        } else {
            /* SSAA: pick the center pixel of each screen-pixel block. */
            const U32 scaleX = fboW / scrW;
            const U32 scaleY = fboH / scrH;
            for (U32 sy = 0; sy < scrH; sy++) {
                U8 *dstRow = dst + sy * scrW;
                for (U32 sx = 0; sx < scrW; sx++) {
                    U32 fx = sx * scaleX + scaleX / 2;
                    U32 fy = (fboH - 1 - sy * scaleY - scaleY / 2);
                    Common_ARGBToIndex(lut, s_colorBuf[fy * fboW + fx],
                                       &dstRow[sx]);
                }
            }
        }
    }

    if (!haveDepth) {
        return; /* backend without depth readback: software Z-buffer stays */
    }

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

    Log_Info("[GPU] renderer active: %s (%ux%u)", be->identifier, resX, resY);
    return TRUE;
}

void GpuRenderer_Shutdown(void) {
    if (!s_active) {
        return;
    }

    /* Drop the seams first so a torn-down renderer never drives a frame. */
    SetGpuPresentHooks(NULL, NULL);
    Fill_Poly_SetHook(NULL);

    s_backend->TargetDestroy();
    s_backend->DestroyContext();
    s_backend = NULL;
    s_backendId = GPU_RENDERER_BACKEND_NONE;
    s_active = 0;
    s_fboHasTerrain = 0;
    s_batchVertCount = 0;
    s_batchTriCount = 0;
    s_batchCLUTBaseRow = 0.0f;
    s_batchScaledFogNear = 0.0f;
    s_batchFogRowScale = 0.0f;
    s_batchCLUTGouraud = NULL;
    s_batchZBuffer = 0;
    s_batchNZW = 0;
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
       held. */
    s_fboHasTerrain = 0;

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
    s_renderQuality = quality;
    if (!s_active) {
        return;
    }

    /* A new render scale rebuilds the target; terrain depth is lost. */
    s_fboHasTerrain = 0;
    s_backend->TargetDestroy();
    if (!s_backend->TargetCreate(s_resX * s_renderQuality,
                                 s_resY * s_renderQuality)) {
        Log_Warn("[GPU] TargetCreate failed at quality %u", quality);
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
void GpuRenderer_ReadbackSceneBuffers(void) {}

#endif /* USE_GPURENDERER */