/*
 * Host test for GpuBlend_Walk (LIB386/H/GPU/GPUBLEND_WALK.H).
 *
 * The walk exists so the GPU blend shader can reproduce ScaleSpriteTransp
 * exactly: this test runs both against the same bank, patterned Log and real
 * blend table, reconstructs the software blend from the emitted source
 * indices, and requires byte equality of the framebuffer plus equality of
 * the Screen* bounds contract the caller consumes (SetClip).
 */
#include "test_harness.h"

#include <GPU/GPUBLEND_WALK.H>
#include <SVGA/CLIP.H>
#include <SVGA/SCALESPT.H>
#include <SVGA/SCREEN.H>
#include <SVGA/SCREENXY.H>

#include <stdlib.h>

#define SCR_W 640
#define SCR_H 480

static U8 *g_log;      // Log framebuffer
static U8 *g_golden;   // copy after ScaleSpriteTransp
static U8 *g_work;     // rebuild from walk indices
static U8 *g_idx;      // walk output (screen-strided)
static U8 *g_bank;     // sprite bank with padding for out-of-range reads
static U8 g_transp[65536];

static GpuBlendBounds g_swBounds;

static U32 g_rng = 0x12345678u;

static U32 rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static void fill_pattern(U8 *buf, size_t n, U32 seed) {
    U32 s = seed;
    size_t i;
    for (i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (U8)(s >> 16);
    }
}

static void setup_screen(void) {
    U32 i;
    Log = g_log;
    ModeDesiredX = SCR_W;
    ModeDesiredY = SCR_H;
    for (i = 0; i < SCR_H; i++) {
        TabOffLine[i] = i * SCR_W;
    }
    ClipXMin = 0;
    ClipYMin = 0;
    ClipXMax = SCR_W - 1;
    ClipYMax = SCR_H - 1;
}

/*
 * Bank layout: U32 offset table (entry 0 at +4), 4-byte sprite header,
 * pixel payload, then a large tail — ScaleSpriteTransp's U32 shift quirks
 * can read outside the payload and the software path reads whatever bank
 * memory sits there, so the walk's identical reads must stay in-bounds.
 */
#define BANK_TAIL (17u * 1024u * 1024u)

static void build_bank(S32 w, S32 h, S32 hotX, S32 hotY, U32 pixelSeed) {
    U32 *offsets = (U32 *)g_bank;
    struct GpuBlendSpriteHeader *hdr =
        (struct GpuBlendSpriteHeader *)(g_bank + 4);
    U8 *px = g_bank + 4 + 4;
    S32 n = w * h;
    S32 i;

    offsets[0] = 4;
    hdr->deltaX = (U8)w;
    hdr->deltaY = (U8)h;
    hdr->hotX = (S8)hotX;
    hdr->hotY = (S8)hotY;
    for (i = 0; i < n; i++) {
        // Every fifth texel transparent so the skip path is exercised.
        px[i] = ((i * 37 + w + pixelSeed) % 5 == 0)
                    ? 0
                    : (U8)(1 + ((i * 13 + h + pixelSeed) % 255));
    }
    memset(px + n, 0, BANK_TAIL - 8 - (size_t)n);
}

static void setup(void) {
    fill_pattern(g_transp, sizeof g_transp, 777);
    setup_screen();
}

/* Golden: software blend, then rebuild the same result from walk indices. */
static void run_case(const char *name, S32 x, S32 y, S32 w, S32 h, S32 hotX,
                     S32 hotY, S32 fx, S32 fy, S32 clipL, S32 clipT,
                     S32 clipR, S32 clipB) {
    GpuBlendBounds walkBounds;
    S32 walked;
    S32 i;

    build_bank(w, h, hotX, hotY, 4242);
    fill_pattern(g_log, SCR_W * SCR_H, 9001);
    memcpy(g_work, g_log, SCR_W * SCR_H);

    ClipXMin = clipL;
    ClipYMin = clipT;
    ClipXMax = clipR;
    ClipYMax = clipB;

    ScaleSpriteTransp(0, x, y, fx, fy, g_bank, g_transp);
    g_swBounds.xMin = ScreenXMin;
    g_swBounds.xMax = ScreenXMax;
    g_swBounds.yMin = ScreenYMin;
    g_swBounds.yMax = ScreenYMax;
    memcpy(g_golden, g_log, SCR_W * SCR_H);

    walked = GpuBlend_Walk(0, x, y, fx, fy, g_bank, clipL, clipT, clipR,
                           clipB, g_idx, SCR_W, &walkBounds);

    ASSERT_EQ_INT(g_swBounds.xMin, walkBounds.xMin);
    ASSERT_EQ_INT(g_swBounds.xMax, walkBounds.xMax);
    ASSERT_EQ_INT(g_swBounds.yMin, walkBounds.yMin);
    ASSERT_EQ_INT(g_swBounds.yMax, walkBounds.yMax);

    if (walked) {
        S32 yy;
        for (yy = walkBounds.yMin; yy < walkBounds.yMax; yy++) {
            U8 *dst = g_work + yy * SCR_W + walkBounds.xMin;
            const U8 *srcIdx = g_idx + yy * SCR_W + walkBounds.xMin;
            for (i = walkBounds.xMax - walkBounds.xMin; i != 0; --i) {
                U8 s = *srcIdx++;
                if (s != 0) {
                    *dst = g_transp[((U16)s << 8) | *dst];
                }
                dst++;
            }
        }
    }

    for (i = 0; i < SCR_W * SCR_H; i++) {
        if (g_golden[i] != g_work[i]) {
            printf("# %s: mismatch at (%d,%d) golden=%d walk=%d\n", name,
                   i % SCR_W, i / SCR_W, g_golden[i], g_work[i]);
            ASSERT_EQ_INT(g_golden[i], g_work[i]);
            return;
        }
    }
    ASSERT_TRUE(1);
}

static void test_1to1_centered(void) {
    run_case("1:1 centered", 320, 240, 16, 12, -4, -3, 65536, 65536, 0, 0,
             SCR_W - 1, SCR_H - 1);
}

static void test_1to1_clipped(void) {
    run_case("1:1 clip left/top", 4, 6, 16, 12, -8, -9, 65536, 65536, 20, 30,
             SCR_W - 1, SCR_H - 1);
    run_case("1:1 clip right/bottom", SCR_W - 8, SCR_H - 6, 16, 12, -2, -2,
             65536, 65536, 0, 0, SCR_W - 4, SCR_H - 3);
    run_case("1:1 narrow clip", 100, 100, 16, 12, -4, -4, 65536, 65536, 90,
             95, 120, 130);
}

static void test_1to1_offscreen(void) {
    run_case("1:1 left of clip", -40, 200, 16, 12, 0, 0, 65536, 65536, 5, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("1:1 above clip", 200, -40, 16, 12, 0, 0, 65536, 65536, 0, 5,
             SCR_W - 1, SCR_H - 1);
    run_case("1:1 right of clip", SCR_W + 10, 200, 16, 12, 0, 0, 65536, 65536,
             0, 0, SCR_W - 1, SCR_H - 1);
    run_case("1:1 below clip", 200, SCR_H + 10, 16, 12, 0, 0, 65536, 65536, 0,
             0, SCR_W - 1, SCR_H - 1);
}

static void test_scaled(void) {
    run_case("scaled 2x", 320, 240, 16, 12, -4, -3, 131072, 131072, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("scaled 0.5x", 320, 240, 30, 22, -4, -3, 32768, 32768, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("scaled odd dims", 300, 200, 15, 9, -7, -4, 98304, 163840, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("scaled non-square factors", 300, 200, 13, 7, -3, -5, 196608,
             49152, 0, 0, SCR_W - 1, SCR_H - 1);
}

static void test_scaled_clipped(void) {
    // Left clip feeds the accumulated correction through startTexX.
    run_case("scaled clip left", 4, 240, 24, 16, -8, -6, 131072, 131072, 40,
             0, SCR_W - 1, SCR_H - 1);
    run_case("scaled clip top", 320, 4, 24, 16, -6, -8, 131072, 131072, 0, 40,
             SCR_W - 1, SCR_H - 1);
    run_case("scaled clip right", SCR_W - 6, 240, 24, 16, -2, -4, 131072,
             131072, 0, 0, SCR_W - 3, SCR_H - 1);
    run_case("scaled clip bottom", 320, SCR_H - 6, 24, 16, -4, -2, 131072,
             131072, 0, 0, SCR_W - 1, SCR_H - 3);
    run_case("scaled both clips", 10, 10, 30, 20, -6, -6, 655360, 655360, 50,
             50, 300, 300);
}

static void test_scaled_offscreen(void) {
    run_case("scaled left of clip", -60, 200, 24, 16, 0, 0, 131072, 131072, 5,
             0, SCR_W - 1, SCR_H - 1);
    run_case("scaled above clip", 200, -60, 24, 16, 0, 0, 131072, 131072, 0,
             5, SCR_W - 1, SCR_H - 1);
    run_case("scaled fully right", SCR_W + 40, 200, 24, 16, 0, 0, 131072,
             131072, 0, 0, SCR_W - 1, SCR_H - 1);
    run_case("scaled fully below", 200, SCR_H + 40, 24, 16, 0, 0, 131072,
             131072, 0, 0, SCR_W - 1, SCR_H - 1);
}

static void test_factor_sentinels(void) {
    run_case("factorx zero", 320, 240, 16, 12, 0, 0, 0, 65536, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("factory zero", 320, 240, 16, 12, 0, 0, 65536, 0, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("factorx negative", 320, 240, 16, 12, 0, 0, -1, 65536, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("factory negative", 320, 240, 16, 12, 0, 0, 65536, -1, 0, 0,
             SCR_W - 1, SCR_H - 1);
    run_case("both factors negative", 320, 240, 16, 12, 0, 0, -65536, -65536,
             0, 0, SCR_W - 1, SCR_H - 1);
}

static void test_random_rounds(void) {
    static const S32 factors[] = {16384,  32768,  49152,  65536,  98304,
                                  131072, 196608, 262144, 393216, 524288,
                                  655360, 983040, 1048576};
    S32 round;

    for (round = 0; round < 200; round++) {
        S32 w = 1 + (S32)(rnd() % 40);
        S32 h = 1 + (S32)(rnd() % 30);
        S32 hotX = (S32)(rnd() % 40) - 20;
        S32 hotY = (S32)(rnd() % 40) - 20;
        S32 x = (S32)(rnd() % (SCR_W + 120)) - 60;
        S32 y = (S32)(rnd() % (SCR_H + 120)) - 60;
        S32 fi = (S32)(rnd() % (sizeof factors / sizeof factors[0]));
        S32 fj = (S32)(rnd() % (sizeof factors / sizeof factors[0]));
        S32 fx = factors[fi];
        S32 fy = factors[fj];
        S32 clipL = (S32)(rnd() % 80);
        S32 clipT = (S32)(rnd() % 80);
        S32 clipR = SCR_W - 1 - (S32)(rnd() % 80);
        S32 clipB = SCR_H - 1 - (S32)(rnd() % 80);
        char name[64];

        if (rnd() % 4 == 0) {
            // Degenerate / negative factors on the random path too.
            if (rnd() % 2) {
                fx = (S32)(rnd() % 4) - 2;
            } else {
                fy = -(S32)(rnd() % 65536) - 1;
            }
        }

        sprintf(name, "random round %d", round);
        run_case(name, x, y, w, h, hotX, hotY, fx, fy, clipL, clipT, clipR,
                 clipB);
        if (test_failures != 0) {
            printf("# params: x=%d y=%d %dx%d hot=(%d,%d) f=(%d,%d) "
                   "clip=(%d,%d)-(%d,%d)\n",
                   x, y, w, h, hotX, hotY, fx, fy, clipL, clipT, clipR,
                   clipB);
            return;
        }
    }
}

int main(void) {
    g_log = (U8 *)malloc(SCR_W * SCR_H);
    g_golden = (U8 *)malloc(SCR_W * SCR_H);
    g_work = (U8 *)malloc(SCR_W * SCR_H);
    g_idx = (U8 *)malloc(SCR_W * SCR_H);
    g_bank = (U8 *)malloc(BANK_TAIL);

    setup();

    RUN_TEST(test_1to1_centered);
    RUN_TEST(test_1to1_clipped);
    RUN_TEST(test_1to1_offscreen);
    RUN_TEST(test_scaled);
    RUN_TEST(test_scaled_clipped);
    RUN_TEST(test_scaled_offscreen);
    RUN_TEST(test_factor_sentinels);
    RUN_TEST(test_random_rounds);

    free(g_log);
    free(g_golden);
    free(g_work);
    free(g_idx);
    free(g_bank);

    TEST_SUMMARY();
    return test_failures != 0;
}
