/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — XBE patches from the frontend's cheats (RetroArch's
 * Cheats menu, a .cht entry whose code is "xbe-patch-v1 ..."): byte runs
 * over a disc XBE, applied at its next boot when its CRC-32 and size match
 * (mods.c, nc_mods_xbe_patches). Pure C.
 *
 *   xbe-patch-v1 CRC32:SIZE+OFFSET:BYTES+OFFSET:BYTES...
 *
 * CRC, size and offsets are eight hex digits; each run is 1 to 64 bytes as
 * hex pairs at a file offset (4 or more: the magic stays), up to 128 runs,
 * sorted and not overlapping, within the size.
 */
#ifndef NATIVE_CORE_XBE_PATCH_H
#define NATIVE_CORE_XBE_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_XBE_PATCH_TAG "xbe-patch-v1"
#define NC_XBE_PATCH_RUNS_MAX 128
#define NC_XBE_PATCH_RUN_MAX 64
#define NC_XBE_PATCH_FRONT_MAX 256      /* the frontend's cheat indices */

typedef struct NcXbePatchRun {
    uint32_t off;
    uint8_t len;
    uint8_t bytes[NC_XBE_PATCH_RUN_MAX];
} NcXbePatchRun;

typedef struct NcXbePatch {
    uint32_t crc, size;
    unsigned nruns;
    NcXbePatchRun runs[NC_XBE_PATCH_RUNS_MAX];
} NcXbePatch;

/* The code is an XBE patch (its tag first, after blanks), well formed or
 * not: it isn't a memory cheat. */
bool nc_xbe_patch_is(const char *code);
/* False, with why in err ("run 3: below offset 4"), when it isn't one. */
bool nc_xbe_patch_parse(const char *code, NcXbePatch *out, char *err, size_t errlen);

/* The frontend's patches (retro_cheat_set), by its cheat index, kept over a
 * restart until nc_xbe_patch_front_reset. Off, or on with the code; false,
 * and off, with why in err when the code isn't one. */
bool nc_xbe_patch_front_set(unsigned index, bool on, const char *code, char *err, size_t errlen);
void nc_xbe_patch_front_reset(void);
/* The patches on now: their codes' hash, whatever their indices (0: none). */
uint64_t nc_xbe_patch_front_hash(void);
/* For an XBE of crc and size: 1 with the one patch on for it in *out and
 * its index in *index; 0 when none is; -1 when more than one is (a
 * conflict: none applies), the first two in *index and *other. hash: the
 * set's (nc_xbe_patch_front_hash), taken with it. */
int nc_xbe_patch_front_match(uint32_t crc, uint32_t size, NcXbePatch *out, unsigned *index, unsigned *other,
                             uint64_t *hash);
/* The patches on, as "CRC32:SIZE" each (for the log): how many. */
unsigned nc_xbe_patch_front_list(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
