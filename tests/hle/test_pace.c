/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The frame pace's arithmetic (pb/pace.h): the swap the decoder waits for,
 * and how far it runs ahead of the GPU, across the ring's wrap. */
#include "hw/xbox/hle/pb/pace.h"
#include "hle_test.h"

#define N 8

static uint64_t ring[N];

/* swaps 1..n, swap k at timeline value 10 * k */
static void swapped(uint64_t n)
{
    for (uint64_t k = 1; k <= n; k++) {
        ring[k % N] = 10 * k;
    }
}

static void test_want(void)
{
    memset(ring, 0, sizeof(ring));
    swapped(2);
    HT_EQ_INT(hle_pace_want(ring, N, 2, 2), 0);      /* nothing two back yet */
    HT_EQ_INT(hle_pace_want(ring, N, 2, 1), 10);
    HT_EQ_INT(hle_pace_want(ring, N, 2, 0), 0);      /* off */
    swapped(21);                                      /* the ring went round */
    HT_EQ_INT(hle_pace_want(ring, N, 21, 2), 190);
    HT_EQ_INT(hle_pace_want(ring, N, 21, 3), 180);
    HT_EQ_INT(hle_pace_want(ring, N, 21, N), 0);      /* deeper than the ring */
}

static void test_ahead(void)
{
    memset(ring, 0, sizeof(ring));
    HT_EQ_INT(hle_pace_ahead(ring, N, 0, 0), 0);      /* no swap yet */
    swapped(3);
    HT_EQ_INT(hle_pace_ahead(ring, N, 3, 0), 3);
    HT_EQ_INT(hle_pace_ahead(ring, N, 3, 20), 1);
    HT_EQ_INT(hle_pace_ahead(ring, N, 3, 30), 0);
    swapped(50);
    HT_EQ_INT(hle_pace_ahead(ring, N, 50, 475), 3);   /* 48, 49, 50 */
    HT_EQ_INT(hle_pace_ahead(ring, N, 50, 0), N);     /* as far as the ring sees */
    HT_EQ_INT(hle_pace_ahead(ring, N, 50, UINT64_MAX), 0);   /* a lost GPU: all done */
}

int main(void)
{
    HT_RUN(test_want);
    HT_RUN(test_ahead);
    HT_DONE();
}
