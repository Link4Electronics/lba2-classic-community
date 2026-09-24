/*
 * Host test for ShadeBoxBlkOverlay (SOURCES/FLOW_A.CPP) — the GpuScene
 * interior dual-path shadow darken.
 *
 * ShadeBoxBlk maps every destination through the shade CLUT, including
 * index 0. On the software path Log is the framebuffer and is fully covered
 * by the room, so clut[0] is rarely hit. Under GpuScene the room and body
 * live in the FBO and Log is only the overlay: 0 means "transparent, show
 * the FBO". Applying clut[0] (an opaque shade colour — 224 on the retail
 * fog table) painted the whole shadow ellipse as solid holes over the
 * doorway actor.
 *
 * ShadeBoxBlkOverlay is the same darken for non-zero overlay pixels and
 * leaves 0 alone. No ASM original: this is new infrastructure, verified by
 * host expectations rather than byte-equality against FLOW_A.ASM.
 */

#include "test_harness.h"

#include "FLOW.H"

#include <SVGA/CLIP.H>
#include <SVGA/SCREEN.H>
#include <POLYGON/POLY.H>

#include <string.h>

void *Log = 0;
U32 TabOffLine[ADELINE_MAX_Y_RES];
S32 ClipXMin = 0;
S32 ClipYMin = 0;
S32 ClipXMax = 639;
S32 ClipYMax = 479;
U8 *PtrCLUTGouraud = 0;
void *Screen = 0;
U32 ModeDesiredX = 640;
U32 ModeDesiredY = 480;

static U8 clut_gouraud[16 * 256];
static U8 frame[640 * 480];

/* Retail-shaped: clut[0] is opaque (224), every other value maps +row. */
static void init_globals(void) {
    ModeDesiredX = 640;
    ModeDesiredY = 480;
    for (U32 i = 0; i < 480; ++i) {
        TabOffLine[i] = i * 640;
    }
    ClipXMin = 0;
    ClipYMin = 0;
    ClipXMax = 639;
    ClipYMax = 479;
    PtrCLUTGouraud = clut_gouraud;
    for (U32 row = 0; row < 16; ++row) {
        for (U32 value = 0; value < 256; ++value) {
            clut_gouraud[row * 256 + value] =
                (value == 0) ? 224 : (U8)((value + row) & 0xFFu);
        }
    }
    memset(frame, 0, sizeof(frame));
    Log = frame;
}

static void test_zeros_stay_transparent(void) {
    init_globals();
    /* Entire span is transparent overlay — the dual-path hole bug. */
    ShadeBoxBlkOverlay(10, 10, 40, 30, 3);
    for (S32 y = 10; y <= 30; ++y) {
        for (S32 x = 10; x <= 40; ++x) {
            ASSERT_EQ_INT(0, frame[y * 640 + x]);
        }
    }
}

static void test_nonzero_still_darkens(void) {
    init_globals();
    frame[15 * 640 + 20] = 50;
    frame[15 * 640 + 21] = 0;
    /* deccoul=3 → row (15-3)*256 = 12*256; clut[50] = (50+12)&0xFF = 62. */
    ShadeBoxBlkOverlay(20, 15, 21, 15, 3);
    ASSERT_EQ_INT(62, frame[15 * 640 + 20]);
    ASSERT_EQ_INT(0, frame[15 * 640 + 21]);
}

static void test_mixed_span_like_shadow_ellipse(void) {
    init_globals();
    /* Transparent body silhouette with opaque overlay edges, as under the
       dual path when sprites/HUD sit in Log over an FBO-resident body. */
    for (S32 x = 100; x < 130; ++x) {
        frame[50 * 640 + x] = 0;
    }
    frame[50 * 640 + 100] = 80;
    frame[50 * 640 + 129] = 90;

    ShadeBoxBlkOverlay(100, 50, 129, 50, 2);
    /* row (15-2)*256 = 13*256; clut[80]=(80+13)&0xFF=93, clut[90]=103. */
    ASSERT_EQ_INT(93, frame[50 * 640 + 100]);
    ASSERT_EQ_INT(103, frame[50 * 640 + 129]);
    for (S32 x = 101; x < 129; ++x) {
        ASSERT_EQ_INT(0, frame[50 * 640 + x]);
    }
}

static void test_clip_window_respected(void) {
    init_globals();
    ClipXMin = 100;
    ClipYMin = 50;
    ClipXMax = 120;
    ClipYMax = 70;
    frame[60 * 640 + 110] = 40;
    frame[60 * 640 + 90] = 40;  /* outside clip */
    frame[60 * 640 + 130] = 40; /* outside clip */

    ShadeBoxBlkOverlay(90, 60, 130, 60, 4);
    /* row (15-4)*256 = 11*256; clut[40]=51 inside clip. */
    ASSERT_EQ_INT(51, frame[60 * 640 + 110]);
    ASSERT_EQ_INT(40, frame[60 * 640 + 90]);
    ASSERT_EQ_INT(40, frame[60 * 640 + 130]);
}

static void test_all_zero_outside_clip_is_noop(void) {
    init_globals();
    ClipXMin = 200;
    ClipYMin = 200;
    ClipXMax = 220;
    ClipYMax = 220;
    ShadeBoxBlkOverlay(0, 0, 10, 10, 5);
    for (U32 i = 0; i < sizeof(frame); ++i) {
        ASSERT_EQ_INT(0, frame[i]);
    }
}

int main(void) {
    RUN_TEST(test_zeros_stay_transparent);
    RUN_TEST(test_nonzero_still_darkens);
    RUN_TEST(test_mixed_span_like_shadow_ellipse);
    RUN_TEST(test_clip_window_respected);
    RUN_TEST(test_all_zero_outside_clip_is_noop);
    TEST_SUMMARY();
    return test_failures != 0;
}
