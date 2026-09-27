//──────────────────────────────────────────────────────────────────────────
// Blend-sprite source walk (GpuScene phase 3)
//──────────────────────────────────────────────────────────────────────────
// Header-only port of ScaleSpriteTransp's sampling and Screen* bookkeeping,
// emitting per-texel source indices instead of blending them: the GPU blend
// shader owns the table lookup, so it needs to know, for every texel of the
// clipped rect, which source index the 16.16 walk sampled (0 = the sprite
// skips that texel and the destination keeps its index).
//
// This mirrors ScaleSpriteTransp line for line — the same U32 shift
// trickery, the same asymmetric clip comparisons, the same sentinel bounds —
// because the walk is ASM-validated behaviour: any "equivalent" rewrite
// would diverge on the clip quirks (a left-clipped scaled sprite feeds the
// accumulated correction back through startTexX, and the low half of
// startTex* shifts into a row/column offset only through the U32 casts).
// The host test compares this walk against ScaleSpriteTransp itself,
// byte for byte, through the real blend table.
//
// idx is addressed in screen coordinates (idx[y * stride + x]); rows outside
// the published rect are untouched.
#ifndef LIB_GPUBLEND_WALK
#define LIB_GPUBLEND_WALK

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GpuBlendBounds_ {
    S32 xMin; ///< ScaleSpriteTransp's ScreenXMin (sentinels when nothing draws)
    S32 xMax; ///< ScreenXMax, exclusive edge (sentinels when nothing draws)
    S32 yMin; ///< ScreenYMin (sentinels when nothing draws)
    S32 yMax; ///< ScreenYMax, exclusive edge (sentinels when nothing draws)
} GpuBlendBounds;

// Sprite header inside a SPRITES.HQR raw bank (delta sizes + signed hotspot).
struct GpuBlendSpriteHeader {
    U8 deltaX;
    U8 deltaY;
    S8 hotX;
    S8 hotY;
};

static S32 GpuBlend_MulShift16(S32 left, S32 right) {
    return (S32)(((long long)left * (long long)right) >> 16);
}

static S32 GpuBlend_Divide1616(S32 value) {
    return (S32)(((long long)1 << 32) / (long long)value);
}

static S32 GpuBlend_SpriteCenter1616(U8 delta) {
    return (S32)(((U32)(delta >> 1) << 16) + ((delta & 1u) ? 0x8000u : 0u));
}

static void GpuBlend_SentinelBounds(GpuBlendBounds *bounds) {
    bounds->xMin = 32000;
    bounds->xMax = -32000;
    bounds->yMin = 32000;
    bounds->yMax = -32000;
}

// Walks ScaleSpriteTransp's 1:1 or scaled sampling for bank entry num at
// (x, y) under the inclusive clip rect, writing source indices (0..255) for
// the clipped rect into idx. factorx/factory are 16.16; the <= 0 sentinel /
// 0x7FFFFFFF substitution happens exactly as in ScaleSpriteTransp.
// Returns TRUE when the rect holds texels (bounds valid, exclusive max
// edges), FALSE when nothing draws (bounds are the sentinels and idx
// is untouched) — in which case ScaleSpriteTransp would have blended
// nothing either.
static S32 GpuBlend_Walk(S32 num, S32 x, S32 y, S32 factorx, S32 factory,
                         const void *ptrbank, S32 clipXMin, S32 clipYMin,
                         S32 clipXMax, S32 clipYMax, U8 *idx, S32 stride,
                         GpuBlendBounds *bounds) {
    if (factorx <= 0) {
        if (factorx == 0) {
            GpuBlend_SentinelBounds(bounds);
            return FALSE;
        }
        factorx = 0x7FFFFFFF;
    }
    if (factory <= 0) {
        if (factory == 0) {
            GpuBlend_SentinelBounds(bounds);
            return FALSE;
        }
        factory = 0x7FFFFFFF;
    }

    const U32 *bank = (const U32 *)ptrbank;
    const U8 *spritePtr = (const U8 *)ptrbank + bank[num];
    const struct GpuBlendSpriteHeader *sprite =
        (const struct GpuBlendSpriteHeader *)spritePtr;
    const U8 *srcBase = spritePtr + sizeof(struct GpuBlendSpriteHeader);
    const S32 clipXMaxPlusOne = clipXMax + 1;
    const S32 clipYMaxPlusOne = clipYMax + 1;

    // Fast mode: 1:1 scaling
    if (factorx == 65536 && factory == 65536) {
        S32 leftX = x + (S8)sprite->hotX;
        S32 topY = y + (S8)sprite->hotY;
        S32 rightX = leftX + sprite->deltaX;
        S32 bottomY = topY + sprite->deltaY;

        if (leftX > clipXMaxPlusOne || topY > clipYMaxPlusOne) {
            GpuBlend_SentinelBounds(bounds);
            return FALSE;
        }

        bounds->xMin = leftX;
        bounds->yMin = topY;
        S32 startX = 0, startY = 0;

        if (leftX < clipXMin) {
            startX = clipXMin - leftX;
            bounds->xMin = clipXMin;
        }

        if (topY <= clipYMin) {
            startY = clipYMin - topY;
            bounds->yMin = clipYMin;
        }

        if (rightX < clipXMin) {
            GpuBlend_SentinelBounds(bounds);
            return FALSE;
        }
        bounds->xMax = rightX;
        if (rightX >= clipXMaxPlusOne) {
            bounds->xMax = clipXMaxPlusOne;
        }

        if (bottomY < clipYMin) {
            GpuBlend_SentinelBounds(bounds);
            return FALSE;
        }
        bounds->yMax = bottomY;
        if (bottomY >= clipYMaxPlusOne) {
            bounds->yMax = clipYMaxPlusOne;
        }

        S32 width = bounds->xMax - bounds->xMin;
        S32 height = bounds->yMax - bounds->yMin;

        if (width <= 0 || height <= 0) {
            GpuBlend_SentinelBounds(bounds);
            return FALSE;
        }

        U8 *dstPtr = idx + stride * bounds->yMin + bounds->xMin;
        const U8 *srcPtr = srcBase + startY * sprite->deltaX + startX;

        S32 srcSkip = sprite->deltaX - width;
        S32 dstSkip = stride - width;

        for (S32 scanY = 0; scanY < height; scanY++) {
            for (S32 scanX = 0; scanX < width; scanX++) {
                *dstPtr++ = *srcPtr++;
            }
            srcPtr += srcSkip;
            dstPtr += dstSkip;
        }
        return TRUE;
    }

    // Scaled mode
    S32 newDeltaX = GpuBlend_MulShift16(sprite->deltaX, factorx);
    S32 newDeltaY = GpuBlend_MulShift16(sprite->deltaY, factory);
    S32 newHotX = GpuBlend_MulShift16((S8)sprite->hotX, factorx);
    S32 newHotY = GpuBlend_MulShift16((S8)sprite->hotY, factory);
    S32 invFactorX = GpuBlend_Divide1616(factorx);
    S32 invFactorY = GpuBlend_Divide1616(factory);
    S32 startTexX =
        GpuBlend_SpriteCenter1616(sprite->deltaX) - invFactorX * (newDeltaX >> 1);
    S32 startTexY =
        GpuBlend_SpriteCenter1616(sprite->deltaY) - invFactorY * (newDeltaY >> 1);

    S32 leftX = x + newHotX;
    S32 topY = y + newHotY;
    S32 rightX = leftX + newDeltaX;
    S32 bottomY = topY + newDeltaY;

    bounds->xMin = leftX;
    if (leftX > clipXMaxPlusOne) {
        GpuBlend_SentinelBounds(bounds);
        return FALSE;
    }

    if (leftX < clipXMin) {
        bounds->xMin = clipXMin;
        S32 clipDelta = (clipXMin - 1) - leftX;
        S32 adjusted = startTexX + clipDelta * invFactorX;
        startTexX += adjusted;
    }

    if (rightX < clipXMin) {
        GpuBlend_SentinelBounds(bounds);
        return FALSE;
    }
    bounds->xMax = rightX;
    if (rightX >= clipXMaxPlusOne) {
        bounds->xMax = clipXMaxPlusOne;
    }

    bounds->yMin = topY;
    if (topY > clipYMaxPlusOne) {
        GpuBlend_SentinelBounds(bounds);
        return FALSE;
    }

    if (topY < clipYMin) {
        bounds->yMin = clipYMin;
        S32 clipDelta = (clipYMin - 1) - topY;
        S32 adjusted = startTexY + clipDelta * invFactorY;
        startTexY += adjusted;
    }

    if (bottomY <= clipYMin) {
        GpuBlend_SentinelBounds(bounds);
        return FALSE;
    }
    bounds->yMax = bottomY;
    if (bottomY > clipYMaxPlusOne) {
        bounds->yMax = clipYMaxPlusOne;
    }

    S32 height = bounds->yMax - bounds->yMin;
    S32 width = bounds->xMax - bounds->xMin;
    if (height <= 0 || width <= 0) {
        GpuBlend_SentinelBounds(bounds);
        return FALSE;
    }

    U8 *dstPtr = idx + stride * bounds->yMin + bounds->xMin;

    const U8 *srcPtr = srcBase + ((U32)startTexY >> 16) * sprite->deltaX;
    U32 xFrac = ((U32)startTexX) << 16;
    U32 yFrac = ((U32)startTexY) << 16;
    U32 xDec = ((U32)invFactorX) << 16;
    U32 yDec = ((U32)invFactorY) << 16;
    U32 xInt = ((U32)invFactorX) >> 16;
    U32 yInt = ((U32)invFactorY) >> 16;
    S32 srcRowWidth = sprite->deltaX;

    srcPtr += ((U32)startTexX >> 16);

    U32 pendingRows = 0;
    for (S32 scanY = height; scanY != 0; --scanY) {
        S32 remaining = width;
        const U8 *lineSrc = srcPtr;
        U8 *lineDst = dstPtr;
        U32 lineFrac = xFrac;

        while (remaining-- != 0) {
            *lineDst++ = *lineSrc;

            U32 prevFrac = lineFrac;
            lineFrac += xDec;
            lineSrc += xInt;
            if (lineFrac < prevFrac) {
                ++lineSrc;
            }
        }

        U32 prevYFrac = yFrac;
        yFrac += yDec;
        pendingRows += yInt;
        if (yFrac < prevYFrac) {
            ++pendingRows;
        }

        if (pendingRows != 0) {
            srcPtr += srcRowWidth * pendingRows;
            pendingRows = 0;
        }

        dstPtr += stride;
    }
    return TRUE;
}

#ifdef __cplusplus
}
#endif

#endif // LIB_GPUBLEND_WALK
