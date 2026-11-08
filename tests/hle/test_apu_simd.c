/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The VP's vector paths against scalar models of their arithmetic, bit for
 * bit: the mix (vp/mix_inline.h) and the HRTF filter's dot product
 * (vp/hrtf.h), NEON's and AVX2's fused multiply-adds against fmaf in NEON's
 * lanes and order, SSE2's multiplies and adds against the scalar forms; the
 * monitor's float to 16-bit conversion and the linear resampler's stereo
 * path (vp/resample.c) against the scalar code. On x86-64 at each level the
 * host has (vp/simd.h). `test_apu_simd bench`: each path's time by level
 * (configured with -DCMAKE_BUILD_TYPE=Release). */
#include "hle_test.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "native/dev/apu/vp/hrtf.h"
#include "native/dev/apu/vp/mix_inline.h"
#include "native/dev/apu/vp/resample.h"

static uint32_t g_rng = 2026;

static float rnd(void)   /* [-1, 1) */
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(int32_t)g_rng / 2147483648.0f;
}

/* the levels to run: x86-64's the host has, highest first; elsewhere one */
static int levels(int *out)
{
    int n = 0;
#if defined(__x86_64__)
    for (int l = vp_simd_set(VP_SIMD_AVX2); l >= VP_SIMD_SCALAR; l--) {
        out[n++] = l;
    }
#else
    out[n++] = 0;
#endif
    return n;
}

static void set_level(int level)
{
#if defined(__x86_64__)
    vp_simd_set(level);
#else
    (void)level;
#endif
}

/* back to the host's highest */
static void reset_level(void)
{
#if defined(__x86_64__)
    vp_simd_set(VP_SIMD_AVX2);
#endif
}

static const char *level_name(int level)
{
#if defined(__x86_64__)
    return vp_simd_name(level);
#else
    (void)level;
    return "NEON";
#endif
}

/* fused: the path rounds a multiply-add once (NEON, AVX2 + FMA) */
static bool fused(int level)
{
#if defined(__x86_64__)
#if defined(__FMA__)
    return level != VP_SIMD_SSE2;   /* the compiler may fuse the scalar form */
#else
    return level == VP_SIMD_AVX2;
#endif
#else
    (void)level;
    return true;
#endif
}

/* the path sums as NEON does (NEON itself; AVX2 + FMA) */
static bool neon_sums(int level)
{
#if defined(__x86_64__)
    return level == VP_SIMD_AVX2;
#elif defined(__aarch64__)
    (void)level;
    return true;
#else
    (void)level;
    return false;
#endif
}

/* ---- the mix */
static void test_mix(void)
{
    int ls[3];
    const int nl = levels(ls);
    for (int li = 0; li < nl; li++) {
        set_level(ls[li]);
        unsigned off = 0;
        for (int trial = 0; trial < 2000; trial++) {
            float src[NUM_SAMPLES_PER_FRAME], dst[NUM_SAMPLES_PER_FRAME], want[NUM_SAMPLES_PER_FRAME];
            const float g = rnd() * 2.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                src[i] = rnd() * (i & 1 ? 1.0f : 1e-3f);
                dst[i] = rnd() * 4.0f;
                if (fused(ls[li])) {
                    want[i] = fmaf(g, src[i], dst[i]);
                } else {
                    const float p = g * src[i];
                    want[i] = dst[i] + p;
                }
            }
            mix_one_bin(dst, src, g);
            off += memcmp(dst, want, sizeof(dst)) != 0;
        }
        fprintf(stderr, "  mix %-8s %s: %u of 2000 frames off\n", level_name(ls[li]),
                fused(ls[li]) ? "fused" : "multiply, add", off);
        HT_EQ_INT(off, 0);
    }
    reset_level();
}

/* Check that ramped gains round like constant gains on each SIMD path. */
static void test_mix_ramp(void)
{
    int ls[3];
    const int nl = levels(ls);
    for (int li = 0; li < nl; li++) {
        set_level(ls[li]);
        unsigned off = 0;
        for (int trial = 0; trial < 2000; trial++) {
            float src[NUM_SAMPLES_PER_FRAME], dst[NUM_SAMPLES_PER_FRAME], want[NUM_SAMPLES_PER_FRAME];
            float g[NUM_SAMPLES_PER_FRAME];
            const float from = rnd() * 2.0f, to = rnd() * 2.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                g[i] = from + (to - from) * (i / 32.0f);
                src[i] = rnd() * (i & 1 ? 1.0f : 1e-3f);
                dst[i] = rnd() * 4.0f;
                if (fused(ls[li])) {
                    want[i] = fmaf(g[i], src[i], dst[i]);
                } else {
                    const float p = g[i] * src[i];
                    want[i] = dst[i] + p;
                }
            }
            mix_one_bin_ramp(dst, src, g);
            off += memcmp(dst, want, sizeof(dst)) != 0;
        }
        fprintf(stderr, "  ramp %-8s %s: %u of 2000 frames off\n", level_name(ls[li]),
                fused(ls[li]) ? "fused" : "multiply, add", off);
        HT_EQ_INT(off, 0);
    }
    reset_level();
}

/* ---- the HRTF filter's dot product */

/* NEON's hrtf_dot in scalar code: two 4-lane accumulators, each fused
 * multiply-add one rounding, summed (a0 + a1), then (0 + 1) + (2 + 3) */
static float neon_dot(const float *c, const float *tar, float wgt, const float *w, float dfrac)
{
    const bool interp = dfrac > 0.0f;
    const float w0 = 1 - dfrac;
    float a[8] = {0};
    for (int m = 0; m < HRTF_TAPS_PADDED; m += 8) {
        for (int j = 0; j < 8; j++) {
            float cj = c[m + j];
            if (tar) {
                cj = fmaf(tar[m + j] - cj, wgt, cj);
            }
            float s = w[m + j];
            if (interp) {
                const float p = w[m + j - 1] * dfrac;
                s = fmaf(s, w0, p);
            }
            a[j] = fmaf(cj, s, a[j]);
        }
    }
    float t[4];
    for (int j = 0; j < 4; j++) {
        t[j] = a[j] + a[j + 4];
    }
    const float lo = t[0] + t[1], hi = t[2] + t[3];
    return lo + hi;
}

#if defined(__x86_64__)
static VP_AVX2 float dot_avx2(const float *c, const float *tar, float wgt, const float *w, float dfrac)
{
    return hrtf_dot_avx2(c, tar, wgt, w, dfrac);
}
#endif

/* the build's NEON path or x86-64's AVX2 one against the model: the same
 * sums, the coefficients moved alike; SSE2's within rounding */
static void test_hrtf_neon_sums(void)
{
    int ls[3];
    const int nl = levels(ls);
    for (int li = 0; li < nl; li++) {
        const int level = ls[li];
        unsigned off = 0;
        double max_err = 0;
        for (int trial = 0; trial < 4000; trial++) {
            float c[HRTF_TAPS_PADDED], c2[HRTF_TAPS_PADDED], tar[HRTF_TAPS_PADDED], w[HRTF_TAPS_PADDED + 1];
            for (int m = 0; m < HRTF_TAPS_PADDED; m++) {
                c[m] = rnd() * (m < 8 ? 1.0f : 0.25f);
                tar[m] = rnd();
            }
            c[HRTF_TAPS_PADDED - 1] = tar[HRTF_TAPS_PADDED - 1] = 0.0f;
            for (int m = 0; m <= HRTF_TAPS_PADDED; m++) {
                w[m] = rnd();
            }
            memcpy(c2, c, sizeof(c));
            const bool moving = trial & 1;
            const float dfrac = trial & 2 ? 0.0f : (trial % 97) / 97.0f;
            const float wgt = (trial % 32) / 32.0f;
            const float want = neon_dot(c, moving ? tar : NULL, wgt, w + 1, dfrac);
            float got;
#if defined(__x86_64__)
            if (level == VP_SIMD_AVX2) {
                got = dot_avx2(c2, moving ? tar : NULL, wgt, w + 1, dfrac);
            } else if (level == VP_SIMD_SSE2) {
                got = hrtf_dot_sse2(c2, moving ? tar : NULL, wgt, w + 1, dfrac);
            } else {
                got = hrtf_dot(c2, moving ? tar : NULL, wgt, w + 1, dfrac);
            }
#else
            got = hrtf_dot(c2, moving ? tar : NULL, wgt, w + 1, dfrac);
#endif
            off += memcmp(&got, &want, sizeof(got)) != 0;
            HT_CHECK(memcmp(c, c2, sizeof(c)) == 0);    /* the coefficients aren't written */
            max_err = fmax(max_err, fabs((double)got - want));
        }
        fprintf(stderr, "  hrtf %-8s %u of 4000 sums off NEON's, max |difference| %.3g\n",
                level_name(level), off, max_err);
        if (neon_sums(level)) {
            HT_EQ_INT(off, 0);
        }
        HT_CHECK(max_err < 2e-6);
    }
    reset_level();
}

/* ---- the monitor's conversion */
static short to_short(float v)
{
    if (v != v) {
        return 0;
    }
    v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    return (short)(v * 32767.0f);
}

static void test_float_to_short(void)
{
    enum { N = 70 };    /* the vector loops and their tails */
    float in[N];
    short want[N], got[N];
    int ls[3];
    const int nl = levels(ls);
    for (int li = 0; li < nl; li++) {
        set_level(ls[li]);
        unsigned off = 0;
        for (int trial = 0; trial < 500; trial++) {
            for (int i = 0; i < N; i++) {
                in[i] = rnd() * 1.5f;
            }
            /* the edges, in the vector part (NaN's conversion is undefined C) */
            static const float edge[] = {1.0f, -1.0f, 0.0f, -0.0f, 0.99999994f, -0.99999994f,
                                         1.0000001f, INFINITY, -INFINITY, 1e-30f, 3.0517578e-05f};
            in[trial % 60] = edge[trial % (sizeof(edge) / sizeof(edge[0]))];
            in[(trial * 7) % 60] = NAN;
            for (int i = 0; i < N; i++) {
                want[i] = to_short(in[i]);
            }
            vp_float_to_short_array(in, got, N);
            off += memcmp(got, want, sizeof(got)) != 0;
        }
        fprintf(stderr, "  float to 16-bit %-8s %u of 500 blocks off\n", level_name(ls[li]), off);
        HT_EQ_INT(off, 0);
    }
    reset_level();
}

/* ---- the linear resampler: its stereo path against each channel alone
 * (the scalar loop), and on x86-64 against the scalar level */
typedef struct {
    int channels, ch;
    long n;
    float buf[64];
} Feed;

static long feed(void *p, float **data)
{
    Feed *f = p;
    g_rng = 77 + (uint32_t)f->n;    /* the same stream for every reader */
    for (int i = 0; i < 32; i++) {
        const float l = rnd(), r = rnd();
        if (f->channels == 2) {
            f->buf[2 * i] = l;
            f->buf[2 * i + 1] = r;
        } else {
            f->buf[i] = f->ch ? r : l;
        }
    }
    f->n += 32;
    *data = f->buf;
    return 32;
}

static void linear(int channels, int ch, double ratio, float *out, long frames)
{
    Feed f = {.channels = channels, .ch = ch};
    VpResampler *r = vp_resample_new(feed, VP_RESAMPLE_LINEAR, channels, &f);
    for (long done = 0; done < frames; done += 32) {
        HT_CHECK(vp_resample_read(r, ratio, 32, out + done * channels) == 32);
    }
    vp_resample_delete(r);
}

static void test_linear_stereo(void)
{
    enum { FRAMES = 4096 };
    static float st[2 * FRAMES], mono[2][FRAMES], ref[2 * FRAMES];
    static const double ratios[] = {48000.0 / 22050, 48000.0 / 32000, 1.0, 48000.0 / 44100, 0.37, 3.7};
    int ls[3];
    const int nl = levels(ls);
    unsigned off = 0, levels_off = 0;
    for (size_t k = 0; k < sizeof(ratios) / sizeof(ratios[0]); k++) {
        set_level(ls[nl - 1]);
        linear(2, 0, ratios[k], ref, FRAMES);
        for (int li = 0; li < nl; li++) {
            set_level(ls[li]);
            linear(2, 0, ratios[k], st, FRAMES);
            linear(1, 0, ratios[k], mono[0], FRAMES);
            linear(1, 1, ratios[k], mono[1], FRAMES);
            for (long i = 0; i < FRAMES; i++) {
                off += memcmp(&st[2 * i], &mono[0][i], 4) || memcmp(&st[2 * i + 1], &mono[1][i], 4);
            }
            levels_off += memcmp(st, ref, sizeof(st)) != 0;
        }
    }
    fprintf(stderr, "  linear stereo: %u frames off its channels alone, %u runs off the lowest level's\n", off,
            levels_off);
    HT_EQ_INT(off, 0);
    HT_EQ_INT(levels_off, 0);
    reset_level();
}

/* ---- `bench`: each path's time by level */
static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void bench(void)
{
    static float bins[8][NUM_SAMPLES_PER_FRAME], src[NUM_SAMPLES_PER_FRAME], conv[64];
    static short pcm[64];
    static HrtfFilter f;
    float h[2][HRTF_NUM_TAPS], io[HRTF_SAMPLES_PER_FRAME][2];
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        src[i] = rnd();
        io[i][0] = rnd();
        io[i][1] = rnd();
    }
    for (int i = 0; i < 64; i++) {
        conv[i] = rnd() * 1.2f;
    }
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            h[ch][k] = rnd();
        }
    }
    int ls[3];
    const int nl = levels(ls);
    for (int li = 0; li < nl; li++) {
        set_level(ls[li]);
        const int n = 200000;
        double t = now_us();
        for (int i = 0; i < n; i++) {
            for (int b = 0; b < 8; b++) {
                mix_one_bin(bins[b], src, 0.001f);
            }
            __asm__ __volatile__("" ::: "memory");
        }
        const double mix = (now_us() - t) / n * 1e3;
        hrtf_filter_init(&f);
        hrtf_filter_set_target_params(&f, h, 7.3f);
        t = now_us();
        for (int i = 0; i < n / 10; i++) {
            if (i % 50 == 0) {
                hrtf_filter_set_target_params(&f, h, i % 100 ? 7.3f : -11.6f);
            }
            hrtf_filter_process(&f, io, io);
            __asm__ __volatile__("" ::: "memory");
        }
        const double hrtf = (now_us() - t) / (n / 10) * 1e3;
        t = now_us();
        for (int i = 0; i < n; i++) {
            vp_float_to_short_array(conv, pcm, 64);
            __asm__ __volatile__("" ::: "memory");
        }
        const double cvt = (now_us() - t) / n * 1e3;
        fprintf(stderr, "  bench %-8s mix %6.1f ns (8 bins), hrtf %7.1f ns (a stereo frame, moving 1 in 50), "
                "to 16-bit %5.1f ns (64 samples)\n", level_name(ls[li]), mix, hrtf, cvt);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        bench();
        return 0;
    }
    HT_RUN(test_mix);
    HT_RUN(test_mix_ramp);
    HT_RUN(test_hrtf_neon_sums);
    HT_RUN(test_float_to_short);
    HT_RUN(test_linear_stereo);
    HT_DONE();
}
