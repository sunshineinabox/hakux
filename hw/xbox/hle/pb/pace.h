/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — the frame pace's arithmetic (pb/pbdraw.c g_pace), pure
 * for the host tests. ring[k % n] holds the timeline value of swap k (from
 * 1; swaps is the newest), for the last n swaps.
 */
#ifndef HW_XBOX_HLE_PB_PACE_H
#define HW_XBOX_HLE_PB_PACE_H

#include <stdint.h>

/* The value of the swap depth before the newest, the one the decoder waits
 * for; 0 (no wait) when there is none yet. depth < n. */
static inline uint64_t hle_pace_want(const uint64_t *ring, unsigned n, uint64_t swaps, unsigned depth)
{
    return depth && depth < n && swaps > depth ? ring[(swaps - depth) % n] : 0;
}

/* How many of the newest swaps the GPU has not finished (done: its
 * timeline), the newest included: how far decoding runs ahead of the
 * GPU. At most n. */
static inline unsigned hle_pace_ahead(const uint64_t *ring, unsigned n, uint64_t swaps, uint64_t done)
{
    unsigned ahead = 0;
    for (uint64_t k = swaps; k >= 1 && ahead < n && ring[k % n] > done; k--) {
        ahead++;
    }
    return ahead;
}

#endif
