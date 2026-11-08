/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Xbox fill and smoothing rules, based on nxdk_pgraph_tests captures. */
#include "hw/xbox/hle/vk/fill.h"
#include "hle_test.h"

#define PT 0x1B00u
#define LN 0x1B01u
#define FL 0x1B02u
#define MS_ON  0xFFFF0001u              /* D3D's antialiasing control: multisampling on */
#define MS_OFF 0xFFFF0000u

static void test_modes(void)
{
    HT_EQ_INT(hle_fill_mode(PT), HLE_FILL_POINT);
    HT_EQ_INT(hle_fill_mode(LN), HLE_FILL_LINE);
    HT_EQ_INT(hle_fill_mode(FL), HLE_FILL_FILL);
    HT_EQ_INT(hle_fill_mode(0), HLE_FILL_FILL);         /* never written */
    HT_EQ_INT(hle_fill_mode(0x1B03), HLE_FILL_FILL);
}

/* Triangle wireframe uses line rasterization and its offset mode. */
static void test_tris(void)
{
    HleFill f = hle_fill(HLE_TOPO_TRIS, LN, LN, HLE_CULL_BACK, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_EQ_INT(f.mode, HLE_FILL_LINE);
    HT_CHECK(f.lines && !f.points);
    HT_EQ_INT(f.offset, 1);
    f = hle_fill(HLE_TOPO_TRIS, PT, PT, HLE_CULL_NONE, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_EQ_INT(f.mode, HLE_FILL_POINT);
    HT_CHECK(f.points && !f.lines);
    HT_EQ_INT(f.offset, 2);
    f = hle_fill(HLE_TOPO_TRIS, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_CHECK(f.mode == HLE_FILL_FILL && !f.lines && !f.points && f.offset == 0);
    /* Fall back to solid fill when non-solid modes are unavailable. */
    f = hle_fill(HLE_TOPO_TRIS, LN, LN, HLE_CULL_NONE, HLE_FACES_BOTH, 0, MS_ON, false);
    HT_CHECK(f.mode == HLE_FILL_FILL && !f.lines && f.offset == 0);
}

/* Polygon fill mode does not change line and point primitives. */
static void test_prims(void)
{
    HleFill f = hle_fill(HLE_TOPO_LINES, PT, PT, HLE_CULL_NONE, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_CHECK(f.mode == HLE_FILL_FILL && f.lines && !f.points);
    f = hle_fill(HLE_TOPO_POINTS, LN, LN, HLE_CULL_NONE, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_CHECK(f.mode == HLE_FILL_FILL && f.points && !f.lines);
    HT_CHECK(!hle_fill_split(HLE_TOPO_LINES, FL, LN, HLE_CULL_NONE));
}

/* Different front/back modes may require separate passes. */
static void test_faces(void)
{
    HT_CHECK(hle_fill_split(HLE_TOPO_TRIS, FL, LN, HLE_CULL_NONE));
    HT_CHECK(!hle_fill_split(HLE_TOPO_TRIS, FL, LN, HLE_CULL_BACK));
    HT_CHECK(!hle_fill_split(HLE_TOPO_TRIS, LN, LN, HLE_CULL_NONE));
    HT_CHECK(!hle_fill_split(HLE_TOPO_TRIS, FL, 0, HLE_CULL_NONE));     /* both filled */
    HleFill f = hle_fill(HLE_TOPO_TRIS, FL, LN, HLE_CULL_NONE, HLE_FACES_FRONT, 0, MS_ON, true);
    HT_EQ_INT(f.mode, HLE_FILL_FILL);
    f = hle_fill(HLE_TOPO_TRIS, FL, LN, HLE_CULL_NONE, HLE_FACES_BACK, 0, MS_ON, true);
    HT_EQ_INT(f.mode, HLE_FILL_LINE);
    /* With front faces culled, use the back-face mode. */
    f = hle_fill(HLE_TOPO_TRIS, FL, PT, HLE_CULL_FRONT, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_EQ_INT(f.mode, HLE_FILL_POINT);
    f = hle_fill(HLE_TOPO_TRIS, FL, PT, HLE_CULL_BACK, HLE_FACES_BOTH, 0, MS_ON, true);
    HT_EQ_INT(f.mode, HLE_FILL_FILL);
}

/* Smoothing applies only without multisampling: bit 0 for lines,
 * bit 1 for filled triangles. */
static void test_smooth(void)
{
    HleFill f = hle_fill(HLE_TOPO_LINES, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 3, MS_ON, true);
    HT_CHECK(!f.lsmooth && !f.psmooth);
    f = hle_fill(HLE_TOPO_LINES, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 1, MS_OFF, true);
    HT_CHECK(f.lsmooth && !f.psmooth);
    f = hle_fill(HLE_TOPO_LINES, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 2, MS_OFF, true);
    HT_CHECK(!f.lsmooth && !f.psmooth);
    f = hle_fill(HLE_TOPO_TRIS, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 2, MS_OFF, true);
    HT_CHECK(!f.lsmooth && f.psmooth);
    f = hle_fill(HLE_TOPO_TRIS, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 1, MS_OFF, true);
    HT_CHECK(!f.lsmooth && !f.psmooth);
    f = hle_fill(HLE_TOPO_TRIS, LN, LN, HLE_CULL_NONE, HLE_FACES_BOTH, 3, MS_OFF, true);
    HT_CHECK(f.lsmooth && !f.psmooth);
    f = hle_fill(HLE_TOPO_TRIS, PT, PT, HLE_CULL_NONE, HLE_FACES_BOTH, 3, MS_OFF, true);
    HT_CHECK(!f.lsmooth && !f.psmooth);
    f = hle_fill(HLE_TOPO_POINTS, FL, FL, HLE_CULL_NONE, HLE_FACES_BOTH, 3, MS_OFF, true);
    HT_CHECK(!f.lsmooth && !f.psmooth);
}

int main(void)
{
    HT_RUN(test_modes);
    HT_RUN(test_tris);
    HT_RUN(test_prims);
    HT_RUN(test_faces);
    HT_RUN(test_smooth);
    HT_DONE();
}
