/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fill and smoothing state from the Xbox render registers.
 * No Vulkan dependency; see tests/hle/test_fill.c. */
#ifndef HW_XBOX_HLE_VK_FILL_H
#define HW_XBOX_HLE_VK_FILL_H

#include <stdbool.h>
#include <stdint.h>

enum { HLE_FILL_FILL = 0, HLE_FILL_LINE = 1, HLE_FILL_POINT = 2 };     /* VkPolygonMode */
enum { HLE_CULL_NONE = 0, HLE_CULL_FRONT = 1, HLE_CULL_BACK = 2 };     /* VkCullModeFlagBits */
/* Faces selected for this draw pass. */
enum { HLE_FACES_BOTH, HLE_FACES_FRONT, HLE_FACES_BACK };
/* Primitive topology grouped by rasterization behavior. */
enum { HLE_TOPO_POINTS, HLE_TOPO_LINES, HLE_TOPO_TRIS };

/* Translate Xbox polygon-mode values; unknown values default to fill. */
static inline unsigned hle_fill_mode(uint32_t v)
{
    return v == 0x1B00 ? HLE_FILL_POINT : v == 0x1B01 ? HLE_FILL_LINE : HLE_FILL_FILL;
}

/* Different front/back modes require separate passes when both faces render.
 * Front faces are drawn first, which can change primitive overlap order. */
static inline bool hle_fill_split(unsigned topo, uint32_t front, uint32_t back, unsigned cull)
{
    return topo == HLE_TOPO_TRIS && cull == HLE_CULL_NONE &&
           hle_fill_mode(front) != hle_fill_mode(back);
}

typedef struct HleFill {
    uint8_t mode;               /* Polygon mode (fill for line/point primitives). */
    bool lines;                 /* Apply line width and smoothing. */
    bool points;                /* Apply point size and sprites. */
    bool lsmooth, psmooth;      /* Enabled line and polygon smoothing. */
    uint8_t offset;             /* Polygon-offset mode: fill, line, or point. */
} HleFill;

/* Choose rasterization for one pass. Line/point primitives ignore polygon
 * modes. Without fillModeNonSolid, triangles use solid fill.
 * Smoothing requires multisampling off (aactl bit 0 clear).
 * edgeaa bit 0 smooths lines; bit 1 smooths filled polygons. */
static inline HleFill hle_fill(unsigned topo, uint32_t front, uint32_t back, unsigned cull,
                               unsigned faces, uint32_t edgeaa, uint32_t aactl, bool nonsolid)
{
    HleFill f = { HLE_FILL_FILL, topo == HLE_TOPO_LINES, topo == HLE_TOPO_POINTS,
                  false, false, 0 };
    if (topo == HLE_TOPO_TRIS && nonsolid) {
        bool b = faces == HLE_FACES_BACK || (faces == HLE_FACES_BOTH && cull == HLE_CULL_FRONT);
        f.mode = (uint8_t)hle_fill_mode(b ? back : front);
        f.lines = f.mode == HLE_FILL_LINE;
        f.points = f.mode == HLE_FILL_POINT;
        f.offset = f.mode;
    }
    uint32_t smooth = aactl & 1 ? 0 : edgeaa & 3;
    f.lsmooth = f.lines && (smooth & 1);
    f.psmooth = topo == HLE_TOPO_TRIS && f.mode == HLE_FILL_FILL && (smooth & 2);
    return f;
}

#endif
