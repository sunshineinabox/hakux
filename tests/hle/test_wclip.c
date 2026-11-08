/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/xbox/hle/pb/wclip.h"
#include "hle_test.h"

static uint32_t hz[8], vt[8];
static HleRect out[HLE_CMD_MAX_CLIP];

/* window i: x and y min/max, both included */
static void win(unsigned i, uint32_t x1, uint32_t x2, uint32_t y1, uint32_t y2)
{
    hz[i] = x1 | x2 << 16;
    vt[i] = y1 | y2 << 16;
}

static void none(void)
{
    memset(hz, 0, sizeof(hz));
    memset(vt, 0, sizeof(vt));
}

static bool rect_eq(const HleRect *r, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    return r->x1 == x1 && r->y1 == y1 && r->x2 == x2 && r->y2 == y2;
}

/* What the NV2A draws at (x, y) of a target with the antialiasing ax x ay:
 * the registers read as they are */
static bool drawn(bool exclusive, uint32_t ax, uint32_t ay, int32_t x, int32_t y)
{
    bool used = false, in = false;
    for (unsigned i = 0; i < 8; i++) {
        if (!hz[i] && !vt[i]) {
            continue;
        }
        used = true;
        in |= x >= (int32_t)((hz[i] & 0xFFF) * ax) && x < (int32_t)((((hz[i] >> 16) & 0xFFF) + 1) * ax) &&
              y >= (int32_t)((vt[i] & 0xFFF) * ay) && y < (int32_t)((((vt[i] >> 16) & 0xFFF) + 1) * ay);
    }
    return !used || in != exclusive;
}

/* Every pixel of the area drawn exactly once if drawn() says so, never else */
static void check_exact(bool exclusive, uint32_t ax, uint32_t ay, const HleRect *area)
{
    const int n = hle_pb_window_clip(hz, vt, exclusive, ax, ay, area, out);
    HT_CHECK(n >= -1 && n <= HLE_CMD_MAX_CLIP);
    unsigned bad = 0;
    for (int32_t y = area->y1; y < area->y2; y++) {
        for (int32_t x = area->x1; x < area->x2; x++) {
            unsigned hits = n < 0;
            for (int i = 0; i < n; i++) {
                hits += x >= out[i].x1 && x < out[i].x2 && y >= out[i].y1 && y < out[i].y2;
            }
            bad += hits != (unsigned)drawn(exclusive, ax, ay, x, y);
        }
    }
    HT_EQ_INT(bad, 0);
}

static void test_no_clip(void)
{
    const HleRect area = { 0, 0, 640, 480 };
    none();
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &area, out), -1);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, true, 1, 1, &area, out), -1);
    /* D3D's "no scissors": the first window over the whole surface */
    win(0, 0, 639, 0, 479);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &area, out), -1);
    /* an exclusive window outside the area excludes nothing */
    none();
    win(0, 700, 800, 0, 10);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, true, 1, 1, &area, out), -1);
}

/* One inclusive window: one rectangle (the scissor the draws had before),
 * scaled by the antialiasing and cut to the area (the surface clip) */
static void test_inclusive_one(void)
{
    const HleRect area = { 0, 0, 640, 480 };
    none();
    win(0, 100, 199, 50, 149);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &area, out), 1);
    HT_CHECK(rect_eq(&out[0], 100, 50, 200, 150));
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 2, 2, &area, out), 1);
    HT_CHECK(rect_eq(&out[0], 200, 100, 400, 300));
    const HleRect part = { 150, 0, 640, 100 };
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &part, out), 1);
    HT_CHECK(rect_eq(&out[0], 150, 50, 200, 100));
    /* outside the area: nothing drawn */
    const HleRect far = { 300, 300, 640, 480 };
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &far, out), 0);
}

/* Several inclusive windows: their union, not the box around them (which
 * drew between two windows apart) */
static void test_inclusive_union(void)
{
    const HleRect area = { 0, 0, 640, 480 };
    none();
    win(0, 0, 99, 0, 99);
    win(1, 500, 599, 300, 399);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &area, out), 2);
    HT_CHECK(rect_eq(&out[0], 0, 0, 100, 100));
    HT_CHECK(rect_eq(&out[1], 500, 300, 600, 400));
    /* two stacked windows as wide: one rectangle */
    none();
    win(0, 10, 19, 0, 9);
    win(3, 10, 19, 10, 29);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &area, out), 1);
    HT_CHECK(rect_eq(&out[0], 10, 0, 20, 30));
    /* overlapping (an L): disjoint pieces */
    none();
    win(0, 0, 99, 0, 19);
    win(1, 0, 19, 0, 99);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, false, 1, 1, &area, out), 2);
    HT_CHECK(rect_eq(&out[0], 0, 0, 100, 20));
    HT_CHECK(rect_eq(&out[1], 0, 20, 20, 100));
}

/* Exclusive: everything outside, as up to 4 rectangles around one window;
 * one over the whole area draws nothing */
static void test_exclusive(void)
{
    const HleRect area = { 0, 0, 640, 480 };
    none();
    win(0, 100, 199, 50, 149);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, true, 1, 1, &area, out), 4);
    HT_CHECK(rect_eq(&out[0], 0, 0, 640, 50));
    HT_CHECK(rect_eq(&out[1], 0, 50, 100, 150));
    HT_CHECK(rect_eq(&out[2], 200, 50, 640, 150));
    HT_CHECK(rect_eq(&out[3], 0, 150, 640, 480));
    check_exact(true, 1, 1, &area);
    win(1, 0, 639, 0, 479);
    HT_EQ_INT(hle_pb_window_clip(hz, vt, true, 1, 1, &area, out), 0);
}

static uint32_t g_seed = 12345;

static uint32_t rnd(uint32_t m)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) % m;
}

/* Random windows against the registers read pixel by pixel */
static void test_random(void)
{
    for (unsigned t = 0; t < 400; t++) {
        none();
        const unsigned nw = 1 + rnd(8);
        for (unsigned i = 0; i < nw; i++) {
            /* some empty (max below min), some windows set twice */
            const uint32_t w = rnd(8), x1 = rnd(40), x2 = rnd(70), y1 = rnd(30), y2 = rnd(60);
            win(w, x1, x2, y1, y2);
        }
        const uint32_t ax = 1 + rnd(2), ay = 1 + rnd(2), ex = rnd(2);
        const int32_t x1 = (int32_t)rnd(10), y1 = (int32_t)rnd(10), x2 = 40 + (int32_t)rnd(40);
        const HleRect area = { x1, y1, x2, 30 + (int32_t)rnd(30) };
        check_exact(ex, ax, ay, &area);
    }
    /* many pieces: 8 windows apart on both axes; 8 columns, each its own
     * height */
    const HleRect area = { 0, 0, 40, 40 };
    none();
    for (unsigned i = 0; i < 8; i++) {
        win(i, 2 + 4 * i, 2 + 4 * i, 2 + 4 * i, 2 + 4 * i);
    }
    check_exact(true, 1, 1, &area);
    check_exact(false, 1, 1, &area);
    for (unsigned i = 0; i < 8; i++) {
        win(i, 2 + 4 * i, 3 + 4 * i, i, 30 - i);
    }
    check_exact(true, 1, 1, &area);
    check_exact(false, 1, 1, &area);
}

int main(void)
{
    test_no_clip();
    test_inclusive_one();
    test_inclusive_union();
    test_exclusive();
    test_random();
    HT_DONE();
}
