/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — push-buffer mode's surface objects: which one a new one
 * replaces once the table is full (pb/pbdraw.c evict_slot), and which are
 * held. Pure: the decoder passes a view of its slots and the ids it holds.
 */
#ifndef HW_XBOX_HLE_PB_OBJSLOT_H
#define HW_XBOX_HLE_PB_OBJSLOT_H

#include "hw/xbox/hle/hle_pure.h"

typedef struct HlePbSlot {
    uint32_t id;                /* 0: a free slot (an object retired) */
    uint64_t bound;             /* the bind count when last bound, 0 never */
    bool kept;                  /* a cube or a view into one: never replaced */
} HlePbSlot;

/* Whether slot s is held: kept, or named by refs (the bound targets, the
 * one shown, the one just made, the z-reads' sources). A 0 in refs names
 * nothing (no depth target bound), and a free slot is never held. */
bool hle_pb_slot_held(const HlePbSlot *s, const uint32_t *refs, unsigned nrefs);
/* The slot a new object takes once all n are used: a free one, else the one
 * bound longest ago that isn't held; -1 if every one is. */
int hle_pb_evict_choice(const HlePbSlot *s, unsigned n, const uint32_t *refs, unsigned nrefs);

#endif
