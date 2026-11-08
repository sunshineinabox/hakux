/* Copyright (C) 2026-present PortareOS */
/*
 * hakuX HLE GPU — ETC2 / EAC block encoder (etc2_enc.h).
 *
 * ETC1's block: two halves (flip 0: the left and right 2x4 columns, flip 1:
 * the top and bottom 4x2 rows), each a base colour (4 bits a channel, or 5
 * bits and the other half's as a 3-bit delta) and a modifier table; every
 * texel one of the table's four modifiers over the base. A half's base is
 * its texels' mean; its table and modifiers the least squared error of the
 * eight. Both flips and both colour modes are tried (the delta mode is kept
 * valid: a delta past -4..3 would make an ETC2 T, H or planar block).
 *
 * Punch-through (RGB A1): the delta layout only, bit 33 the opaque bit; a
 * block with a transparent texel sets it off, which makes the small
 * modifiers zero and modifier index 2 transparent.
 *
 * EAC alpha: a base, a multiplier and one of 16 tables, every texel one of
 * the table's eight; the multiplier is fitted to the texels' range and the
 * base to its middle, the table by least squared error.
 */
#include "hw/xbox/hle/tex/etc2_enc.h"

#include <limits.h>
#include <string.h>

static const int k_tab[8][2] = {
    { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 },
};

/* a half's texels (row-major indices): [flip][half] */
static const uint8_t k_half[2][2][8] = {
    { { 0, 1, 4, 5, 8, 9, 12, 13 }, { 2, 3, 6, 7, 10, 11, 14, 15 } },
    { { 0, 1, 2, 3, 4, 5, 6, 7 }, { 8, 9, 10, 11, 12, 13, 14, 15 } },
};

static inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static inline int chan(uint32_t p, unsigned sh)
{
    return (int)((p >> sh) & 0xFF);
}

static inline bool transparent(uint32_t p)
{
    return (p >> 24) < 128;
}

/* The mean colour of half h (punch: of its opaque texels) */
static void half_mean(const uint32_t px[16], const uint8_t *h, bool punch, int m[3])
{
    int s[3] = { 0, 0, 0 }, n = 0;
    for (unsigned i = 0; i < 8; i++) {
        uint32_t p = px[h[i]];
        if (punch && transparent(p)) {
            continue;
        }
        s[0] += chan(p, 16);
        s[1] += chan(p, 8);
        s[2] += chan(p, 0);
        n++;
    }
    for (unsigned c = 0; c < 3; c++) {
        m[c] = n ? (s[c] + n / 2) / n : 0;
    }
}

/* Half h over base: its best table and the texels' modifier indices
 * (sel[i], 2 bits: msb, lsb); the squared error. One modifier serves a
 * texel's three channels, so its least-squares value is the mean of the
 * channel differences (d); the table is chosen on those alone (the
 * residual across channels doesn't depend on it), the error then counted
 * exactly, clamped. punch: the small modifiers are 0 and index 2 is
 * transparent. */
static uint32_t half_fit(const uint32_t px[16], const uint8_t *h, const int base[3], bool punch,
                         uint8_t *table, uint8_t sel[8])
{
    int d[8];
    bool tr[8];
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t p = px[h[i]];
        tr[i] = punch && transparent(p);
        d[i] = (chan(p, 16) - base[0] + chan(p, 8) - base[1] + chan(p, 0) - base[2]) / 3;
    }
    uint32_t best = UINT32_MAX;
    unsigned bt = 0;
    for (unsigned t = 0; t < 8; t++) {
        const int a = punch ? 0 : k_tab[t][0], b = k_tab[t][1];
        uint32_t err = 0;
        for (unsigned i = 0; i < 8; i++) {
            if (tr[i]) {
                continue;
            }
            /* the nearest of +a, +b, -a, -b (punch: 0, +b, -b) */
            const int v = d[i], m = v >= 0 ? (v - a < b - v ? a : b) : (v + a > -b - v ? -a : -b);
            const int e = v - m;
            err += (uint32_t)(e * e);
        }
        if (err < best) {
            best = err;
            bt = t;
        }
    }
    *table = (uint8_t)bt;
    const int a = punch ? 0 : k_tab[bt][0], b = k_tab[bt][1];
    uint32_t err = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (tr[i]) {
            sel[i] = 2;
            continue;
        }
        const int v = d[i];
        const int m = v >= 0 ? (v - a < b - v ? a : b) : (v + a > -b - v ? -a : -b);
        sel[i] = m == a ? 0 : m == b ? 1 : m == -a ? 2 : 3;
        if (punch && sel[i] == 2) {
            sel[i] = 0;                 /* a = 0: index 0 is the same modifier */
        }
        const uint32_t p = px[h[i]];
        const int dr = clamp255(base[0] + m) - chan(p, 16);
        const int dg = clamp255(base[1] + m) - chan(p, 8);
        const int db = clamp255(base[2] + m) - chan(p, 0);
        err += (uint32_t)(dr * dr + dg * dg + db * db);
    }
    return err;
}

static inline int q4(int v)         /* 8-bit -> 4-bit, rounded */
{
    return (v * 15 + 127) / 255;
}

static inline int q5(int v)         /* 8-bit -> 5-bit, rounded */
{
    return (v * 31 + 127) / 255;
}

static inline int x4(int v)         /* 4-bit -> 8-bit */
{
    return v * 17;
}

static inline int x5(int v)         /* 5-bit -> 8-bit */
{
    return (v << 3) | (v >> 2);
}

typedef struct Fit {
    uint32_t err;
    bool diff, flip;
    int c[2][3];                    /* the halves' quantized base colours (4 or 5 bits) */
    uint8_t table[2];
    uint8_t sel[2][8];
} Fit;

/* Both halves fitted in one colour mode and flip; true when diff and the
 * second half's delta had to be clamped (still a valid block, but the
 * individual mode may do better) */
static bool fit(const uint32_t px[16], bool diff, bool flip, bool punch, Fit *f)
{
    bool clamped = false;
    f->err = 0;
    f->diff = diff;
    f->flip = flip;
    int m[2][3];
    half_mean(px, k_half[flip][0], punch, m[0]);
    half_mean(px, k_half[flip][1], punch, m[1]);
    for (unsigned c = 0; c < 3; c++) {
        if (diff) {
            f->c[0][c] = q5(m[0][c]);
            int d = q5(m[1][c]) - f->c[0][c];
            const int cd = d < -4 ? -4 : d > 3 ? 3 : d;
            clamped |= cd != d;
            f->c[1][c] = f->c[0][c] + cd;
        } else {
            f->c[0][c] = q4(m[0][c]);
            f->c[1][c] = q4(m[1][c]);
        }
    }
    for (unsigned h = 0; h < 2; h++) {
        int base[3];
        for (unsigned c = 0; c < 3; c++) {
            base[c] = diff ? x5(f->c[h][c]) : x4(f->c[h][c]);
        }
        f->err += half_fit(px, k_half[flip][h], base, punch, &f->table[h], f->sel[h]);
    }
    return clamped;
}

/* bit 33: diff (RGB) or opaque (RGB A1) */
static void pack(const Fit *f, bool bit33, uint8_t out[8])
{
    uint64_t w = 0;
    if (f->diff) {
        w |= (uint64_t)f->c[0][0] << 59 | (uint64_t)((f->c[1][0] - f->c[0][0]) & 7) << 56;
        w |= (uint64_t)f->c[0][1] << 51 | (uint64_t)((f->c[1][1] - f->c[0][1]) & 7) << 48;
        w |= (uint64_t)f->c[0][2] << 43 | (uint64_t)((f->c[1][2] - f->c[0][2]) & 7) << 40;
    } else {
        w |= (uint64_t)f->c[0][0] << 60 | (uint64_t)f->c[1][0] << 56;
        w |= (uint64_t)f->c[0][1] << 52 | (uint64_t)f->c[1][1] << 48;
        w |= (uint64_t)f->c[0][2] << 44 | (uint64_t)f->c[1][2] << 40;
    }
    w |= (uint64_t)f->table[0] << 37 | (uint64_t)f->table[1] << 34;
    w |= (uint64_t)bit33 << 33 | (uint64_t)f->flip << 32;
    /* texel i = x * 4 + y: its index's msb at bit 16 + i, lsb at bit i */
    for (unsigned h = 0; h < 2; h++) {
        for (unsigned i = 0; i < 8; i++) {
            const unsigned rm = k_half[f->flip][h][i], x = rm & 3, y = rm >> 2, t = x * 4 + y;
            w |= (uint64_t)(f->sel[h][i] >> 1) << (16 + t) | (uint64_t)(f->sel[h][i] & 1) << t;
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        out[i] = (uint8_t)(w >> (56 - 8 * i));
    }
}

void hle_etc2_rgb_block(const uint32_t px[16], uint8_t out[8])
{
    Fit best, f;
    best.err = UINT32_MAX;
    for (unsigned flip = 0; flip < 2; flip++) {
        /* the delta mode (5-bit bases); the individual mode (4-bit) only
         * where the halves' means are too far apart for a delta */
        const bool clamped = fit(px, true, flip, false, &f);
        if (f.err < best.err) {
            best = f;
        }
        if (clamped) {
            fit(px, false, flip, false, &f);
            if (f.err < best.err) {
                best = f;
            }
        }
    }
    pack(&best, best.diff, out);
}

void hle_etc2_rgba1_block(const uint32_t px[16], uint8_t out[8])
{
    bool punch = false;
    for (unsigned i = 0; i < 16 && !punch; i++) {
        punch = transparent(px[i]);
    }
    Fit best, f;
    best.err = UINT32_MAX;
    for (unsigned flip = 0; flip < 2; flip++) {
        fit(px, true, flip, punch, &f);
        if (f.err < best.err) {
            best = f;
        }
    }
    pack(&best, !punch, out);
}

/* ---- EAC alpha ----------------------------------------------------------------- */

static const int k_atab[16][8] = {
    { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 },
    { -2, -5, -8, -13, 1, 4, 7, 12 }, { -2, -4, -6, -13, 1, 3, 5, 12 },
    { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 },
    { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },
    { -2, -6, -8, -10, 1, 5, 7, 9 },  { -2, -5, -8, -10, 1, 4, 7, 9 },
    { -2, -4, -8, -10, 1, 3, 7, 9 },  { -2, -5, -7, -10, 1, 4, 6, 9 },
    { -3, -4, -7, -10, 2, 3, 6, 9 },  { -1, -2, -3, -10, 0, 1, 2, 9 },
    { -4, -6, -8, -9, 3, 5, 7, 8 },   { -3, -5, -7, -9, 2, 4, 6, 8 },
};

static void pack_alpha(int base, int mult, int table, const uint8_t sel[16], uint8_t out[8])
{
    out[0] = (uint8_t)base;
    out[1] = (uint8_t)(mult << 4 | table);
    uint64_t w = 0;                 /* texel i = x * 4 + y: 3 bits at 45 - 3 i */
    for (unsigned x = 0; x < 4; x++) {
        for (unsigned y = 0; y < 4; y++) {
            w |= (uint64_t)sel[y * 4 + x] << (45 - 3 * (x * 4 + y));
        }
    }
    for (unsigned i = 0; i < 6; i++) {
        out[2 + i] = (uint8_t)(w >> (40 - 8 * i));
    }
}

void hle_eac_alpha_block(const uint32_t px[16], uint8_t out[8])
{
    int a[16], lo = 255, hi = 0;
    for (unsigned i = 0; i < 16; i++) {
        a[i] = chan(px[i], 24);
        lo = a[i] < lo ? a[i] : lo;
        hi = a[i] > hi ? a[i] : hi;
    }
    if (lo == hi) {
        /* table 13's modifier 4 is 0: the base exactly, whatever the multiplier */
        static const uint8_t four[16] = { 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 };
        pack_alpha(lo, 1, 13, four, out);
        return;
    }
    uint32_t best = UINT32_MAX;
    int bbase = 0, bmult = 1, btab = 0;
    uint8_t bsel[16] = { 0 };
    for (unsigned t = 0; t < 16; t++) {
        const int *tab = k_atab[t];
        const int span = tab[7] - tab[3];
        const int m0 = ((hi - lo) + span / 2) / span;
        {
            const int m = m0 < 1 ? 1 : m0 > 15 ? 15 : m0;
            int base = (lo + hi - m * (tab[7] + tab[3])) / 2;
            base = clamp255(base);
            int v[8];
            for (unsigned k = 0; k < 8; k++) {
                v[k] = clamp255(base + m * tab[k]);
            }
            uint32_t err = 0;
            uint8_t sel[16];
            for (unsigned i = 0; i < 16 && err < best; i++) {
                uint32_t pe = UINT32_MAX;
                for (unsigned k = 0; k < 8; k++) {
                    const int d = v[k] - a[i];
                    const uint32_t e = (uint32_t)(d * d);
                    if (e < pe) {
                        pe = e;
                        sel[i] = (uint8_t)k;
                    }
                }
                err += pe;
            }
            if (err < best) {
                best = err;
                bbase = base;
                bmult = m;
                btab = (int)t;
                memcpy(bsel, sel, 16);
            }
        }
    }
    pack_alpha(bbase, bmult, btab, bsel, out);
}
