/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — push-buffer mode's window clip (SET_WINDOW_CLIP_*) as
 * disjoint rectangles: the back end records a draw once in each, scissored
 * to it (vk/draw.c). Pure: the decoder passes the registers.
 */
#ifndef HW_XBOX_HLE_PB_WCLIP_H
#define HW_XBOX_HLE_PB_WCLIP_H

#include "hw/xbox/hle/front/cmd.h"

/* The part of area (render-target pixels, [x1, x2) x [y1, y2): the surface
 * clip's) the window clip lets the NV2A draw into, as disjoint rectangles in
 * out (HLE_CMD_MAX_CLIP of them at most, which no clip needs more than).
 * horiz and vert: SET_WINDOW_CLIP_HORIZONTAL and _VERTICAL's 8 words (min
 * 11:0, max 27:16, both included, in logical pixels: scaled by the
 * antialiasing's ax x ay). Inclusive: inside the union of the rectangles;
 * exclusive: outside all of them. A rectangle with both words 0 is unused
 * (D3D's SetScissors leaves those after the ones it sets so). Returns how
 * many; 0: nothing is drawn; -1: no clip (no rectangle used, or the clip
 * leaves the whole area). */
int hle_pb_window_clip(const uint32_t *horiz, const uint32_t *vert, bool exclusive, uint32_t ax,
                       uint32_t ay, const HleRect *area, HleRect *out);

#endif
