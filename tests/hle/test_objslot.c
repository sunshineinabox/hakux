/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hw/xbox/hle/pb/objslot.h"
#include "hle_test.h"

#define N 128

static HlePbSlot slots[N];

static void full_table(void)
{
    for (unsigned i = 0; i < N; i++) {
        slots[i] = (HlePbSlot){ .id = 100 + i, .bound = 1000 + i };
    }
}

/* A retired slot (id 0) in a full table, no depth target bound (ds 0): the
 * free slot is taken, not mistaken for the depth target and skipped. */
static void test_free_slot_first(void)
{
    full_table();
    slots[57].id = 0;
    slots[57].bound = 0;
    const uint32_t refs[] = { 100, 0, 101, 102 };      /* rt, ds (none), shown, made */
    HT_EQ_INT(hle_pb_evict_choice(slots, N, refs, 4), 57);
    HT_CHECK(!hle_pb_slot_held(&slots[57], refs, 4));
    /* before one bound longer ago, or never */
    slots[3].bound = 0;
    HT_EQ_INT(hle_pb_evict_choice(slots, N, refs, 4), 57);
}

/* No free slot: the one bound longest ago, held ones skipped. */
static void test_least_recently_bound(void)
{
    full_table();
    const uint32_t refs[] = { 100, 0, 101, 102, 103 };  /* rt, ds, shown, made, a z-read's source */
    HT_EQ_INT(hle_pb_evict_choice(slots, N, refs, 5), 4);
    slots[4].kept = true;                               /* a cube */
    HT_EQ_INT(hle_pb_evict_choice(slots, N, refs, 5), 5);
    HT_CHECK(hle_pb_slot_held(&slots[0], refs, 5));
    HT_CHECK(hle_pb_slot_held(&slots[4], refs, 5));
    HT_CHECK(!hle_pb_slot_held(&slots[5], refs, 5));
}

/* Every slot held: none. */
static void test_all_held(void)
{
    full_table();
    for (unsigned i = 0; i < N; i++) {
        slots[i].kept = true;
    }
    HT_EQ_INT(hle_pb_evict_choice(slots, N, NULL, 0), -1);
}

int main(void)
{
    HT_RUN(test_free_slot_first);
    HT_RUN(test_least_recently_bound);
    HT_RUN(test_all_held);
    HT_DONE();
}
