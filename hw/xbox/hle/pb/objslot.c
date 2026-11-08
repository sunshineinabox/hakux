/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX HLE GPU — push-buffer mode's surface object slots (objslot.h).
 */
#include "hw/xbox/hle/pb/objslot.h"

bool hle_pb_slot_held(const HlePbSlot *s, const uint32_t *refs, unsigned nrefs)
{
    if (!s->id) {
        return false;
    }
    if (s->kept) {
        return true;
    }
    for (unsigned i = 0; i < nrefs; i++) {
        if (refs[i] && refs[i] == s->id) {
            return true;
        }
    }
    return false;
}

int hle_pb_evict_choice(const HlePbSlot *s, unsigned n, const uint32_t *refs, unsigned nrefs)
{
    int best = -1;
    for (unsigned i = 0; i < n; i++) {
        if (!s[i].id) {
            return (int)i;
        }
        if (!hle_pb_slot_held(&s[i], refs, nrefs) && (best < 0 || s[i].bound < s[best].bound)) {
            best = (int)i;
        }
    }
    return best;
}
