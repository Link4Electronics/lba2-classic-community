/*
 * Host test for the Log overlay upload rect (LIB386/H/GPU/GPUPRESENT_DIRTY.H).
 *
 * Two halves: the bounding-box scan and the plan (union / history / size
 * change) against hand-chosen cases, then a randomized simulation — random
 * content frames, including empty and moving ones and surface size changes,
 * applied to a simulated backend texture under the rules the backends
 * implement (recreated texture uploads everything; otherwise only the
 * planned rect), requiring texture == frame after every present. That
 * equality is the ghosting property the union exists for.
 */
#include "test_harness.h"

#include <GPU/GPUPRESENT_DIRTY.H>

#include <stdlib.h>

#define MAX_W 96
#define MAX_H 72

static U32 g_rng = 0xC0FFEEu;

static U32 rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

/* ── DirtyBounds ───────────────────────────────────────────────────────── */

static void test_bounds_full(void) {
    U8 frame[MAX_W * MAX_H];
    GpuPresentRect r;
    U32 i;

    for (i = 0; i < MAX_W * MAX_H; i++) {
        frame[i] = (U8)(1 + (i & 0x7F));
    }
    GpuPresent_DirtyBounds(frame, MAX_W, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(0, r.x);
    ASSERT_EQ_INT(0, r.y);
    ASSERT_EQ_INT(MAX_W, r.w);
    ASSERT_EQ_INT(MAX_H, r.h);
}

static void test_bounds_empty(void) {
    U8 frame[MAX_W * MAX_H];
    GpuPresentRect r;

    memset(frame, 0, sizeof(frame));
    GpuPresent_DirtyBounds(frame, MAX_W, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(0, r.w);
    ASSERT_EQ_INT(0, r.h);

    GpuPresent_DirtyBounds(NULL, MAX_W, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(0, r.w);
    GpuPresent_DirtyBounds(frame, MAX_W, 0, MAX_H, &r);
    ASSERT_EQ_INT(0, r.w);
    GpuPresent_DirtyBounds(frame, MAX_W, MAX_W, 0, &r);
    ASSERT_EQ_INT(0, r.w);
}

static void test_bounds_single_pixel(void) {
    U8 frame[MAX_W * MAX_H];
    GpuPresentRect r;

    memset(frame, 0, sizeof(frame));
    frame[20 * MAX_W + 10] = 7;
    GpuPresent_DirtyBounds(frame, MAX_W, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(10, r.x);
    ASSERT_EQ_INT(20, r.y);
    ASSERT_EQ_INT(1, r.w);
    ASSERT_EQ_INT(1, r.h);
}

static void test_bounds_two_corners(void) {
    U8 frame[MAX_W * MAX_H];
    GpuPresentRect r;

    memset(frame, 0, sizeof(frame));
    frame[3 * MAX_W + 5] = 1;      /* near top-left */
    frame[60 * MAX_W + 90] = 2;    /* near bottom-right */
    GpuPresent_DirtyBounds(frame, MAX_W, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(5, r.x);
    ASSERT_EQ_INT(3, r.y);
    ASSERT_EQ_INT(90 - 5 + 1, r.w);
    ASSERT_EQ_INT(60 - 3 + 1, r.h);
}

static void test_bounds_pitch_padding_ignored(void) {
    U8 frame[MAX_H * (MAX_W + 8)];
    GpuPresentRect r;
    U32 y;

    memset(frame, 0, sizeof(frame));
    /* Nonzero bytes only in the pitch padding: not content. */
    for (y = 0; y < MAX_H; y++) {
        memset(frame + y * (MAX_W + 8) + MAX_W, 0xEE, 8);
    }
    GpuPresent_DirtyBounds(frame, MAX_W + 8, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(0, r.w);
    ASSERT_EQ_INT(0, r.h);

    frame[10 * (MAX_W + 8) + 4] = 3;
    GpuPresent_DirtyBounds(frame, MAX_W + 8, MAX_W, MAX_H, &r);
    ASSERT_EQ_INT(4, r.x);
    ASSERT_EQ_INT(10, r.y);
    ASSERT_EQ_INT(1, r.w);
    ASSERT_EQ_INT(1, r.h);
}

/* ── PlanUpload ────────────────────────────────────────────────────────── */

static GpuPresentRect rect(S32 x, S32 y, S32 w, S32 h) {
    GpuPresentRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static void test_plan_no_history(void) {
    GpuPresentRect bounds = rect(4, 6, 10, 12);
    GpuPresentRect prev = rect(0, 0, 0, 0);
    GpuPresentRect upload, remember;

    GpuPresent_PlanUpload(&bounds, &prev, 0, &upload, &remember);
    ASSERT_EQ_INT(4, upload.x);
    ASSERT_EQ_INT(6, upload.y);
    ASSERT_EQ_INT(10, upload.w);
    ASSERT_EQ_INT(12, upload.h);
    /* remember is always this frame's own bounds, not the union. */
    ASSERT_EQ_INT(4, remember.x);
    ASSERT_EQ_INT(10, remember.w);

    bounds = rect(0, 0, 0, 0);
    GpuPresent_PlanUpload(&bounds, &prev, 0, &upload, &remember);
    ASSERT_EQ_INT(0, upload.w);
    ASSERT_EQ_INT(0, upload.h);
}

static void test_plan_size_change_drops_history(void) {
    GpuPresentRect bounds = rect(1, 2, 3, 4);
    GpuPresentRect prev = rect(30, 40, 20, 20);
    GpuPresentRect upload, remember;

    GpuPresent_PlanUpload(&bounds, &prev, 1, &upload, &remember);
    ASSERT_EQ_INT(1, upload.x);
    ASSERT_EQ_INT(2, upload.y);
    ASSERT_EQ_INT(3, upload.w);
    ASSERT_EQ_INT(4, upload.h);
    ASSERT_EQ_INT(1, remember.x);
    ASSERT_EQ_INT(3, remember.w);
}

static void test_plan_content_vanished(void) {
    GpuPresentRect bounds = rect(0, 0, 0, 0);
    GpuPresentRect prev = rect(3, 4, 6, 7);
    GpuPresentRect upload, remember;

    /* The ghost case: this frame carries nothing, the texture still shows
       last frame's content — the history region must be rewritten. */
    GpuPresent_PlanUpload(&bounds, &prev, 0, &upload, &remember);
    ASSERT_EQ_INT(3, upload.x);
    ASSERT_EQ_INT(4, upload.y);
    ASSERT_EQ_INT(6, upload.w);
    ASSERT_EQ_INT(7, upload.h);
    ASSERT_EQ_INT(0, remember.w);
}

static void test_plan_union(void) {
    GpuPresentRect bounds = rect(10, 10, 5, 5);
    GpuPresentRect prev = rect(0, 0, 20, 20);
    GpuPresentRect upload, remember;

    GpuPresent_PlanUpload(&bounds, &prev, 0, &upload, &remember);
    ASSERT_EQ_INT(0, upload.x);
    ASSERT_EQ_INT(0, upload.y);
    ASSERT_EQ_INT(20, upload.w);
    ASSERT_EQ_INT(20, upload.h);
    ASSERT_EQ_INT(10, remember.x);
    ASSERT_EQ_INT(5, remember.w);

    /* Disjoint on x only: union spans the gap. */
    bounds = rect(50, 0, 4, 4);
    prev = rect(0, 0, 10, 4);
    GpuPresent_PlanUpload(&bounds, &prev, 0, &upload, &remember);
    ASSERT_EQ_INT(0, upload.x);
    ASSERT_EQ_INT(54, upload.x + upload.w);
    ASSERT_EQ_INT(4, upload.h);
}

/* ── Simulation: texture == frame after every present ──────────────────── */

static void paint_random_content(U8 *frame, U32 w, U32 h) {
    U32 k = rnd() % 6; /* often 0: an empty frame */
    U32 i;

    memset(frame, 0, (size_t)w * h);
    for (i = 0; i < k; i++) {
        U32 rx = rnd() % w;
        U32 ry = rnd() % h;
        U32 rw = 1 + rnd() % (w - rx);
        U32 rh = 1 + rnd() % (h - ry);
        U8 v = (U8)(1 + rnd() % 255);
        U32 y, x;

        for (y = 0; y < rh; y++) {
            for (x = 0; x < rw; x++) {
                frame[(ry + y) * w + (rx + x)] = v;
            }
        }
    }
}

static void test_simulation(void) {
    enum { W1 = 64, H1 = 48, W2 = 80, H2 = 60, FRAMES = 400 };
    static U8 frame[MAX_W * MAX_H];
    static U8 texture[MAX_W * MAX_H];
    U32 w = W1, h = H1;
    GpuPresentRect prev;
    GpuPresentRect bounds, upload, remember;
    S32 justCreated = 1;
    S32 sizeChanged = 0;
    U32 t;
    U32 maxUploadArea = 0;
    U32 fullUploads = 0;

    GpuPresent_RectEmpty(&prev);
    memset(texture, 0xA5, sizeof(texture)); /* undefined until first create */

    for (t = 0; t < FRAMES; t++) {
        if ((rnd() % 40) == 0) {
            /* Surface size change: the backend recreates its texture. */
            w = (w == W1) ? W2 : W1;
            h = (h == H1) ? H2 : H1;
            sizeChanged = 1;
            justCreated = 1;
        }
        paint_random_content(frame, w, h);

        GpuPresent_DirtyBounds(frame, w, w, h, &bounds);
        GpuPresent_PlanUpload(&bounds, &prev, sizeChanged, &upload, &remember);

        if (justCreated) {
            /* Backend rule: a (re)created texture is uploaded in full
               regardless of the planned rect. */
            memcpy(texture, frame, (size_t)w * h);
            justCreated = 0;
            fullUploads++;
        } else if (upload.w > 0 && upload.h > 0) {
            U32 y;

            for (y = 0; y < (U32)upload.h; y++) {
                memcpy(texture + (U32)upload.y * w + (U32)upload.x + y * w,
                       frame + (U32)upload.y * w + (U32)upload.x + y * w,
                       (size_t)upload.w);
            }
            if ((U32)upload.w * (U32)upload.h > maxUploadArea) {
                maxUploadArea = (U32)upload.w * (U32)upload.h;
            }
        }

        ASSERT_TRUE(memcmp(texture, frame, (size_t)w * h) == 0);

        prev = remember;
        sizeChanged = 0;
    }

    /* The shrink must actually shrink: with random sparse content the plan
       never needs the whole surface on a non-create frame. */
    ASSERT_TRUE(maxUploadArea < (U32)W2 * H2);
    ASSERT_TRUE(fullUploads < FRAMES);
}

int main(void) {
    RUN_TEST(test_bounds_full);
    RUN_TEST(test_bounds_empty);
    RUN_TEST(test_bounds_single_pixel);
    RUN_TEST(test_bounds_two_corners);
    RUN_TEST(test_bounds_pitch_padding_ignored);
    RUN_TEST(test_plan_no_history);
    RUN_TEST(test_plan_size_change_drops_history);
    RUN_TEST(test_plan_content_vanished);
    RUN_TEST(test_plan_union);
    RUN_TEST(test_simulation);
    TEST_SUMMARY();
    return test_failures != 0;
}
