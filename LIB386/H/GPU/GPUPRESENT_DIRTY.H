//──────────────────────────────────────────────────────────────────────────
// Log overlay upload rect (GpuScene phase 3)
//──────────────────────────────────────────────────────────────────────────
// The present uploads the Log overlay into the backend's texture every
// frame, whole surface, whatever the Log carries. With the scene on the
// GPU (GpuScene) the Log holds only what software still draws — plates,
// re-blits, punches, text, modals — so most of it reads index 0 and the
// upload can shrink to the region that carries content.
//
// The region is the bounding box of this frame's nonzero texels unioned
// with last frame's, because the texture still holds last frame's
// pixels: content that vanished must be rewritten with this frame's
// zeros, or it stays on the presented screen as a ghost. Empty bounds
// with empty history upload nothing (the texture already reads all
// zero); a surface size change drops the history, since the backend
// recreated its texture, uploads everything, and the old rect's
// coordinates do not belong to the new surface.
//
// The host test (tests/gpu_present_dirty) drives random frame sequences
// through a simulated texture under exactly these rules and requires
// texture == frame after every present: the ghosting property the union
// exists for, checked rather than argued.
#ifndef LIB_GPUPRESENT_DIRTY
#define LIB_GPUPRESENT_DIRTY

#include <stddef.h> /* NULL, size_t */
#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GpuPresentRect_ {
    S32 x;
    S32 y;
    S32 w; ///< 0 = nothing to upload
    S32 h;
} GpuPresentRect;

static void GpuPresent_RectFull(GpuPresentRect *r, S32 width, S32 height) {
    r->x = 0;
    r->y = 0;
    r->w = width;
    r->h = height;
}

static void GpuPresent_RectEmpty(GpuPresentRect *r) {
    r->x = 0;
    r->y = 0;
    r->w = 0;
    r->h = 0;
}

// Bounding box of the nonzero texels of one indexed frame. Rows are
// `pitch` bytes apart; only the first `width` bytes of each row count —
// pitch padding is not content. out->w is 0 when nothing draws.
static void GpuPresent_DirtyBounds(const U8 *pixels, U32 pitch, U32 width,
                                   U32 height, GpuPresentRect *out) {
    U32 y;
    S32 xMin = 0;
    S32 xMax = -1;
    U32 yTop = 0;
    U32 yBot = 0;
    S32 found = 0;

    GpuPresent_RectEmpty(out);

    if (pixels == NULL || width == 0 || height == 0) {
        return;
    }

    for (y = 0; y < height; y++) {
        const U8 *row = pixels + (size_t)y * (size_t)pitch;
        S32 rowMin = -1;
        S32 rowMax = -1;
        U32 x;

        for (x = 0; x < width; x++) {
            if (row[x] != 0) {
                if (rowMin < 0) {
                    rowMin = (S32)x;
                }
                rowMax = (S32)x;
            }
        }
        if (rowMin < 0) {
            continue;
        }
        if (!found) {
            found = 1;
            yTop = y;
            xMin = rowMin;
            xMax = rowMax;
        } else {
            if (rowMin < xMin) {
                xMin = rowMin;
            }
            if (rowMax > xMax) {
                xMax = rowMax;
            }
        }
        yBot = y;
    }

    if (!found) {
        return;
    }
    out->x = xMin;
    out->y = (S32)yTop;
    out->w = xMax - xMin + 1;
    out->h = (S32)(yBot - yTop + 1);
}

// The rect to upload this frame (union of this frame's bounds and last
// frame's, unless history is void) and the bounds to remember for next
// frame (this frame's own — not the union, which next frame's union will
// cover again through remember alone).
static void GpuPresent_PlanUpload(const GpuPresentRect *bounds,
                                  const GpuPresentRect *prev, S32 sizeChanged,
                                  GpuPresentRect *upload,
                                  GpuPresentRect *remember) {
    S32 x0, y0, x1, y1;

    *remember = *bounds;

    if (sizeChanged || prev->w <= 0 || prev->h <= 0) {
        *upload = *bounds;
        return;
    }
    if (bounds->w <= 0 || bounds->h <= 0) {
        *upload = *prev;
        return;
    }

    x0 = bounds->x < prev->x ? bounds->x : prev->x;
    y0 = bounds->y < prev->y ? bounds->y : prev->y;
    x1 = bounds->x + bounds->w;
    if (prev->x + prev->w > x1) {
        x1 = prev->x + prev->w;
    }
    y1 = bounds->y + bounds->h;
    if (prev->y + prev->h > y1) {
        y1 = prev->y + prev->h;
    }
    upload->x = x0;
    upload->y = y0;
    upload->w = x1 - x0;
    upload->h = y1 - y0;
}

#ifdef __cplusplus
}
#endif

#endif // LIB_GPUPRESENT_DIRTY
