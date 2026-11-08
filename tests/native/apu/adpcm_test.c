/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test reserved header bytes and step-index clamping for mono and stereo. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "native/dev/apu/vp/adpcm.h"

static int g_fail;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

/* Channel headers (PCM, index, reserved), then interleaved 4-byte chunks. */
static void block(uint8_t *b, int channels, uint8_t index, uint8_t reserved)
{
    for (int ch = 0; ch < channels; ch++) {
        b[ch * 4 + 0] = 0x34;
        b[ch * 4 + 1] = (uint8_t)(0x12 + ch);
        b[ch * 4 + 2] = index;
        b[ch * 4 + 3] = reserved;
    }
    for (int i = channels * 4; i < channels * 36; i++) {
        b[i] = (uint8_t)(i * 37 + 11);
    }
}

int main(void)
{
    uint8_t b[72];
    int16_t ref[130], out[130];
    for (int channels = 1; channels <= 2; channels++) {
        char what[96];
        block(b, channels, 40, 0);
        int n = adpcm_decode_block(ref, b, (size_t)channels * 36, channels);
        snprintf(what, sizeof(what), "%d ch: a block decodes 65 samples", channels);
        check(n == 65, what);

        block(b, channels, 40, 0x08);
        memset(out, 0, sizeof(out));
        n = adpcm_decode_block(out, b, (size_t)channels * 36, channels);
        snprintf(what, sizeof(what), "%d ch: reserved byte 0x08 decodes the same", channels);
        check(n == 65 && memcmp(out, ref, sizeof(int16_t) * 65 * channels) == 0, what);

        block(b, channels, 88, 0);
        adpcm_decode_block(ref, b, (size_t)channels * 36, channels);
        block(b, channels, 100, 0);
        n = adpcm_decode_block(out, b, (size_t)channels * 36, channels);
        snprintf(what, sizeof(what), "%d ch: step index 100 decodes as 88", channels);
        check(n == 65 && memcmp(out, ref, sizeof(int16_t) * 65 * channels) == 0, what);

        block(b, channels, 0, 0);
        adpcm_decode_block(ref, b, (size_t)channels * 36, channels);
        block(b, channels, 0xC8, 0);    /* negative as int8_t */
        n = adpcm_decode_block(out, b, (size_t)channels * 36, channels);
        snprintf(what, sizeof(what), "%d ch: step index 0xC8 decodes as 0", channels);
        check(n == 65 && memcmp(out, ref, sizeof(int16_t) * 65 * channels) == 0, what);
    }
    check(adpcm_decode_block(out, b, 3, 1) == 0, "a block shorter than its header: none");
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
