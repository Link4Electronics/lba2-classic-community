//──────────────────────────────────────────────────────────────────────────
// Sprite atlas decode (GpuScene phase 3)
//──────────────────────────────────────────────────────────────────────────
// Pure RG8 decode of the two opaque world-sprite layouts into a shelf-
// packed atlas row. R = palette index, G = valid (0/1); the mode-7 shader
// discards G = 0 so transparent texels and AffGraph RLE skips leave the FBO
// alone. Header-only so host tests can exercise the whole domain without
// linking the renderer.
#ifndef LIB_GPUSPRITE_DECODE
#define LIB_GPUSPRITE_DECODE

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

// AffGraph / SPRITES.HQR RLE: header {Delta_X, Delta_Y, Hot_X, Hot_Y} then
// per-line NbBlock of 00=skip / 01=copy / 10=repeat (op in the top 2 bits,
// count in the low 6, +1). Mirrors Common_DecodeBrick / ClippingGraph.
// Returns FALSE if the stream runs past maxBytes or overruns deltaX.
static S32 GpuSprite_DecodeRle(const U8 *data, U32 maxBytes, U8 *atlas,
                               U32 atlasW, S32 ox, S32 oy) {
    const U8 deltaX = data[0];
    const U8 nbLine = data[1];
    const U8 *graphics = data + 4;
    const U8 *end = data + maxBytes;
    S32 line;

    for (line = 0; line < (S32)nbLine; line++) {
        U8 nbBlock;
        S32 x = 0;
        U8 j;

        if (graphics >= end) {
            return FALSE;
        }
        nbBlock = *graphics++;
        for (j = 0; j < nbBlock; j++) {
            U8 control;
            U8 opCode;
            U32 counter;
            U8 *px;

            if (graphics >= end) {
                return FALSE;
            }
            control = *graphics;
            opCode = (U8)(control >> 6);
            counter = (U32)(control & 0x3F);
            px = atlas + ((U32)(oy + line) * atlasW + (U32)(ox + x)) * 2;

            if (opCode == 0) {
                graphics++;
                x += (S32)counter + 1; /* skip: leave valid = 0 */
            } else {
                counter++; /* one more, same as AffGraph */
                if (opCode == 1) {
                    if (graphics + 1 + counter > end) {
                        return FALSE;
                    }
                    graphics++;
                    while (counter > 0) {
                        px[0] = *graphics++;
                        px[1] = 1;
                        x++;
                        px += 2;
                        counter--;
                    }
                } else {
                    U8 color;
                    if (graphics + 2 > end) {
                        return FALSE;
                    }
                    color = graphics[1];
                    graphics += 2;
                    while (counter > 0) {
                        px[0] = color;
                        px[1] = 1;
                        x++;
                        px += 2;
                        counter--;
                    }
                }
            }
        }
        if (x > (S32)deltaX) {
            return FALSE;
        }
    }
    return TRUE;
}

// ScaleSprite / SPRIRAW.HQR: same 4-byte header, body is Delta_X*Delta_Y
// raw palette bytes. Color 0 is the transparent sentinel ScaleSprite skips,
// so it stays G = 0; every other index is G = 1.
static S32 GpuSprite_DecodeRaw(const U8 *data, U32 maxBytes, U8 *atlas,
                               U32 atlasW, S32 ox, S32 oy) {
    const U32 w = data[0];
    const U32 h = data[1];
    const U8 *src = data + 4;
    U32 row;
    U32 need;

    if (w == 0 || h == 0) {
        return FALSE;
    }
    need = w * h;
    if (maxBytes < 4 + need) {
        return FALSE;
    }
    for (row = 0; row < h; row++) {
        U8 *dst = atlas + ((U32)(oy + (S32)row) * atlasW + (U32)ox) * 2;
        U32 col;
        for (col = 0; col < w; col++) {
            U8 px = src[row * w + col];
            dst[0] = px;
            dst[1] = (px != 0) ? 1 : 0;
            dst += 2;
        }
    }
    return TRUE;
}

// One decode step for either layout (rawFormat: 0 = RLE, 1 = raw).
static S32 GpuSprite_Decode(S32 rawFormat, const U8 *data, U32 maxBytes,
                            U8 *atlas, U32 atlasW, S32 ox, S32 oy) {
    if (rawFormat) {
        return GpuSprite_DecodeRaw(data, maxBytes, atlas, atlasW, ox, oy);
    }
    return GpuSprite_DecodeRle(data, maxBytes, atlas, atlasW, ox, oy);
}

#ifdef __cplusplus
}
#endif

#endif // LIB_GPUSPRITE_DECODE
