/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The voices' resamplers (native/dev/apu/vp/resample.h) measured: linear,
 * and band-limited sinc (resample_sinc.c; AArch64 and x86-64, there at each
 * vector level the host has). Their
 * input comes from a callback 32 stereo frames at a time, as the VP feeds
 * them. Levels are least-squares sine fits over a window after the start
 * (no FFT leakage, no startup transient); the sinc converter is held to
 * the stereo audio spec's criteria, the linear one reported beside it.
 * `test_apu_resample bench`: each quality's time a voice-frame (32 stereo
 * output frames) at four rates. */
#include "hle_test.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "native/dev/apu/vp/resample.h"
#include "native/dev/apu/vp/simd.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- a test signal, 32 stereo frames a callback */
enum { TONE, IMPULSE, STEP };

typedef struct {
    int kind;
    double fs;                  /* its rate */
    double f[2], amp[2];        /* each channel's tone (f 0: DC) */
    long at;                    /* the impulse's or step's frame */
    long len;                   /* frames, then silence (-1: endless) */
    long n;                     /* frames given */
    float buf[64];
} Signal;

static long feed(void *p, float **data)
{
    Signal *s = p;
    for (int i = 0; i < 32; i++) {
        const long n = s->n + i;
        for (int ch = 0; ch < 2; ch++) {
            float v = 0.0f;
            if (s->len < 0 || n < s->len) {
                switch (s->kind) {
                case TONE:
                    v = s->f[ch] ? (float)(s->amp[ch] * sin(2 * M_PI * s->f[ch] * n / s->fs)) : (float)s->amp[ch];
                    break;
                case IMPULSE:
                    v = n == s->at ? (float)s->amp[ch] : 0.0f;
                    break;
                default:
                    v = n >= s->at ? (float)s->amp[ch] : 0.0f;
                    break;
                }
            }
            s->buf[2 * i + ch] = v;
        }
    }
    s->n += 32;
    *data = s->buf;
    return 32;          /* always 32, padded with silence, as the VP's callback */
}

static Signal tone(double fs, double fl, double al, double fr, double ar)
{
    return (Signal){.kind = TONE, .fs = fs, .f = {fl, fr}, .amp = {al, ar}, .len = -1};
}

/* n output frames of s at ratio, read 32 at a time */
static float *convert(int quality, Signal *s, double ratio, long n)
{
    float *out = calloc((size_t)n * 2, sizeof(float));
    VpResampler *r = vp_resample_new(feed, quality, 2, s);
    for (long done = 0; done < n;) {
        const long want = n - done < 32 ? n - done : 32;
        const long got = vp_resample_read(r, ratio, want, out + 2 * done);
        HT_CHECK(got == want);
        if (got <= 0) {
            break;
        }
        done += got;
    }
    vp_resample_delete(r);
    return out;
}

/* the amplitude of the sine at f in channel ch of y[start, start + n) */
static double fit(const float *y, int ch, long start, long n, double f, double fs)
{
    double m[3][4] = {{0}};
    for (long i = 0; i < n; i++) {
        const double a = sin(2 * M_PI * f * i / fs), b = cos(2 * M_PI * f * i / fs), v = y[2 * (start + i) + ch];
        const double row[4] = {a, b, 1.0, v};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                m[r][c] += row[r] * row[c];
            }
        }
    }
    for (int c = 0; c < 3; c++) {
        for (int r = c + 1; r < 3; r++) {
            const double k = m[r][c] / m[c][c];
            for (int j = c; j < 4; j++) {
                m[r][j] -= k * m[c][j];
            }
        }
    }
    double x[3];
    for (int r = 2; r >= 0; r--) {
        double v = m[r][3];
        for (int j = r + 1; j < 3; j++) {
            v -= m[r][j] * x[j];
        }
        x[r] = v / m[r][r];
    }
    return sqrt(x[0] * x[0] + x[1] * x[1]);
}

static double db(double x)
{
    return 20.0 * log10(x);
}

static const char *const k_name[2] = {"linear", "sinc"};
static int g_qualities;         /* 1: linear only, 2: and sinc */

/* ---- the criteria */

/* 22.05 kHz to 48 kHz: 1, 8, 9, 9.5 and 10 kHz within 1 dB */
static void test_passband_22k(void)
{
    static const double tones[] = {1000, 8000, 9000, 9500, 10000};
    for (int q = 0; q < g_qualities; q++) {
        fprintf(stderr, "  %-6s 22.05 -> 48 kHz:", k_name[q]);
        for (size_t k = 0; k < sizeof(tones) / sizeof(tones[0]); k++) {
            Signal s = tone(22050, tones[k], 0.5, tones[k], 0.5);
            float *y = convert(q, &s, 48000.0 / 22050, 12000);
            const double l = db(fit(y, 0, 4000, 7200, tones[k], 48000) / 0.5);
            const double r = db(fit(y, 1, 4000, 7200, tones[k], 48000) / 0.5);
            fprintf(stderr, " %5.0f Hz %+6.2f dB", tones[k], l);
            HT_CHECK(fabs(l - r) < 0.01);
            if (q == VP_RESAMPLE_SINC) {
                HT_CHECK(fabs(l) < 1.0);
            }
            free(y);
        }
        fprintf(stderr, "\n");
    }
}

/* 96 kHz to 48 kHz: a 30 kHz tone's 18 kHz alias 40 dB under the input */
static void test_alias_96k(void)
{
    for (int q = 0; q < g_qualities; q++) {
        Signal s = tone(96000, 30000, 0.5, 30000, 0.5);
        float *y = convert(q, &s, 0.5, 12000);
        const double a = db(fit(y, 0, 4000, 7200, 18000, 48000) / 0.5);
        fprintf(stderr, "  %-6s 96 -> 48 kHz, 30 kHz in: 18 kHz alias %+7.1f dB\n", k_name[q], a);
        if (q == VP_RESAMPLE_SINC) {
            HT_CHECK(a < -40.0);
        }
        free(y);
    }
}

/* 48 kHz to 24 kHz (a voice pitched up an octave): 5 kHz kept */
static void test_downsample_passband(void)
{
    for (int q = 0; q < g_qualities; q++) {
        Signal s = tone(48000, 5000, 0.5, 5000, 0.5);
        float *y = convert(q, &s, 0.5, 12000);
        const double l = db(fit(y, 0, 4000, 7200, 5000, 24000) / 0.5);
        fprintf(stderr, "  %-6s 48 -> 24 kHz: 5 kHz %+6.2f dB\n", k_name[q], l);
        if (q == VP_RESAMPLE_SINC) {
            HT_CHECK(fabs(l) < 1.0);
        }
        free(y);
    }
}

/* unity: 1 kHz as it was */
static void test_unity(void)
{
    for (int q = 0; q < g_qualities; q++) {
        Signal s = tone(48000, 1000, 0.5, 3000, 0.25);
        float *y = convert(q, &s, 1.0, 12000);
        const double l = db(fit(y, 0, 4000, 7200, 1000, 48000) / 0.5), r = db(fit(y, 1, 4000, 7200, 3000, 48000) / 0.25);
        fprintf(stderr, "  %-6s unity: 1 kHz %+6.3f dB, 3 kHz %+6.3f dB\n", k_name[q], l, r);
        HT_CHECK(fabs(l) < 0.1 && fabs(r) < 0.1);
        free(y);
    }
}

/* a silent channel stays silent beside a loud one; silence stays silence */
static void test_channels_and_silence(void)
{
    for (int q = 0; q < g_qualities; q++) {
        Signal s = tone(22050, 1000, 0.9, 0, 0.0);
        float *y = convert(q, &s, 48000.0 / 22050, 6000);
        unsigned leaked = 0;
        for (long i = 0; i < 6000; i++) {
            leaked += y[2 * i + 1] != 0.0f;
        }
        free(y);
        Signal z = tone(22050, 0, 0.0, 0, 0.0);
        y = convert(q, &z, 48000.0 / 22050, 6000);
        unsigned noise = 0;
        for (long i = 0; i < 12000; i++) {
            noise += y[i] != 0.0f;
        }
        free(y);
        fprintf(stderr, "  %-6s channel leakage: %u samples; silence: %u non-zero\n", k_name[q], leaked, noise);
        HT_EQ_INT(leaked, 0);
        HT_EQ_INT(noise, 0);
    }
}

/* DC gain, an impulse's delay, a step's overshoot, and how long a sound's
 * tail rings after its input stops */
static void test_dc_impulse_step_tail(void)
{
    for (int q = 0; q < g_qualities; q++) {
        const double ratio = 48000.0 / 22050;
        Signal dc = tone(22050, 0, 0.5, 0, 0.5);
        float *y = convert(q, &dc, ratio, 6000);
        double mean = 0;
        for (long i = 3000; i < 6000; i++) {
            mean += y[2 * i];
        }
        mean /= 3000;
        free(y);
        Signal imp = {.kind = IMPULSE, .fs = 22050, .amp = {1.0, 1.0}, .at = 1000, .len = -1};
        y = convert(q, &imp, ratio, 4000);
        long peak = 0;
        for (long i = 1; i < 4000; i++) {
            peak = fabsf(y[2 * i]) > fabsf(y[2 * peak]) ? i : peak;
        }
        free(y);
        Signal step = {.kind = STEP, .fs = 22050, .amp = {0.5, 0.5}, .at = 1000, .len = -1};
        y = convert(q, &step, ratio, 4000);
        double top = 0;
        for (long i = 0; i < 4000; i++) {
            top = fmax(top, y[2 * i]);
        }
        free(y);
        Signal fin = tone(22050, 1000, 0.5, 1000, 0.5);
        fin.len = 2000;                     /* then silence, as a voice that ended */
        y = convert(q, &fin, ratio, 8000);
        const long end = (long)ceil(2000 * ratio);
        long rings = 0;
        for (long i = end; i < 8000; i++) {
            if (fabsf(y[2 * i]) > 1e-4f) {
                rings = i - end + 1;
            }
        }
        free(y);
        const double delay = peak - 1000 * ratio;
        fprintf(stderr, "  %-6s DC gain %+7.4f dB, impulse delay %+.1f output frames, step overshoot %.1f %%, "
                        "tail after the input ends %ld output frames\n",
                k_name[q], db(mean / 0.5), delay, (top / 0.5 - 1) * 100, rings);
        HT_CHECK(fabs(db(mean / 0.5)) < 0.1);
        HT_CHECK(rings < 200);
        HT_CHECK(top < 0.5 * 1.2);
    }
}

/* How far ahead of its output a converter has taken its input: the frames
 * the callback gave past what the output covers (the decoder, and the
 * voice's guest-visible position, run that far ahead of what is heard),
 * the callback's 32-frame granularity included */
static void test_lookahead(void)
{
    static const double ratios[] = {48000.0 / 22050, 1.0, 0.5};
    for (int q = 0; q < g_qualities; q++) {
        fprintf(stderr, "  %-6s input taken ahead of the output:", k_name[q]);
        for (size_t k = 0; k < sizeof(ratios) / sizeof(ratios[0]); k++) {
            Signal s = tone(22050, 500, 0.5, 500, 0.5);
            long worst = 0;
            VpResampler *r = vp_resample_new(feed, q, 2, &s);
            float y[64];
            for (long out = 32; out <= 32 * 300; out += 32) {
                vp_resample_read(r, ratios[k], 32, y);
                const long ahead = s.n - (long)floor(out / ratios[k]);
                worst = ahead > worst ? ahead : worst;
            }
            vp_resample_delete(r);
            fprintf(stderr, "  ratio %.3f: up to %ld frames", ratios[k], worst);
            HT_CHECK(worst <= (q == VP_RESAMPLE_SINC ? 200 : 64));
        }
        fprintf(stderr, "\n");
    }
}

/* the same output whatever the reads' sizes (1, 31, 32, 33, 256 frames)
 * as one continuous conversion at the same rate */
static void test_chunking(void)
{
    static const long sizes[] = {1, 31, 32, 33, 256};
    const long n = 9000;
    const double ratio = 48000.0 / 22050;
    for (int q = 0; q < g_qualities; q++) {
        Signal s0 = tone(22050, 1234, 0.5, 4321, 0.3);
        float *ref = convert(q, &s0, ratio, n);
        double worst = 0;
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            Signal s = tone(22050, 1234, 0.5, 4321, 0.3);
            float *y = calloc((size_t)n * 2, sizeof(float));
            VpResampler *r = vp_resample_new(feed, q, 2, &s);
            for (long done = 0; done < n;) {
                const long want = n - done < sizes[k] ? n - done : sizes[k];
                const long got = vp_resample_read(r, ratio, want, y + 2 * done);
                HT_CHECK(got == want);
                done += got > 0 ? got : n;
            }
            vp_resample_delete(r);
            for (long i = 0; i < 2 * n; i++) {
                worst = fmax(worst, fabs((double)y[i] - ref[i]));
            }
            free(y);
        }
        free(ref);
        fprintf(stderr, "  %-6s chunks of 1, 31, 32, 33, 256 vs continuous: largest difference %.3g\n", k_name[q],
                worst);
        HT_CHECK(worst <= 1e-6);
    }
}

/* the pitch changing every read (1.6 and 2.6 in turn): no NaN, no jump
 * larger than the tone's own steepest step (with room for the ratio's) */
static void test_rapid_pitch(void)
{
    for (int q = 0; q < g_qualities; q++) {
        Signal s = tone(22050, 300, 0.5, 300, 0.5);
        VpResampler *r = vp_resample_new(feed, q, 2, &s);
        float y[64];
        float last = 0.0f;
        double worst = 0;
        bool finite = true;
        for (int i = 0; i < 4000; i++) {
            const double ratio = i % 2 ? 2.6 : 1.6;
            HT_CHECK(vp_resample_read(r, ratio, 32, y) == 32);
            for (int k = 0; k < 32; k++) {
                finite &= isfinite(y[2 * k]) != 0;
                if (i > 50) {
                    worst = fmax(worst, fabs((double)y[2 * k] - last));
                }
                last = y[2 * k];
            }
        }
        vp_resample_delete(r);
        const double bound = 0.5 * 2 * M_PI * 300 / 22050 / 1.6;   /* the steepest step at the slower ratio */
        fprintf(stderr, "  %-6s pitch 1.6 / 2.6 each read: largest step %.4f (the tone's steepest %.4f)\n", k_name[q],
                worst, bound);
        HT_CHECK(finite);
        HT_CHECK(worst < 1.5 * bound);
    }
}

/* the pitch's whole range with its envelope, changing every read (1/512,
 * the stretched filter's widest 1/8 and past it, just under and at 1, 512):
 * every read whole, every sample finite and within the input's peak */
static void test_extremes(void)
{
    static const double ratios[] = {1.0 / 512, 0.125, 0.13, 0.99, 1.0, 512.0, 0.1, 3.0};
    for (int q = 0; q < g_qualities; q++) {
        Signal s = tone(22050, 440, 0.5, 660, 0.5);
        VpResampler *r = vp_resample_new(feed, q, 2, &s);
        float y[64];
        bool whole = true, bounded = true;
        for (int i = 0; i < 4000; i++) {
            whole &= vp_resample_read(r, ratios[i % 8], 32, y) == 32;
            for (int k = 0; k < 64; k++) {
                bounded &= isfinite(y[k]) && fabsf(y[k]) < 0.5f * 1.2f;
            }
        }
        vp_resample_delete(r);
        fprintf(stderr, "  %-6s ratios 1/512 to 512 in turn: %s, %s\n", k_name[q], whole ? "every read whole" : "short",
                bounded ? "finite and bounded" : "out of bounds");
        HT_CHECK(whole);
        HT_CHECK(bounded);
    }
}

/* each ratio's rate as asked, to the VP's 1/512 and 512 (base pitch and
 * envelope): after n output frames the input taken is n / ratio plus what
 * the converter holds ahead (vp_resample_ahead; negative when the position
 * has stepped past it, up to a step), within its look-ahead and a
 * callback's 32 frames. Linear keeps its position in float: within 0.1 %. */
static void test_rate(void)
{
    static const double ratios[] = {1.0 / 512, 1.0 / 300, 1.0 / 9, 0.5, 2.0, 300.0, 512.0};
    for (int q = 0; q < g_qualities; q++) {
        fprintf(stderr, "  %-6s input taken against n / ratio:", k_name[q]);
        for (size_t k = 0; k < sizeof(ratios) / sizeof(ratios[0]); k++) {
            Signal s = tone(22050, 440, 0.5, 660, 0.5);
            VpResampler *r = vp_resample_new(feed, q, 2, &s);
            const long n = ratios[k] < 1 ? 64 : (long)(64 * ratios[k]);
            float y[64];
            for (long done = 0; done < n; done += 32) {
                vp_resample_read(r, ratios[k], 32, y);
            }
            const double taken = s.n - vp_resample_ahead(r), want = n / ratios[k];
            const double ahead = vp_resample_ahead(r);
            vp_resample_delete(r);
            fprintf(stderr, "  %g: %+.2g (ahead %.0f)", ratios[k], taken - want, ahead);
            HT_CHECK(fabs(taken - want) < (q == VP_RESAMPLE_SINC ? 1e-6 : 1e-3) * want + 1e-6);
            HT_CHECK(ahead >= -1 / ratios[k] && ahead <= 140 + 32);
        }
        fprintf(stderr, "\n");
    }
}

/* a voice reused (reset) converts its new sound as a new converter does */
static void test_reuse(void)
{
    for (int q = 0; q < g_qualities; q++) {
        /* one converter and one callback object, as the VP's voice filter:
         * a sound, then the voice's next sound in the same object */
        Signal s = tone(22050, 700, 0.8, 900, 0.8);
        VpResampler *r = vp_resample_new(feed, q, 2, &s);
        float junk[64];
        for (int i = 0; i < 100; i++) {
            vp_resample_read(r, 1.3, 32, junk);
        }
        const Signal next = tone(32000, 2000, 0.4, 2500, 0.2);
        s = next;
        vp_resample_reset(r);
        float y[64 * 50], fresh[64 * 50];
        for (int i = 0; i < 50; i++) {
            vp_resample_read(r, 48000.0 / 32000, 32, y + 64 * i);
        }
        vp_resample_delete(r);
        Signal c = next;
        VpResampler *f = vp_resample_new(feed, q, 2, &c);
        for (int i = 0; i < 50; i++) {
            vp_resample_read(f, 48000.0 / 32000, 32, fresh + 64 * i);
        }
        vp_resample_delete(f);
        fprintf(stderr, "  %-6s reset: %s\n", k_name[q], memcmp(y, fresh, sizeof(y)) ? "differs" : "as new");
        HT_CHECK(memcmp(y, fresh, sizeof(y)) == 0);
    }
}

/* `bench`: a voice-frame's time, 32 stereo output frames, by quality and
 * input rate, from a callback that only hands over a buffer */
static long feed_buffer(void *p, float **data)
{
    *data = p;
    return 32;
}

static void bench(void)
{
    static const double rates[] = {22050, 32000, 44100, 48000, 96000};
    float in[64];
    for (int i = 0; i < 64; i++) {
        in[i] = (float)(0.5 * sin(i * 0.3));
    }
    for (int q = 0; q < g_qualities; q++) {
        fprintf(stderr, "  bench %-6s", k_name[q]);
        for (size_t k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
            VpResampler *r = vp_resample_new(feed_buffer, q, 2, in);
            float y[64];
            for (int i = 0; i < 200; i++) {
                vp_resample_read(r, 48000.0 / rates[k], 32, y);
            }
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int i = 0; i < 20000; i++) {
                vp_resample_read(r, 48000.0 / rates[k], 32, y);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            vp_resample_delete(r);
            const double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / 20000;
            fprintf(stderr, "  %5.0f Hz %6.3f us", rates[k], us);
        }
        fprintf(stderr, "  (a voice-frame)\n");
    }
}

#if defined(__x86_64__)
/* The levels' sinc against each other (vp/simd.h): AVX2 + FMA, SSE2 and
 * scalar code round differently, within a 16-bit sample's 1/4 */
static void test_levels_agree(void)
{
    static const double ratios[] = {48000.0 / 22050, 48000.0 / 32000, 1.0, 48000.0 / 64000, 48000.0 / 96000,
                                    48000.0 / 192000, 48000.0 / 1e6};
    const int top = vp_simd_set(VP_SIMD_AVX2);
    for (size_t k = 0; k < sizeof(ratios) / sizeof(ratios[0]); k++) {
        float *y[3] = {NULL};
        for (int level = VP_SIMD_SCALAR; level <= top; level++) {
            vp_simd_set(level);
            Signal s = tone(48000 / ratios[k], 997, 0.5, 3001, 0.7);
            y[level] = convert(VP_RESAMPLE_SINC, &s, ratios[k], 4096);
        }
        fprintf(stderr, "  sinc at %.4f:", ratios[k]);
        for (int level = VP_SIMD_SCALAR; level < top; level++) {
            double e = 0;
            for (long i = 0; i < 2 * 4096; i++) {
                e = fmax(e, fabs((double)y[level][i] - y[top][i]));
            }
            fprintf(stderr, " %s %.3g", vp_simd_name(level), e);
            HT_CHECK(e < 0.25 / 32768);
        }
        fprintf(stderr, " off %s\n", vp_simd_name(top));
        for (int level = VP_SIMD_SCALAR; level <= top; level++) {
            free(y[level]);
        }
    }
    vp_simd_set(top);
}
#endif

/* ---- hardware: the Xbox's 8-point Lagrange interpolation (xemu #3105) */
static float g_hw_in[256][2];
static int g_hw_pos;
static float g_hw_frame[32 * 2];

static long hw_cb(void *cb_data, float **data)
{
    (void)cb_data;
    for (int i = 0; i < 32; i++) {
        const int k = g_hw_pos + i;
        g_hw_frame[2 * i] = k < 256 ? g_hw_in[k][0] : 0.0f;
        g_hw_frame[2 * i + 1] = k < 256 ? g_hw_in[k][1] : 0.0f;
    }
    g_hw_pos += 32;
    *data = g_hw_frame;
    return 32;
}

/* Check the exact 16.16 cursor after n outputs. */
static void hw_read(const uint32_t step, int n, float out[][2])
{
    uint32_t steps[256];
    for (int i = 0; i < n; i++) {
        steps[i] = step;
    }
    g_hw_pos = 0;
    VpResampler *r = vp_resample_new(hw_cb, VP_RESAMPLE_HARDWARE, 2, NULL);
    HT_CHECK(r && vp_resample_quality(r) == VP_RESAMPLE_HARDWARE);
    HT_EQ_INT(vp_resample_read_steps(r, steps, 1.0, n, &out[0][0]), n);
    HT_CHECK(g_hw_pos - vp_resample_ahead(r) == (double)n * step / 65536.0);
    vp_resample_delete(r);
}

/* Verify that output stays silent once the cursor and filter window pass
 * the last input sample, at whole and fractional steps. */
static void test_hardware_end(void)
{
    static const uint32_t steps[] = {0x10000, 0x8000, 0x5555, 0x18000};
    const int last = 40;                /* the sound's last frame: an impulse */
    for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
        memset(g_hw_in, 0, sizeof(g_hw_in));
        g_hw_in[last][0] = 1.0f;
        g_hw_pos = 0;
        VpResampler *r = vp_resample_new(hw_cb, VP_RESAMPLE_HARDWARE, 2, NULL);
        int n = 0;
        float y[2];
        for (; n < 200; n++) {
            const double silence = g_hw_pos > last + 1 ? g_hw_pos - (last + 1) : 0;
            if (vp_resample_ahead(r) - silence <= -vp_resample_reach(r)) {
                break;
            }
            vp_resample_read_steps(r, &steps[k], 1.0, 1, y);
        }
        const double cursor = (double)n * steps[k] / 65536.0;
        HT_CHECK(n < 200 && cursor <= last + 5 + steps[k] / 65536.0);
        int heard = 0;
        for (int i = 0; i < 16; i++) {
            vp_resample_read_steps(r, &steps[k], 1.0, 1, y);
            heard += y[0] != 0.0f;
        }
        HT_EQ_INT(heard, 0);
        vp_resample_delete(r);
    }
}

static void test_hardware(void)
{
    HT_EQ_INT(vp_resample_quality_named("hardware"), VP_RESAMPLE_HARDWARE);
    HT_EQ_INT(vp_resample_quality_named("sinc"), VP_RESAMPLE_SINC);
    HT_EQ_INT(vp_resample_quality_named("linear"), VP_RESAMPLE_LINEAR);
    HT_EQ_INT(vp_resample_quality_named("cubic"), -1);
    /* whole steps: the input itself, from its first frame */
    static float out[256][2];
    for (int k = 0; k < 256; k++) {
        g_hw_in[k][0] = (k % 17) / 17.0f - 0.5f;
        g_hw_in[k][1] = -g_hw_in[k][0] * 0.25f;
    }
    hw_read(0x10000, 64, out);
    HT_CHECK(memcmp(out, g_hw_in, sizeof(float[64][2])) == 0);
    /* half steps through an impulse at frame 10: the measured weights at
     * half a sample, 1225, -245, 49 and -5 / 2048 each side */
    memset(g_hw_in, 0, sizeof(g_hw_in));
    g_hw_in[10][0] = 1.0f;
    hw_read(0x8000, 48, out);
    static const float half[4] = {1225 / 2048.0f, -245 / 2048.0f, 49 / 2048.0f, -5 / 2048.0f};
    for (int n = 0; n < 48; n++) {
        float want = 0.0f;
        if (n == 20) {
            want = 1.0f;
        } else if (n & 1) {
            const int d = n < 20 ? (20 - n) / 2 : (n - 21) / 2;     /* 9.5, 10.5: 0 */
            want = d < 4 ? half[d] : 0.0f;
        }
        HT_CHECK(fabsf(out[n][0] - want) < 1e-7f && out[n][1] == 0.0f);
    }
    /* DC through thirds of a sample: unity within the weights' rounding */
    for (int k = 0; k < 256; k++) {
        g_hw_in[k][0] = g_hw_in[k][1] = 0.5f;
    }
    hw_read(0x5555, 96, out);
    for (int n = 16; n < 96; n++) {
        HT_CHECK(fabsf(out[n][0] - 0.5f) < 1e-4f && fabsf(out[n][1] - 0.5f) < 1e-4f);
    }
}

static void run_suite(void)
{
    HT_RUN(test_passband_22k);
    HT_RUN(test_alias_96k);
    HT_RUN(test_downsample_passband);
    HT_RUN(test_unity);
    HT_RUN(test_channels_and_silence);
    HT_RUN(test_dc_impulse_step_tail);
    HT_RUN(test_lookahead);
    HT_RUN(test_chunking);
    HT_RUN(test_rapid_pitch);
    HT_RUN(test_extremes);
    HT_RUN(test_rate);
    HT_RUN(test_reuse);
    HT_RUN(test_hardware);
    HT_RUN(test_hardware_end);
}

int main(int argc, char **argv)
{
    g_qualities = vp_resample_sinc_available() ? 2 : 1;
#if defined(__x86_64__)
    /* each vector level the host has (vp/simd.h), highest first */
    for (int level = vp_simd_set(VP_SIMD_AVX2); level >= VP_SIMD_SCALAR; level--) {
        vp_simd_set(level);
        fprintf(stderr, "  vector paths: %s\n", vp_simd_name(level));
        if (argc > 1 && !strcmp(argv[1], "bench")) {
            bench();
        } else {
            run_suite();
        }
    }
    vp_simd_set(VP_SIMD_AVX2);
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        return 0;
    }
    HT_RUN(test_levels_agree);
#else
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        bench();
        return 0;
    }
    if (g_qualities == 1) {
        fprintf(stderr, "  sinc not in this build: linear only\n");
    }
    run_suite();
#endif
    HT_DONE();
}
