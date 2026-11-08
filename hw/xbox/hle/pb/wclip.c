/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — push-buffer mode's window clip (wclip.h).
 */
#include "hw/xbox/hle/pb/wclip.h"

/* v into the sorted edges e[0..n), once; the new count */
static unsigned edge_add(int32_t *e, unsigned n, int32_t v)
{
    unsigned i = 0;
    while (i < n && e[i] < v) {
        i++;
    }
    if (i < n && e[i] == v) {
        return n;
    }
    memmove(&e[i + 1], &e[i], (n - i) * sizeof(*e));
    e[i] = v;
    return n + 1;
}

/* The area swept in bands between the rectangles' top and bottom edges: in
 * each, the runs of x the clip lets through, a run continuing one of the
 * band above (same x) making that rectangle taller. An inclusive clip of one
 * rectangle is one; an exclusive one up to 4 (above, left, right, below). */
int hle_pb_window_clip(const uint32_t *horiz, const uint32_t *vert, bool exclusive, uint32_t ax,
                       uint32_t ay, const HleRect *area, HleRect *out)
{
    HleRect r[8];
    unsigned nr = 0;
    bool used = false;
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t x = horiz[i], y = vert[i];
        if (!x && !y) {
            continue;
        }
        used = true;
        const HleRect c = {         /* its part inside the area */
            MAX((int32_t)((x & 0xFFF) * ax), area->x1), MAX((int32_t)((y & 0xFFF) * ay), area->y1),
            MIN((int32_t)((((x >> 16) & 0xFFF) + 1) * ax), area->x2),
            MIN((int32_t)((((y >> 16) & 0xFFF) + 1) * ay), area->y2) };
        if (c.x1 < c.x2 && c.y1 < c.y2) {
            r[nr++] = c;
        }
    }
    if (!used || area->x1 >= area->x2 || area->y1 >= area->y2) {
        return -1;
    }
    int32_t ye[18];
    unsigned ny = edge_add(ye, edge_add(ye, 0, area->y1), area->y2);
    for (unsigned i = 0; i < nr; i++) {
        ny = edge_add(ye, edge_add(ye, ny, r[i].y1), r[i].y2);
    }
    unsigned n = 0, prev[9], nprev = 0;     /* the band above's rectangles */
    for (unsigned b = 0; b + 1 < ny; b++) {
        const int32_t y1 = ye[b], y2 = ye[b + 1];
        int32_t xe[18], run[9][2];
        unsigned nx = edge_add(xe, edge_add(xe, 0, area->x1), area->x2), nrun = 0;
        for (unsigned i = 0; i < nr; i++) {
            if (r[i].y1 <= y1 && r[i].y2 >= y2) {
                nx = edge_add(xe, edge_add(xe, nx, r[i].x1), r[i].x2);
            }
        }
        for (unsigned k = 0; k + 1 < nx; k++) {
            bool covered = false;
            for (unsigned i = 0; i < nr && !covered; i++) {
                covered = r[i].y1 <= y1 && r[i].y2 >= y2 && r[i].x1 <= xe[k] && r[i].x2 >= xe[k + 1];
            }
            if (covered == exclusive) {
                continue;
            }
            if (nrun && run[nrun - 1][1] == xe[k]) {
                run[nrun - 1][1] = xe[k + 1];
            } else {
                run[nrun][0] = xe[k];
                run[nrun++][1] = xe[k + 1];
            }
        }
        unsigned cur[9];
        for (unsigned j = 0; j < nrun; j++) {
            unsigned p = 0;
            while (p < nprev && (out[prev[p]].x1 != run[j][0] || out[prev[p]].x2 != run[j][1])) {
                p++;
            }
            if (p < nprev) {
                out[prev[p]].y2 = y2;
                cur[j] = prev[p];
            } else {
                out[n] = (HleRect){ run[j][0], y1, run[j][1], y2 };
                cur[j] = n++;
            }
        }
        memcpy(prev, cur, nrun * sizeof(*cur));
        nprev = nrun;
    }
    if (n == 1 && out[0].x1 == area->x1 && out[0].y1 == area->y1 && out[0].x2 == area->x2 &&
        out[0].y2 == area->y2) {
        return -1;
    }
    return (int)n;
}
