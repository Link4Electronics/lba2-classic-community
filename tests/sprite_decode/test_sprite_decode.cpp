/*
 * Host test for GpuSprite_DecodeRle / GpuSprite_DecodeRaw
 * (LIB386/H/GPU/GPUSPRITE_DECODE.H).
 *
 * Opaque world sprites land in the mode-7 RG8 page as R = palette index,
 * G = valid. RLE skips and raw color-0 must leave G = 0 (the fragment
 * shader discards); every painted texel must be G = 1 with the right index.
 * Also checks the bounds guards a corrupt bank relies on.
 */
#include "test_harness.h"

#include <GPU/GPUSPRITE_DECODE.H>
#include <string.h>

static U8 atlas[64 * 64 * 2];

static void clear_atlas(void) {
    memset(atlas, 0, sizeof atlas);
}

/* One control byte then its payload, matching AffGraph. */
static void test_rle_simple_copy(void) {
    static const U8 sprite[] = {
        3, 1, 0, 0, /* 3×1 */
        1,          /* NbBlock=1 */
        0x40,       /* op=01 copy, raw=0 → counter=1 → 1 pixel */
        0x2A,       /* pixel */
    };
    clear_atlas();
    ASSERT_TRUE(GpuSprite_DecodeRle(sprite, sizeof sprite, atlas, 64, 0, 0));
    ASSERT_EQ_INT(0x2A, atlas[0]);
    ASSERT_EQ_INT(1, atlas[1]);
}

static void test_rle_skip_leaves_valid_zero(void) {
    static const U8 sprite[] = {
        4, 1, 0, 0,
        2,          /* NbBlock=2 */
        0x01,       /* op=00 skip, raw=1 → counter=2 → skip 2 */
        0x40,       /* op=01 copy, raw=0 → 1 pixel */
        0x33,
    };
    clear_atlas();
    ASSERT_TRUE(GpuSprite_DecodeRle(sprite, sizeof sprite, atlas, 64, 0, 0));
    ASSERT_EQ_INT(0, atlas[1]);  /* skip pixel 0 */
    ASSERT_EQ_INT(0, atlas[3]);  /* skip pixel 1 (byte pair) */
    ASSERT_EQ_INT(0x33, atlas[4]);
    ASSERT_EQ_INT(1, atlas[5]);
}

static void test_rle_repeat(void) {
    static const U8 sprite[] = {
        4, 1, 0, 0,
        1,
        0x82,       /* op=10 repeat, raw=2 → counter=3 → three pixels */
        0x55,
    };
    clear_atlas();
    ASSERT_TRUE(GpuSprite_DecodeRle(sprite, sizeof sprite, atlas, 64, 0, 0));
    ASSERT_EQ_INT(0x55, atlas[0]);
    ASSERT_EQ_INT(1, atlas[1]);
    ASSERT_EQ_INT(0x55, atlas[2]);
    ASSERT_EQ_INT(1, atlas[3]);
    ASSERT_EQ_INT(0x55, atlas[4]);
    ASSERT_EQ_INT(1, atlas[5]);
    ASSERT_EQ_INT(0, atlas[6]); /* untouched past the run */
}

static void test_rle_rejects_overrun(void) {
    static const U8 sprite[] = {
        8, 1, 0, 0,
        1,
        0x7F, /* copy, raw=63 → counter=64, but no payload bytes */
    };
    clear_atlas();
    ASSERT_TRUE(!GpuSprite_DecodeRle(sprite, sizeof sprite, atlas, 64, 0, 0));
}

static void test_raw_zero_is_transparent(void) {
    static const U8 sprite[] = {
        3, 2, 0, 0,
        0, 1, 2, /* row 0 */
        3, 0, 4, /* row 1 */
    };
    clear_atlas();
    ASSERT_TRUE(GpuSprite_DecodeRaw(sprite, sizeof sprite, atlas, 64, 0, 0));
    /* row 0 */
    ASSERT_EQ_INT(0, atlas[1]);   /* color 0 → G=0 */
    ASSERT_EQ_INT(1, atlas[3]);
    ASSERT_EQ_INT(1, atlas[5]);
    /* row 1 at atlas y=1 */
    ASSERT_EQ_INT(1, atlas[64 * 2 + 1]);
    ASSERT_EQ_INT(0, atlas[64 * 2 + 3]);
    ASSERT_EQ_INT(1, atlas[64 * 2 + 5]);
}

static void test_raw_rejects_short_body(void) {
    static const U8 sprite[] = { 4, 4, 0, 0, 1, 2, 3 }; /* claims 16 pixels */
    clear_atlas();
    ASSERT_TRUE(!GpuSprite_DecodeRaw(sprite, sizeof sprite, atlas, 64, 0, 0));
}

static void test_raw_rejects_empty(void) {
    static const U8 sprite[] = { 0, 0, 0, 0 };
    clear_atlas();
    ASSERT_TRUE(!GpuSprite_DecodeRaw(sprite, sizeof sprite, atlas, 64, 0, 0));
}

static void test_dispatch_by_format(void) {
    static const U8 raw[] = { 1, 1, 0, 0, 0x77 };
    static const U8 rle[] = { 1, 1, 0, 0, 1, 0x40, 0x77 };
    clear_atlas();
    ASSERT_TRUE(GpuSprite_Decode(1, raw, sizeof raw, atlas, 64, 0, 0));
    ASSERT_EQ_INT(0x77, atlas[0]);
    ASSERT_EQ_INT(1, atlas[1]);
    clear_atlas();
    ASSERT_TRUE(GpuSprite_Decode(0, rle, sizeof rle, atlas, 64, 0, 0));
    ASSERT_EQ_INT(0x77, atlas[0]);
    ASSERT_EQ_INT(1, atlas[1]);
}

int main(void) {
    RUN_TEST(test_rle_simple_copy);
    RUN_TEST(test_rle_skip_leaves_valid_zero);
    RUN_TEST(test_rle_repeat);
    RUN_TEST(test_rle_rejects_overrun);
    RUN_TEST(test_raw_zero_is_transparent);
    RUN_TEST(test_raw_rejects_short_body);
    RUN_TEST(test_raw_rejects_empty);
    RUN_TEST(test_dispatch_by_format);
    TEST_SUMMARY();
    return test_failures != 0;
}
