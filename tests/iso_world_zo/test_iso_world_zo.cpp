/*
 * Host test for GpuRenderer_IsoWorldZO (LIB386/H/GPU/GPURENDERER.H).
 *
 * Interior brick quads stamp this aZ on all six vertices and write depth
 * under GEQUAL with the clear at 0 (farthest): smaller aZ is nearer. The
 * iso camera's depth axis is world (x+z) — Map2Screen's ground term — so
 * the function must invert that sum, ignore y, and clamp the S16 grid sum
 * into the 16-bit field. Grid coords are 0..SIZE_CUBE, so the interesting
 * failures are the ends of the domain and the ordering relationship
 * sprites will later share, not a handful of in-range samples.
 */
#include "test_harness.h"

#include <GPU/GPURENDERER.H>

static void test_origin_is_farthest(void) {
    ASSERT_EQ_INT(65535, GpuRenderer_IsoWorldZO(0, 0, 0));
}

static void test_nearer_sum_is_smaller_az(void) {
    /* Larger x+z is nearer on screen → smaller aZ under GEQUAL. */
    ASSERT_EQ_INT(65534, GpuRenderer_IsoWorldZO(1, 0, 0));
    ASSERT_EQ_INT(65534, GpuRenderer_IsoWorldZO(0, 0, 1));
    ASSERT_EQ_INT(65533, GpuRenderer_IsoWorldZO(1, 0, 1));
    ASSERT_TRUE(GpuRenderer_IsoWorldZO(10, 0, 10) <
                GpuRenderer_IsoWorldZO(5, 0, 5));
    ASSERT_TRUE(GpuRenderer_IsoWorldZO(63, 255, 63) <
                GpuRenderer_IsoWorldZO(0, 0, 0));
}

static void test_y_is_ignored(void) {
    ASSERT_EQ_INT(GpuRenderer_IsoWorldZO(10, 0, 20),
                  GpuRenderer_IsoWorldZO(10, 1, 20));
    ASSERT_EQ_INT(GpuRenderer_IsoWorldZO(10, 0, 20),
                  GpuRenderer_IsoWorldZO(10, 4096, 20));
}

static void test_x_and_z_are_symmetric(void) {
    ASSERT_EQ_INT(GpuRenderer_IsoWorldZO(3, 7, 11),
                  GpuRenderer_IsoWorldZO(11, 7, 3));
}

static void test_negative_sum_clamps_to_zero(void) {
    ASSERT_EQ_INT(65535, GpuRenderer_IsoWorldZO(-1, 0, 0));
    ASSERT_EQ_INT(65535, GpuRenderer_IsoWorldZO(0, 0, -8));
    ASSERT_EQ_INT(65535, GpuRenderer_IsoWorldZO(-40, 0, -40));
}

static void test_oversized_sum_clamps_to_65535(void) {
    ASSERT_EQ_INT(0, GpuRenderer_IsoWorldZO(65535, 0, 0));
    ASSERT_EQ_INT(0, GpuRenderer_IsoWorldZO(40000, 0, 40000));
    ASSERT_EQ_INT(0, GpuRenderer_IsoWorldZO(65535, 0, 65535));
}

static void test_retail_grid_ends(void) {
    /* SIZE_CUBE_X/Z are 64: (0,0) farthest, (63,63) nearest of the room. */
    ASSERT_EQ_INT(65535, GpuRenderer_IsoWorldZO(0, 0, 0));
    ASSERT_EQ_INT(65535 - 126, GpuRenderer_IsoWorldZO(63, 0, 63));
}

int main(void) {
    RUN_TEST(test_origin_is_farthest);
    RUN_TEST(test_nearer_sum_is_smaller_az);
    RUN_TEST(test_y_is_ignored);
    RUN_TEST(test_x_and_z_are_symmetric);
    RUN_TEST(test_negative_sum_clamps_to_zero);
    RUN_TEST(test_oversized_sum_clamps_to_65535);
    RUN_TEST(test_retail_grid_ends);
    TEST_SUMMARY();
    return test_failures != 0;
}
