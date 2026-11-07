/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The frontend's XBE patches (native/core/xbe_patch.c): the xbe-patch-v1
 * code's form and its limits, and the set the boot picks one from. */
#include "hle_test.h"

#include "native/core/xbe_patch.h"

#include <stdio.h>

static NcXbePatch g_p;
static char g_err[96];

static bool parse(const char *code)
{
    g_err[0] = 0;
    return nc_xbe_patch_parse(code, &g_p, g_err, sizeof(g_err));
}

/* the issue's example, and its form's freedoms: blanks around, either case */
static void test_parse(void)
{
    HT_CHECK(parse("xbe-patch-v1 1A2B3C4D:00200000+00001234:9090+00004321:EB"));
    HT_EQ_U32(g_p.crc, 0x1A2B3C4D);
    HT_EQ_U32(g_p.size, 0x200000);
    HT_EQ_U32(g_p.nruns, 2);
    HT_EQ_U32(g_p.runs[0].off, 0x1234);
    HT_EQ_U32(g_p.runs[0].len, 2);
    HT_CHECK(g_p.runs[0].bytes[0] == 0x90 && g_p.runs[0].bytes[1] == 0x90);
    HT_EQ_U32(g_p.runs[1].off, 0x4321);
    HT_EQ_U32(g_p.runs[1].len, 1);
    HT_EQ_U32(g_p.runs[1].bytes[0], 0xEB);
    HT_CHECK(parse("  xbe-patch-v1 df0db6d8:00157000+0004e163:a0+0004E18E:A0 \n"));
    HT_EQ_U32(g_p.crc, 0xDF0DB6D8);
    HT_EQ_U32(g_p.nruns, 2);
    HT_EQ_U32(g_p.runs[0].bytes[0], 0xA0);
    /* adjacent runs, the last byte of the file, offset 4 */
    HT_CHECK(parse("xbe-patch-v1 00000000:00001000+00000004:01+00000005:02+00000FFF:03"));
    HT_EQ_U32(g_p.nruns, 3);
    HT_CHECK(nc_xbe_patch_is("xbe-patch-v1 junk"));
    HT_CHECK(nc_xbe_patch_is(" xbe-patch-v1"));
    HT_CHECK(!nc_xbe_patch_is("xbe-patch-v10 1:2"));
    HT_CHECK(!nc_xbe_patch_is("w8 0x100 1"));
    HT_CHECK(!nc_xbe_patch_is("0010E000 0000005A"));
}

static const char *refused(const char *code)
{
    return parse(code) ? "" : g_err;
}

/* what isn't one, and why */
static void test_refused(void)
{
    HT_EQ_STR(refused("xbe-patch-v2 1A2B3C4D:00200000+00001234:90"), "not an xbe-patch-v1 code");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4:00200000+00001234:90"), "no CRC32:SIZE (eight hex digits each)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D-00200000+00001234:90"), "no CRC32:SIZE (eight hex digits each)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000"), "no runs");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+1234:90"), "run 1: no OFFSET: (eight hex digits)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00001234:909"), "run 1: an odd hex digit");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00001234:"), "run 1: no bytes");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00000003:90"), "run 1: below offset 4 (the XBE's magic)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00000000:90"), "run 1: below offset 4 (the XBE's magic)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00001234:9090+00001235:90"),
              "run 2: not after run 1 (sorted, not overlapping)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00004321:EB+00001234:90"),
              "run 2: not after run 1 (sorted, not overlapping)");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00001000+00000FFF:9090"), "run 1: past the XBE's size");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00001234:90 w8 0x100 1"), "\"w8 0x100 1\" after run 1");
    HT_EQ_STR(refused("xbe-patch-v1 1A2B3C4D:00200000+00001234:90+"), "run 2: no OFFSET: (eight hex digits)");
    /* 64 bytes a run, 128 runs */
    char code[128 * 20 + 64];
    int at = snprintf(code, sizeof(code), "xbe-patch-v1 00000000:00100000+00001000:");
    for (int i = 0; i < 64; i++) {
        at += snprintf(code + at, sizeof(code) - at, "%02X", i);
    }
    HT_CHECK(parse(code));
    HT_EQ_U32(g_p.runs[0].len, 64);
    snprintf(code + at, sizeof(code) - at, "FF");
    HT_EQ_STR(refused(code), "run 1: more than 64 bytes");
    at = snprintf(code, sizeof(code), "xbe-patch-v1 00000000:00100000");
    for (int i = 0; i < 128; i++) {
        at += snprintf(code + at, sizeof(code) - at, "+%08X:90", 0x1000 + 2 * i);
    }
    HT_CHECK(parse(code));
    HT_EQ_U32(g_p.nruns, 128);
    snprintf(code + at, sizeof(code) - at, "+%08X:90", 0x2000);
    HT_EQ_STR(refused(code), "more than 128 runs");
}

#define A "xbe-patch-v1 DF0DB6D8:00157000+0004E163:A0"
#define A2 "xbe-patch-v1 DF0DB6D8:00157000+0004E18E:A0"
#define B "xbe-patch-v1 6D3D7B26:00222000+000AFDE1:A0"

/* the frontend's: one on for the XBE booting applies; two are a conflict;
 * another XBE's or a malformed one doesn't; the set's hash moves with it */
static void test_front(void)
{
    NcXbePatch p;
    unsigned i = 99, j = 99;
    uint64_t h = 1;
    char err[96], list[64];
    nc_xbe_patch_front_reset();
    HT_EQ_U32(nc_xbe_patch_front_hash(), 0);
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D8, 0x157000, &p, &i, &j, &h), 0);
    HT_CHECK(h == 0);
    HT_CHECK(nc_xbe_patch_front_set(2, true, A, err, sizeof(err)));
    HT_CHECK(nc_xbe_patch_front_set(5, true, B, err, sizeof(err)));
    const uint64_t two = nc_xbe_patch_front_hash();
    HT_CHECK(two != 0);
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D8, 0x157000, &p, &i, &j, &h), 1);
    HT_EQ_U32(i, 2);
    HT_CHECK(h == two);
    HT_EQ_U32(p.runs[0].off, 0x4E163);
    HT_EQ_INT(nc_xbe_patch_front_match(0x6D3D7B26, 0x222000, &p, &i, &j, NULL), 1);
    HT_EQ_U32(i, 5);
    /* the wrong revision: same CRC, other size; other CRC */
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D8, 0x158000, &p, &i, &j, NULL), 0);
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D9, 0x157000, &p, &i, &j, NULL), 0);
    HT_EQ_U32(nc_xbe_patch_front_list(list, sizeof(list)), 2);
    HT_EQ_STR(list, "DF0DB6D8:00157000, 6D3D7B26:00222000");
    /* a second patch for the same XBE: neither */
    HT_CHECK(nc_xbe_patch_front_set(7, true, A2, err, sizeof(err)));
    i = j = 99;
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D8, 0x157000, &p, &i, &j, NULL), -1);
    HT_EQ_U32(i, 2);
    HT_EQ_U32(j, 7);
    HT_CHECK(nc_xbe_patch_front_hash() != two);
    /* turned off: the set as before */
    HT_CHECK(nc_xbe_patch_front_set(7, false, NULL, NULL, 0));
    HT_CHECK(nc_xbe_patch_front_hash() == two);
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D8, 0x157000, &p, &i, &j, NULL), 1);
    /* a malformed code over one that was on: refused, and off */
    HT_CHECK(!nc_xbe_patch_front_set(2, true, "xbe-patch-v1 DF0DB6D8:00157000+00000000:A0", err, sizeof(err)));
    HT_EQ_STR(err, "run 1: below offset 4 (the XBE's magic)");
    HT_EQ_INT(nc_xbe_patch_front_match(0xDF0DB6D8, 0x157000, &p, &i, &j, NULL), 0);
    /* the same codes at other indices (RetroArch renumbers the cheats that
     * are on): the same set */
    HT_CHECK(nc_xbe_patch_front_set(2, true, A, err, sizeof(err)));
    HT_CHECK(nc_xbe_patch_front_hash() == two);
    nc_xbe_patch_front_set(2, false, NULL, NULL, 0);
    nc_xbe_patch_front_set(5, false, NULL, NULL, 0);
    nc_xbe_patch_front_set(0, true, A, err, sizeof(err));
    nc_xbe_patch_front_set(1, true, B, err, sizeof(err));
    HT_CHECK(nc_xbe_patch_front_hash() == two);
    nc_xbe_patch_front_set(1, false, NULL, NULL, 0);
    HT_CHECK(nc_xbe_patch_front_hash() != two);
    HT_CHECK(!nc_xbe_patch_front_set(256, true, A, err, sizeof(err)));
    nc_xbe_patch_front_reset();
    HT_EQ_U32(nc_xbe_patch_front_hash(), 0);
    HT_EQ_U32(nc_xbe_patch_front_list(list, sizeof(list)), 0);
    HT_EQ_STR(list, "");
}

int main(void)
{
    HT_RUN(test_parse);
    HT_RUN(test_refused);
    HT_RUN(test_front);
    HT_DONE();
}
