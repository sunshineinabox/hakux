/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The voices' resamplers (vp/resample.h) through the real voice processing,
 * linear and sinc (the hakux_audio_resampler option, NC_APU_RESAMPLER):
 * a looping 22.05 kHz voice's levels and its seamless loop, the pitch
 * changing every frame, a voice's end (all of it heard before the voice
 * goes off; how far its guest-visible position runs ahead of what is
 * heard), short one-shot sounds heard whole, a reused voice, a persistent
 * stream starved and fed again, and an ADPCM voice's loop. `apu-resample-smoke bench`: throughput, the worst frame and
 * allocations at 32 to 256 voices (not a test; a host figure, not the
 * Nova's). Allocations are counted by wrapping malloc, calloc and realloc
 * (linked with --wrap). */
#include "apu_fixture.h"

static unsigned long g_allocs, g_alloc_bytes;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t s);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n)
{
    __atomic_fetch_add(&g_allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_alloc_bytes, n, __ATOMIC_RELAXED);
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t s)
{
    __atomic_fetch_add(&g_allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_alloc_bytes, n * s, __ATOMIC_RELAXED);
    return __real_calloc(n, s);
}
void *__wrap_realloc(void *p, size_t n)
{
    __atomic_fetch_add(&g_allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_alloc_bytes, n, __ATOMIC_RELAXED);
    return __real_realloc(p, n);
}

static const char *const k_q[2] = {"linear", "sinc"};

/* the amplitude of the sine at f Hz (48 kHz) in channel ch of y[start, +n) */
static double fit(int16_t (*y)[2], int ch, long start, long n, double f)
{
    double m[3][4] = {{0}};
    for (long i = 0; i < n; i++) {
        const double a = sin(2 * M_PI * f * i / 48000), b = cos(2 * M_PI * f * i / 48000);
        const double row[4] = {a, b, 1.0, y[start + i][ch] / 32767.0};
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

/* the rate a pitch plays at: 2^(-pitch / 4096) output samples an input one */
static double ratio_of(int16_t pitch)
{
    return pow(2.0, -pitch / 4096.0);
}

/* a mono 2D voice at 0 dB to both front bins */
static Voice front(uint16_t handle, uint32_t page, int16_t pitch)
{
    Voice v = voice(handle, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, page);
    v.vol[0] = v.vol[1] = 0;
    v.pitch = pitch;
    return v;
}

#define REC_FRAMES 400
static int16_t g_rec[REC_FRAMES * NUM_SAMPLES_PER_FRAME][2];

/* A looping 22.05 kHz voice: 1 kHz and 8 kHz levels (sinc within 1 dB, the
 * linear one reported), and the loop seamless (no step larger than the
 * tone's steepest) */
static void test_levels_and_loop(int q)
{
    static const double tones[] = {1000, 8000};
    const int16_t p = pitch_for(22050);
    const double ratio = ratio_of(p);
    for (int k = 0; k < 2; k++) {
        apu_up_with(false, true, 1, k_q[q]);
        pcm_tone(0, 2205, tones[k], 22050, 0.5);    /* 100 and 800 cycles: a seamless loop */
        Voice v = front(100, 0, p);
        v.ebo = 2205 - 1;
        start(&v);
        frames_into(REC_FRAMES, g_rec);
        apu_down();
        const double f = tones[k] * 48000.0 / (22050 * ratio);
        const double level = 20 * log10(fit(g_rec, 0, 3200, 9600, f) / 0.5);
        int worst = 0;
        for (long i = 3201; i < REC_FRAMES * NUM_SAMPLES_PER_FRAME; i++) {
            worst = MAX(worst, abs(g_rec[i][0] - g_rec[i - 1][0]));
        }
        const double steepest = 0.5 * 32767 * 2 * M_PI * f / 48000;
        char what[160];
        snprintf(what, sizeof(what), "%s: looping 22.05 kHz voice, %.0f Hz at %+.2f dB, largest step %d (the tone's "
                 "steepest %.0f)", k_q[q], tones[k], level, worst, steepest);
        check((q == 0 || fabs(level) < 1.0) && worst < 1.1 * steepest + 2, what);
    }
}

/* the pitch changing every frame (22.05 and 32 kHz voices in turn) */
static void test_pitch_every_frame(int q)
{
    apu_up_with(false, true, 1, k_q[q]);
    pcm_tone(0, 2205, 500, 22050, 0.5);
    Voice v = front(100, 0, pitch_for(22050));
    v.ebo = 2205 - 1;
    start(&v);
    int worst = 0;
    for (int f = 0; f < 200; f++) {
        pio(NV1BA0_PIO_SET_CURRENT_VOICE, 100);
        pio(NV1BA0_PIO_SET_VOICE_TAR_PITCH, (uint32_t)(uint16_t)pitch_for(f % 2 ? 32000 : 22050) << 16);
        frames_into(1, g_rec + NUM_SAMPLES_PER_FRAME * f);
    }
    apu_down();
    for (long i = 20 * NUM_SAMPLES_PER_FRAME; i < 200 * NUM_SAMPLES_PER_FRAME; i++) {
        worst = MAX(worst, abs(g_rec[i][0] - g_rec[i - 1][0]));
    }
    const double steepest = 0.5 * 32767 * 2 * M_PI * 500 * (32000.0 / 22050) / 48000;
    char what[120];
    snprintf(what, sizeof(what), "%s: pitch changing every frame, largest step %d (bound %.0f)", k_q[q], worst,
             1.2 * steepest);
    check(worst < 1.2 * steepest + 2, what);
}

/* A 2205-sample voice that doesn't loop: all of its 4800 output samples
 * heard, the voice inactive in the frame after its last (not when its
 * decoder reached the end, the resampler's look-ahead still to play), and
 * how far its guest-visible position (CBO) runs ahead of what has been
 * heard mid-way */
static void test_end(int q, int *end_frame, long *heard, long *lead)
{
    apu_up_with(false, true, 1, k_q[q]);
    pcm_tone(0, 2205, 1000, 22050, 0.5);
    const int16_t p = pitch_for(22050);
    Voice v = front(100, 0, p);
    v.ebo = 2205 - 1;
    v.loop = false;
    start(&v);
    *end_frame = -1;
    *lead = 0;
    for (int f = 0; f < 200; f++) {
        frames_into(1, g_rec + NUM_SAMPLES_PER_FRAME * f);
        if (f == 60) {
            *lead = (long)voice_cbo(100) - (long)floor(61 * NUM_SAMPLES_PER_FRAME / ratio_of(p));
        }
        if (*end_frame < 0 && !voice_active(100)) {
            *end_frame = f;
        }
    }
    apu_down();
    *heard = 0;
    for (long i = 0; i < 200 * NUM_SAMPLES_PER_FRAME; i++) {
        *heard = abs(g_rec[i][0]) > 30 ? i + 1 : *heard;
    }
    char what[200];
    const long whole = (long)(2205 * ratio_of(p));
    snprintf(what, sizeof(what), "%s: a voice's end: heard to sample %ld of %ld, inactive after frame %d, "
             "position %ld input samples ahead of the output", k_q[q], *heard, whole, *end_frame, *lead);
    check(*heard >= whole - 2 && *end_frame >= *heard / NUM_SAMPLES_PER_FRAME &&
          *end_frame <= *heard / NUM_SAMPLES_PER_FRAME + 2, what);
}

/* Short one-shot sounds, their decoder at the end in their first frame or
 * two: all of each heard, then the voice off. 64 frames at 22.05 kHz (139
 * output frames) and 128 at 96 kHz (64: the stretched filter), at half
 * scale: the output frames at half that level or more, less the first
 * frame's first half (a new voice fades in across its first frame,
 * measured). */
static void test_short(int q)
{
    static const struct {
        uint32_t frames;
        double fs;
    } k_sounds[] = {{64, 22050}, {128, 96000}};
    for (int k = 0; k < 2; k++) {
        apu_up_with(false, true, 1, k_q[q]);
        int16_t *pcm = (int16_t *)(g_ram + PCM_PA);
        for (uint32_t i = 0; i < k_sounds[k].frames; i++) {
            pcm[i] = 16384;
        }
        const int16_t p = pitch_for(k_sounds[k].fs);
        Voice v = front(100, 0, p);
        v.ebo = k_sounds[k].frames - 1;
        v.loop = false;
        start(&v);
        int end_frame = -1;
        for (int f = 0; f < 20; f++) {
            frames_into(1, g_rec + NUM_SAMPLES_PER_FRAME * f);
            if (end_frame < 0 && !voice_active(100)) {
                end_frame = f;
            }
        }
        apu_down();
        long loud = 0;
        for (long i = 0; i < 20 * NUM_SAMPLES_PER_FRAME; i++) {
            loud += g_rec[i][0] >= 8192;
        }
        const double want = k_sounds[k].frames * ratio_of(p) - NUM_SAMPLES_PER_FRAME / 2;
        char what[200];
        snprintf(what, sizeof(what), "%s: a %u-frame sound at %.2f kHz: %ld output frames heard of %.0f "
                 "(the fade-in's first 16 left out), inactive after frame %d", k_q[q], k_sounds[k].frames,
                 k_sounds[k].fs / 1000, loud, want, end_frame);
        check(fabs(loud - want) <= 3 && end_frame >= 0, what);
    }
}

/* A voice reused for a new sound sounds as it does on a fresh APU */
static void test_reuse(int q)
{
    static int16_t fresh[3 * NUM_SAMPLES_PER_FRAME][2], reused[3 * NUM_SAMPLES_PER_FRAME][2];
    apu_up_with(false, true, 1, k_q[q]);
    pcm_tone(1, 2205, 3000, 22050, 0.4);
    Voice b = front(100, 1, pitch_for(22050));
    b.ebo = 2205 - 1;
    start(&b);
    frames_into(3, fresh);
    apu_down();
    apu_up_with(false, true, 1, k_q[q]);
    pcm_tone(0, 2205, 1000, 22050, 0.5);
    pcm_tone(1, 2205, 3000, 22050, 0.4);
    Voice a = front(100, 0, pitch_for(32000));
    a.ebo = 2205 - 1;
    start(&a);
    frames(30);
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;    /* the list empty again: the voice started anew */
    start(&b);
    frames_into(3, reused);
    apu_down();
    char what[96];
    snprintf(what, sizeof(what), "%s: a reused voice's new sound as on a fresh APU", k_q[q]);
    check(memcmp(fresh, reused, sizeof(fresh)) == 0, what);
}

/* A persistent stream: segment list A (two 1024-sample segments of a tone)
 * plays, B is empty and the voice starves (silence, still on), then B is
 * given and it plays again */
static void test_stream_starve_recover(int q)
{
    apu_up_with(false, true, 1, k_q[q]);
    for (uint32_t p = 0; p < 4; p++) {
        int16_t *s = (int16_t *)(g_ram + PCM_PA + 0x1000 * p);
        for (uint32_t i = 0; i < 1024; i++) {
            s[i] = (int16_t)lrint(0.5 * 32767.0 * sin(2 * M_PI * 1000 * (p * 1024 + i) / 22050.0));
        }
        wr32(SSL_PA + 8 * p, PCM_PA + 0x1000 * p);
        wr32(SSL_PA + 8 * p + 4, 1024 | NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 16);
    }
    const int16_t p = pitch_for(22050);
    Voice v = front(100, 0, p);
    v.loop = false;
    v.fmt_extra = NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | NV_PAVS_VOICE_CFG_FMT_PERSIST;
    start(&v);
    pio(NV1BA0_PIO_SET_CURRENT_VOICE, 100);
    pio(NV1BA0_PIO_SET_VOICE_SSL_A, 0 << 8 | 2);
    pio(NV1BA0_PIO_SET_VOICE_SSL_B, 2 << 8 | 0);
    frames_into(180, g_rec);                      /* A: 2048 samples, about 4460 output */
    const bool on_starved = voice_active(100);
    int16_t starved_peak = 0;
    for (long i = 150 * NUM_SAMPLES_PER_FRAME; i < 180 * NUM_SAMPLES_PER_FRAME; i++) {
        starved_peak = MAX(starved_peak, (int16_t)abs(g_rec[i][0]));
    }
    const double played = fit(g_rec, 0, 20 * NUM_SAMPLES_PER_FRAME, 100 * NUM_SAMPLES_PER_FRAME,
                              1000 * 48000.0 / (22050 * ratio_of(p)));
    pio(NV1BA0_PIO_SET_CURRENT_VOICE, 100);
    pio(NV1BA0_PIO_SET_VOICE_SSL_B, 2 << 8 | 2);
    frames_into(100, g_rec);
    const double again = fit(g_rec, 0, 20 * NUM_SAMPLES_PER_FRAME, 60 * NUM_SAMPLES_PER_FRAME,
                             1000 * 48000.0 / (22050 * ratio_of(p)));
    apu_down();
    char what[200];
    snprintf(what, sizeof(what), "%s: stream played at %.3f, starved (peak %d, voice %s), fed again at %.3f", k_q[q],
             played, starved_peak, on_starved ? "on" : "off", again);
    check(fabs(played - 0.5) < 0.06 && starved_peak == 0 && on_starved && fabs(again - 0.5) < 0.06, what);
}

/* An ADPCM voice looping 4 blocks of a steady 8000 (header 8000, its
 * nibbles 0): steady through every loop */
static void test_adpcm_loop(int q)
{
    apu_up_with(false, true, 1, k_q[q]);
    uint8_t *b = g_ram + PCM_PA + 0x1000 * 4;
    for (int k = 0; k < 4; k++) {
        const int16_t h = 8000;
        memcpy(b + 36 * k, &h, 2);
    }
    Voice v = front(100, 4, 0);
    v.container = NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM;
    v.ebo = 4 * 64 - 1;
    start(&v);
    frames_into(80, g_rec);
    apu_down();
    int lo = 32767, hi = -32768;
    for (long i = 20 * NUM_SAMPLES_PER_FRAME; i < 80 * NUM_SAMPLES_PER_FRAME; i++) {
        lo = MIN(lo, g_rec[i][0]);
        hi = MAX(hi, g_rec[i][0]);
    }
    char what[120];
    snprintf(what, sizeof(what), "%s: ADPCM voice looping 4 blocks of 8000: %d to %d over 7 loops", k_q[q], lo, hi);
    check(lo >= 7990 && hi <= 8010, what);
}

/* `bench`: n voices (mono, 22.05, 32, 44.1 and 48 kHz in turn, looping
 * tones), one worker */
static void bench(int q, int n)
{
    apu_up_with(false, true, 1, k_q[q]);
    static const double rates[] = {22050, 32000, 44100, 48000};
    for (uint32_t p = 0; p < 4; p++) {
        pcm_tone(p, 1024, 440 + 220 * p, rates[p], 0.05);
    }
    for (int k = 0; k < n; k++) {
        const bool three_d = k >= 192;
        Voice v = front(three_d ? k - 192 : 64 + k, k % 4, pitch_for(rates[k % 4]));
        v.list = three_d ? NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP : NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP;
        v.ebo = 1024 - 1;
        start(&v);
    }
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, 0 | 1 << 8 | 2 << 16 | 3 << 24);
    frames(100);
    const unsigned long allocs = g_allocs, bytes = g_alloc_bytes;
    double worst = 0;
    struct timespec t0, t1, a, b;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < 3000; f++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        frames(1);
        clock_gettime(CLOCK_MONOTONIC, &b);
        worst = fmax(worst, (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("bench %-6s %3d voices: %6.1f us a frame (%.1fx real time), worst %6.1f us, %lu allocations (%lu bytes) "
           "in 3000 frames\n", k_q[q], n, s / 3000 * 1e6, 3000 / s / 1500.0, worst, g_allocs - allocs,
           g_alloc_bytes - bytes);
    apu_down();
}

static long silence(void *cb_data, float **data)
{
    static float zero[64];
    (void)cb_data;
    *data = zero;
    return 32;
}

/* the bytes a voice's resampler allocates, by quality */
static long converter_bytes(int q)
{
    const unsigned long bytes = g_alloc_bytes;
    VpResampler *r = vp_resample_new(silence, q == 1 ? VP_RESAMPLE_SINC : VP_RESAMPLE_LINEAR, 2, NULL);
    const long n = (long)(g_alloc_bytes - bytes);
    vp_resample_delete(r);
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    g_ram = calloc(1, RAM_SIZE);
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        for (int q = 0; q < 2; q++) {
            printf("bench %-6s a voice's resampler: %ld bytes\n", k_q[q], converter_bytes(q));
        }
        for (int q = 0; q < 2; q++) {
            static const int counts[] = {32, 64, 128, 256};
            for (int k = 0; k < 4; k++) {
                bench(q, counts[k]);
            }
        }
        return 0;
    }
    alarm(60);
    printf("sinc in this build: %s\n", vp_resample_sinc_available() ? "yes" : "no");
    check(vp_resample_sinc_available(), "the core is built with the sinc (NEON; x86-64: AVX2+FMA, SSE2)");
#if defined(__x86_64__)
    /* each vector level (vp/simd.h), capped at the VP's start by NC_APU_SIMD;
     * the CPU's own where it has no AVX2 */
    static const char *const caps[] = {"avx2", "sse2", "scalar"};
#else
    static const char *const caps[] = {"neon"};
#endif
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        setenv("NC_APU_SIMD", caps[c], 1);
        printf("vector paths: %s\n", caps[c]);
        int end[2];
        long heard[2], lead[2];
        for (int q = 0; q < 2; q++) {
            test_levels_and_loop(q);
            test_pitch_every_frame(q);
            test_end(q, &end[q], &heard[q], &lead[q]);
            test_short(q);
            test_reuse(q);
            test_stream_starve_recover(q);
            test_adpcm_loop(q);
        }
        /* sinc against linear at a voice's end: its position runs at most its
         * filter's look-ahead (16 frames) further ahead */
        check(lead[1] - lead[0] <= 16, "sinc's position lead against linear's");
    }
    unsetenv("NC_APU_SIMD");
    free(g_ram);
    return failures ? 1 : 0;
}
