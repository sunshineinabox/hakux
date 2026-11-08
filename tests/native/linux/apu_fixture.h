/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native APU tests' fixture (apu_stereo.c, apu_resample.c): an APU over
 * a guest RAM of its own, voices set up and started by the PIO methods
 * DirectSound sends (mcpx_apu_vp_write), their samples read through an
 * identity scatter-gather table, and frames run by mcpx_apu_vp_frame with
 * the monitor's output read back. The frame thread stays parked (SECTL
 * off): the test runs the frames. */
#ifndef TESTS_NATIVE_LINUX_APU_FIXTURE_H
#define TESTS_NATIVE_LINUX_APU_FIXTURE_H

#include <math.h>

#include "native/dev/apu/apu_int.h"
#include "native/dev/dev.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"

static unsigned failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

#define RAM_SIZE   (1u << 20)
#define VOICES_PA  0x10000u     /* the voice table: 256 x 128 bytes */
#define SGE_PA     0x20000u     /* the scatter-gather table */
#define SSL_PA     0x28000u     /* the stream segment table */
#define PCM_PA     0x30000u     /* sample pages, mapped in order */
#define PCM_PAGES  64u
#define LOOP_FRAMES 64u         /* pcm_page's loop */

static uint8_t *g_ram;
static MCPXAPUState *d;

static void wr32(uint32_t pa, uint32_t v)
{
    memcpy(g_ram + pa, &v, 4);
}

static void pio(uint32_t method, uint32_t arg)
{
    mcpx_apu_vp_write(d, method, arg, 4);
}

/* A fresh APU with its voice table, an identity scatter-gather table over
 * the sample pages, a stream segment table and empty voice lists; the
 * voices' resampler "linear" or "sinc" */
static void apu_up_with(bool dsp, bool hrtf, unsigned workers, const char *resampler)
{
    static NcVm vm;
    memset(g_ram, 0, RAM_SIZE);
    vm = (NcVm){.phys = g_ram, .ram_size = RAM_SIZE};
    char w[8];
    snprintf(w, sizeof(w), "%u", workers);
    setenv("NC_APU_WORKERS", w, 1);
    setenv("NC_APU_DSP", dsp ? "1" : "0", 1);
    setenv("NC_APU_HRTF", hrtf ? "1" : "0", 1);
    setenv("NC_APU_RESAMPLER", resampler, 1);
    unsetenv("NC_TRACE_APU");
    nc_dev_apu_init(&vm);
    d = g_state;
    for (uint32_t p = 0; p < PCM_PAGES; p++) {
        wr32(SGE_PA + 8 * p, PCM_PA + 0x1000 * p);
    }
    d->regs[NV_PAPU_VPVADDR] = VOICES_PA;
    d->regs[NV_PAPU_VPSGEADDR] = SGE_PA;
    d->regs[NV_PAPU_VPSSLADDR] = SSL_PA;
    mcpx_apu_vp_refresh_voice_cache(d, VOICES_PA);
    mcpx_apu_vp_refresh_ssl_cache(d, SSL_PA);
    d->regs[NV_PAPU_TVL2D] = d->regs[NV_PAPU_TVL3D] = d->regs[NV_PAPU_TVLMP] = 0xFFFF;
}

static void apu_up(bool dsp, bool hrtf, unsigned workers)
{
    apu_up_with(dsp, hrtf, workers, "linear");
}

static void apu_down(void)
{
    nc_dev_apu_stop();
    nc_dev_apu_free();
    nc_mmio_reset();
    nc_irq_reset();
}

/* Sample page p holds LOOP_FRAMES 16-bit frames of l (and r, stereo) */
static void pcm_page(uint32_t p, bool stereo, int16_t l, int16_t r)
{
    int16_t *s = (int16_t *)(g_ram + PCM_PA + 0x1000 * p);
    for (uint32_t i = 0; i < LOOP_FRAMES; i++) {
        s[stereo ? 2 * i : i] = l;
        if (stereo) {
            s[2 * i + 1] = r;
        }
    }
}

/* n mono 16-bit samples of a tone of f Hz at fs, amplitude a, from page p */
static void pcm_tone(uint32_t p, uint32_t n, double f, double fs, double a)
{
    int16_t *s = (int16_t *)(g_ram + PCM_PA + 0x1000 * p);
    for (uint32_t i = 0; i < n; i++) {
        s[i] = (int16_t)lrint(a * 32767.0 * sin(2 * M_PI * f * i / fs));
    }
}

typedef struct {
    uint16_t handle;
    unsigned list;              /* NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_*_TOP */
    bool stereo;
    uint32_t page;              /* its samples' page (a byte offset of page x 4 KiB) */
    int bin[8];
    uint16_t vol[8];
    uint32_t fmt_extra;         /* multipass, its bin, clear-mix, stream, persist */
    uint16_t hrtf;              /* 3D: its HRTF entry, HRTF_NULL_HANDLE for none */
    int16_t pitch;              /* the rate: 2^(-pitch / 4096) output / input */
    uint32_t ebo;               /* the buffer's last sample */
    bool loop;
    unsigned container;         /* NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_* */
} Voice;

static Voice voice(uint16_t handle, unsigned list, bool stereo, uint32_t page)
{
    Voice v = {.handle = handle, .list = list, .stereo = stereo, .page = page, .hrtf = HRTF_NULL_HANDLE,
               .ebo = LOOP_FRAMES - 1, .loop = true, .container = NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16};
    for (int b = 0; b < 8; b++) {
        v.bin[b] = b;
        v.vol[b] = 0xFFF;
    }
    return v;
}

/* the pitch for a voice of fs Hz played at the output's 48 kHz */
static int16_t pitch_for(double fs)
{
    return (int16_t)lrint(-4096.0 * log2(48000.0 / fs));
}

/* The voice's send volumes, as it starts or while it plays (DirectSound's
 * SetMixBinVolumes) */
static void volumes(const Voice *v)
{
    pio(NV1BA0_PIO_SET_CURRENT_VOICE, v->handle);
    const uint16_t *a = v->vol;
    pio(NV1BA0_PIO_SET_VOICE_TAR_VOLA, (a[6] & 0xF) | (uint32_t)a[0] << 4 | (a[7] & 0xF) << 16 | (uint32_t)a[1] << 20);
    pio(NV1BA0_PIO_SET_VOICE_TAR_VOLB,
        (a[6] >> 4 & 0xF) | (uint32_t)a[2] << 4 | (a[7] >> 4 & 0xF) << 16 | (uint32_t)a[3] << 20);
    pio(NV1BA0_PIO_SET_VOICE_TAR_VOLC,
        (a[6] >> 8 & 0xF) | (uint32_t)a[4] << 4 | (a[7] >> 8 & 0xF) << 16 | (uint32_t)a[5] << 20);
}

static void start(const Voice *v)
{
    pio(NV1BA0_PIO_SET_CURRENT_VOICE, v->handle);
    pio(NV1BA0_PIO_SET_VOICE_CFG_VBIN, (uint32_t)v->bin[0] | v->bin[1] << 5 | v->bin[2] << 10 |
                                           v->bin[3] << 16 | v->bin[4] << 21 | (uint32_t)v->bin[5] << 26);
    /* a block a sample frame: DirectSound's samples per block are the
     * voice's channels (the field holds one less) */
    pio(NV1BA0_PIO_SET_VOICE_CFG_FMT,
        (uint32_t)v->bin[6] | v->bin[7] << 5 | v->fmt_extra |
            (v->stereo ? NV_PAVS_VOICE_CFG_FMT_STEREO | 1u << 16 : 0) | (v->loop ? NV_PAVS_VOICE_CFG_FMT_LOOP : 0) |
            NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28 | (uint32_t)v->container << 30);
    volumes(v);
    pio(NV1BA0_PIO_SET_VOICE_TAR_PITCH, (uint32_t)(uint16_t)v->pitch << 16);
    pio(NV1BA0_PIO_SET_VOICE_TAR_HRTF, v->hrtf);
    pio(NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE, 0x1000 * v->page);
    pio(NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO, 0);
    pio(NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO, v->ebo);
    pio(NV1BA0_PIO_SET_ANTECEDENT_VOICE, v->list << 16);
    pio(NV1BA0_PIO_VOICE_ON, v->handle);        /* envelopes off: full level */
}

static bool voice_active(uint16_t handle)
{
    uint32_t w;
    memcpy(&w, g_ram + VOICES_PA + handle * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE, 4);
    return GET_MASK(w, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
}

static uint32_t voice_cbo(uint16_t handle)
{
    uint32_t w;
    memcpy(&w, g_ram + VOICES_PA + handle * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_OFFSET, 4);
    return GET_MASK(w, NV_PAVS_VOICE_PAR_OFFSET_CBO);
}

static int16_t g_out[NUM_SAMPLES_PER_FRAME][2];
static float g_mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

/* n frames; the monitor's output of the last (frame_buf's first 32 pairs:
 * ep_frame_div stays 0 here) and its mix bins; into rec (32 pairs a
 * frame), when given, every frame's */
static void frames_into(int n, int16_t (*rec)[2])
{
    for (int f = 0; f < n; f++) {
        pthread_mutex_lock(&d->lock);
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        memset(g_mixbins, 0, sizeof(g_mixbins));
        mcpx_apu_vp_frame(d, g_mixbins);
        memcpy(g_out, d->monitor.frame_buf, sizeof(g_out));
        pthread_mutex_unlock(&d->lock);
        if (rec) {
            memcpy(rec + NUM_SAMPLES_PER_FRAME * f, g_out, sizeof(g_out));
        }
    }
}

static void frames(int n)
{
    frames_into(n, NULL);
}

/* Hardware attenuation curve from xemu #3105: interpolate between dB
 * steps, then fade linearly from 48 dB to silence at 63 dB. */
static float attenuation(uint16_t vol)
{
    uint32_t nodes[64];
    for (int k = 0; k <= 48; k++) {
        nodes[k] = (uint32_t)floor(4095.0 * pow(10.0, -k / 20.0) + 0.5);
    }
    for (int k = 49; k < 64; k++) {
        nodes[k] = nodes[48] * (63 - k) / 15;
    }
    const int att = vol & 0xFFF;
    if (att >= 63 * 64) {
        return 0.0f;
    }
    const uint32_t n0 = nodes[att >> 6], n1 = nodes[(att >> 6) + 1];
    return ((256 * n0 - 4 * (n0 - n1) * (att & 63)) >> 6) / 16384.0f;
}

/* HRIR tap 7Fh: 31/32 (a sign-magnitude minifloat, not normalized) */
#define TAP_7F (31.0f / 32.0f)

/* the monitor's 16-bit sample of x */
static int pcm(float x)
{
    return (int)(fminf(fmaxf(x, -1.0f), 1.0f) * 32767.0f);
}

#endif
