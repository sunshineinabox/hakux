/* Copyright (C) 2026-present PortareOS */
/* PVIDEO register and YUV conversion tests. */
#include "hw/xbox/hle/pb/pvideo.h"
#include "hle_test.h"
#include <string.h>

#define WH(w, h) ((uint32_t)(w) | (uint32_t)(h) << 16)

static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

/* No scaling. */
static void test_unity(void)
{
    HlePvideoRegs r = { .size_in = WH(640, 480), .ds_dx = HLE_PVIDEO_UNITY,
                        .dt_dy = HLE_PVIDEO_UNITY, .size_out = WH(640, 480),
                        .format = 0x10500 };
    HlePvideo p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_w, 640);
    HT_EQ_U32(p.in_h, 480);
    HT_CHECK(p.scale_x == 1.0f && p.scale_y == 1.0f);
    HT_CHECK(p.in_x == 0.0f && p.in_y == 0.0f);
    HT_EQ_U32(p.out_w, 640);
    HT_EQ_U32(p.out_h, 480);
    HT_CHECK(!p.color_key_on);          /* 0x10500: YUY2, pitch 1280, no key */
}

/* Scale 480 input lines into SpyHunter's 437-line window. */
static void test_shrink(void)
{
    HlePvideoRegs r = { .size_in = WH(640, 480), .ds_dx = HLE_PVIDEO_UNITY,
                        .dt_dy = (480u << 20) / 437, .point_out = WH(0, 21),
                        .size_out = WH(640, 437) };
    HlePvideo p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_h, 480);
    HT_CHECK(near(p.scale_y, 480.0f / 437.0f));
    HT_EQ_U32(p.out_y, 21);
    HT_EQ_U32(p.out_h, 437);
    /* 720 wide into 640 (Worms 4: Mayhem): all 720 columns */
    r = (HlePvideoRegs){ .size_in = WH(720, 480), .ds_dx = (720u << 20) / 640,
                         .dt_dy = HLE_PVIDEO_UNITY, .size_out = WH(640, 480) };
    p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_w, 720);
    HT_CHECK(near(p.scale_x, 1.125f));
}

/* Cap oversized inputs and stretch smaller ones. */
static void test_cap(void)
{
    HlePvideoRegs r = { .size_in = WH(1024, 1024), .ds_dx = HLE_PVIDEO_UNITY,
                        .dt_dy = HLE_PVIDEO_UNITY, .size_out = WH(640, 480) };
    HlePvideo p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_w, 640);
    HT_EQ_U32(p.in_h, 480);
    r = (HlePvideoRegs){ .size_in = WH(640, 448), .ds_dx = HLE_PVIDEO_UNITY,
                         .dt_dy = (448u << 20) / 480, .size_out = WH(640, 480) };
    p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_h, 448);
    HT_CHECK(near(p.scale_y, 448.0f / 480.0f));
    /* from POINT_IN on: 720 columns shown from x 80 at unity in a 640
     * window read all 720, rows from y 16 their 16 more */
    r = (HlePvideoRegs){ .size_in = WH(720, 576), .point_in = (80u * 16) | (16u * 16) << 16,
                         .ds_dx = HLE_PVIDEO_UNITY, .dt_dy = HLE_PVIDEO_UNITY,
                         .size_out = WH(640, 480) };
    p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_w, 720);
    HT_EQ_U32(p.in_h, 496);
    r.size_in = WH(2047, 2047);         /* still SIZE_IN's at most, and no more than needed */
    p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_w, 720);
    HT_EQ_U32(p.in_h, 496);
    r.size_in = WH(700, 480);
    p = hle_pvideo_decode(&r);
    HT_EQ_U32(p.in_w, 700);
    HT_EQ_U32(p.in_h, 480);
}

/* Fractional POINT_IN coordinates and colour-key enable bit. */
static void test_point_in_key(void)
{
    HlePvideoRegs r = { .size_in = WH(640, 480), .point_in = (8u * 16 + 8) | (4u * 16 + 3) << 16,
                        .ds_dx = HLE_PVIDEO_UNITY, .dt_dy = HLE_PVIDEO_UNITY,
                        .size_out = WH(640, 480), .format = 0x110500, .color_key = 0xFF101010 };
    HlePvideo p = hle_pvideo_decode(&r);
    HT_CHECK(near(p.in_x, 8.5f));
    HT_CHECK(near(p.in_y, 4.125f));
    HT_CHECK(p.color_key_on);
    HT_EQ_U32(p.color_key, 0xFF101010);
}

static uint8_t clip8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* Colour controls, BT.709 and bit-exact default BT.601 conversion. */
static void test_color(void)
{
    HlePvideoRegs r = { .size_in = WH(640, 480), .size_out = WH(640, 480),
                        .luminance = 0xFFC00800, .chrominance = 0x0800F800, .format = 0x1010500 };
    HlePvideo p = hle_pvideo_decode(&r);
    HT_EQ_INT(p.color.contrast, 0x800);
    HT_EQ_INT(p.color.brightness, -64);
    HT_EQ_INT(p.color.sat_cos, -0x800);
    HT_EQ_INT(p.color.sat_sin, 0x800);
    HT_CHECK(p.color.bt709);
    r.luminance = r.chrominance = HLE_PVIDEO_ONE;
    r.format = 0x10500;
    p = hle_pvideo_decode(&r);
    HT_CHECK(hle_pvideo_color_default(&p.color));
    unsigned bad = 0;
    for (int Y = 0; Y < 256; Y++) {
        for (int U = 0; U < 256; U++) {
            for (int V = 0; V < 256; V++) {
                const int y = Y - 16, u = U - 128, v = V - 128, c = 298 * y;
                uint8_t q[3];
                hle_pvideo_yuv(&p.color, Y, U, V, q);
                bad += q[0] != clip8((c + 516 * u + 128) >> 8) ||
                       q[1] != clip8((c - 100 * u - 208 * v + 128) >> 8) ||
                       q[2] != clip8((c + 409 * v + 128) >> 8);
            }
        }
    }
    HT_EQ_U32(bad, 0);
    /* BT.709: white, black, and its red (Y 63, Cb 102, Cr 240) */
    HlePvideoColor c = { HLE_PVIDEO_ONE, 0, HLE_PVIDEO_ONE, 0, true };
    uint8_t q[3];
    hle_pvideo_yuv(&c, 235, 128, 128, q);
    HT_CHECK(q[0] == 255 && q[1] == 255 && q[2] == 255);
    hle_pvideo_yuv(&c, 16, 128, 128, q);
    HT_CHECK(q[0] == 0 && q[1] == 0 && q[2] == 0);
    hle_pvideo_yuv(&c, 63, 102, 240, q);
    HT_CHECK(q[2] >= 254 && q[1] <= 1 && q[0] <= 1);
    /* half contrast: white is mid grey; no saturation: grey whatever U, V;
     * a half turn of the hue: U and V negated */
    c = (HlePvideoColor){ 0x800, 0, HLE_PVIDEO_ONE, 0, false };
    hle_pvideo_yuv(&c, 235, 128, 128, q);
    HT_CHECK(q[0] == q[1] && q[1] == q[2] && q[0] >= 126 && q[0] <= 129);
    c = (HlePvideoColor){ HLE_PVIDEO_ONE, 0, 0, 0, false };
    hle_pvideo_yuv(&c, 100, 30, 220, q);
    HT_CHECK(q[0] == q[1] && q[1] == q[2]);
    uint8_t n[3];
    c = (HlePvideoColor){ HLE_PVIDEO_ONE, 0, -HLE_PVIDEO_ONE, 0, false };
    hle_pvideo_yuv(&c, 100, 30, 220, q);
    c.sat_cos = HLE_PVIDEO_ONE;
    hle_pvideo_yuv(&c, 100, 226, 36, n);
    HT_CHECK(q[0] == n[0] && q[1] == n[1] && q[2] == n[2]);
}

/* Odd-width YUY2/UYVY: every pixel is written in both colour paths. */
static void test_convert_odd(void)
{
    uint8_t src[2 * 64];
    for (unsigned i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(16 + (i * 37) % 220);
    }
    const HlePvideoColor def = { HLE_PVIDEO_ONE, 0, HLE_PVIDEO_ONE, 0, false };
    const HlePvideoColor warm = { 0x1200, 8, 0xF00, 0x100, true };
    const HlePvideoColor *cs[3] = { NULL, &def, &warm };
    uint8_t ref[3 * 2 * 4];
    for (unsigned uyvy = 0; uyvy < 2; uyvy++) {
        for (unsigned ci = 0; ci < 3; ci++) {
            uint8_t dst[3 * 2 * 4];
            memset(dst, 0xCD, sizeof(dst));
            hle_pvideo_convert(src, 64, 3, 2, uyvy, cs[ci], dst);
            unsigned unwritten = 0;
            for (unsigned i = 0; i < 6; i++) {
                unwritten += dst[4 * i + 3] != 255;
            }
            HT_EQ_U32(unwritten, 0);
            /* the last pixel of each row: the second pair's first Y */
            for (unsigned row = 0; row < 2; row++) {
                const uint8_t *l = src + row * 64 + 4;
                const int Y = uyvy ? l[1] : l[0], U = uyvy ? l[0] : l[1], V = uyvy ? l[2] : l[3];
                uint8_t q[3];
                hle_pvideo_yuv(cs[ci] ? cs[ci] : &def, Y, U, V, q);
                const uint8_t *o = dst + (row * 3 + 2) * 4;
                HT_CHECK(o[0] == q[0] && o[1] == q[1] && o[2] == q[2]);
            }
            if (ci == 0) {
                memcpy(ref, dst, sizeof(ref));
            } else if (ci == 1) {
                HT_CHECK(memcmp(ref, dst, sizeof(ref)) == 0);  /* the defaults: as plain */
            }
        }
    }
}

int main(void)
{
    HT_RUN(test_unity);
    HT_RUN(test_shrink);
    HT_RUN(test_cap);
    HT_RUN(test_point_in_key);
    HT_RUN(test_color);
    HT_RUN(test_convert_odd);
    HT_DONE();
}
