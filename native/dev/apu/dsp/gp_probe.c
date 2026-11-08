/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — a diagnostic probe of the guest's GP program
 * (NC_APU_GP_PROBE=DIR, with NC_APU_DSP=1). From NC_APU_GP_PROBE_AT
 * seconds of APU frames (default 30: the title's DirectSound up, its DSP
 * image loaded) the VP's mix bins are replaced with a test tone, one bin
 * at a time, silence in every other, and what the GP and EP put out is
 * recorded: the mix buffer after the GP's frame and every output FIFO
 * write (DIR/probe.bin), and the DSPs' memories as the probe starts
 * (DIR/gp_mem.bin, DIR/ep_mem.bin). tools/native/apu-gp-probe.py reads
 * them.
 *
 * It measures how the title's DSP program mixes its bins to the outputs,
 * the stereo downmix among them, as the core's DSP interpreter runs it:
 * not the hardware's analog stage, and not how the VP's mix bins are
 * scaled into the GP on an Xbox. The title's own sounds are replaced
 * while it runs.
 *
 * The schedule: silence; bins 0 to 5 (DirectSound's speakers) at 1 kHz,
 * 100 Hz and 8 kHz; bins 6 to 31 at 1 kHz with a half-second gap after
 * each, for effect tails. Each tone 96 frames (64 ms) at 0.25 of full
 * scale, recorded whole; the gaps silent and not recorded.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "native/dev/apu/apu_int.h"
#include "native/dev/apu/dsp/gp_probe.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PROBE_MAGIC    0x46504750u     /* "PGPF" */
#define TONE_FRAMES    96
#define TONE_AMPLITUDE 0.25

typedef struct {
    int bin;                    /* -1: silence */
    float hz;
    int gap;                    /* silent frames after, not recorded */
} Segment;

static struct {
    bool on, started, done, recording;
    char dir[480];
    uint64_t frame, start;
    Segment segs[64];
    int nsegs, seg, seg_frame;
    double phase;
    FILE *out;
    uint8_t blobs[64 * 1024];
    size_t blobs_len;
    uint32_t nblobs;
} g_probe;

static void add(int bin, float hz, int gap)
{
    if (g_probe.nsegs < (int)(sizeof(g_probe.segs) / sizeof(g_probe.segs[0]))) {
        g_probe.segs[g_probe.nsegs++] = (Segment){bin, hz, gap};
    }
}

void gp_probe_init(void)
{
    memset(&g_probe, 0, sizeof(g_probe));
    const char *dir = getenv("NC_APU_GP_PROBE");
    if (!dir || !*dir) {
        return;
    }
    if (!g_apu_config.use_dsp) {
        nc_log("hakuX-apu", "gp probe: off, it needs NC_APU_DSP=1 (the GP's output used)");
        return;
    }
    snprintf(g_probe.dir, sizeof(g_probe.dir), "%s", dir);
    mkdir(g_probe.dir, 0755);
    const char *at = getenv("NC_APU_GP_PROBE_AT");
    g_probe.start = (uint64_t)((at ? atof(at) : 30.0) * 1500);
    add(-1, 0, 48);
    static const float speaker_hz[] = {1000, 100, 8000};
    for (int k = 0; k < 3; k++) {
        for (int bin = 0; bin < 6; bin++) {
            add(bin, speaker_hz[k], 48);
        }
    }
    for (int bin = 6; bin < NUM_MIXBINS; bin++) {
        add(bin, 1000, 750);
    }
    g_probe.on = true;
    nc_log("hakuX-apu", "gp probe: %d tones into %s from frame %llu", g_probe.nsegs, g_probe.dir,
           (unsigned long long)g_probe.start);
}

static void dump(const char *name, const uint32_t *words[], const size_t counts[], int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_probe.dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    for (int i = 0; i < n; i++) {
        fwrite(words[i], sizeof(uint32_t), counts[i], f);
    }
    fclose(f);
}

/* the DSPs' memories as the probe starts: P, X, Y and the GP's mix buffer,
 * 24-bit words in 32 bits; and the FIFOs' registers */
static void start(MCPXAPUState *d)
{
    const dsp_core_t *gp = &d->gp.dsp->core, *ep = &d->ep.dsp->core;
    const uint32_t *gw[] = {gp->pram, gp->xram, gp->yram, gp->mixbuffer};
    const size_t gn[] = {DSP_PRAM_SIZE, DSP_XRAM_SIZE, DSP_YRAM_SIZE, DSP_MIXBUFFER_SIZE};
    dump("gp_mem.bin", gw, gn, 4);
    const uint32_t *ew[] = {ep->pram, ep->xram, ep->yram};
    const size_t en[] = {DSP_PRAM_SIZE, DSP_XRAM_SIZE, DSP_YRAM_SIZE};
    dump("ep_mem.bin", ew, en, 3);
    char path[512];
    snprintf(path, sizeof(path), "%s/regs.txt", g_probe.dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "GPRST %08x EPRST %08x\n", d->gp.regs[NV_PAPU_GPRST], d->ep.regs[NV_PAPU_EPRST]);
        for (int i = 0; i < GP_OUTPUT_FIFO_COUNT; i++) {
            fprintf(f, "GPOF%d base %08x end %08x\n", i, d->regs[NV_PAPU_GPOFBASE0 + 0x10 * i],
                    d->regs[NV_PAPU_GPOFEND0 + 0x10 * i]);
        }
        for (int i = 0; i < EP_OUTPUT_FIFO_COUNT; i++) {
            fprintf(f, "EPOF%d base %08x end %08x\n", i, d->regs[NV_PAPU_EPOFBASE0 + 0x10 * i],
                    d->regs[NV_PAPU_EPOFEND0 + 0x10 * i]);
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/probe.bin", g_probe.dir);
    g_probe.out = fopen(path, "wb");
    g_probe.started = true;
    nc_log("hakuX-apu", "gp probe: started, GP %s, EP %s",
           (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST) ? "running" : "held",
           (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST) ? "running" : "held");
}

void gp_probe_frame_begin(MCPXAPUState *d, uint32_t *mixbuf)
{
    if (!g_probe.on || g_probe.done || ++g_probe.frame < g_probe.start) {
        return;
    }
    if (!g_probe.started) {
        start(d);
    }
    const Segment *s = &g_probe.segs[g_probe.seg];
    memset(mixbuf, 0, DSP_MIXBUFFER_SIZE * sizeof(*mixbuf));
    g_probe.recording = g_probe.seg_frame < TONE_FRAMES;
    if (g_probe.recording && s->bin >= 0) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbuf[s->bin * NUM_SAMPLES_PER_FRAME + i] = float_to_24b((float)(TONE_AMPLITUDE * sin(g_probe.phase)));
            g_probe.phase += 2 * M_PI * s->hz / 48000;
        }
    }
    g_probe.blobs_len = 0;
    g_probe.nblobs = 0;
}

void gp_probe_fifo(char dsp, unsigned index, const uint8_t *ptr, size_t len)
{
    if (!g_probe.recording || g_probe.blobs_len + 8 + len > sizeof(g_probe.blobs)) {
        return;
    }
    uint8_t *b = g_probe.blobs + g_probe.blobs_len;
    const uint32_t n = (uint32_t)len;
    b[0] = (uint8_t)dsp;
    b[1] = (uint8_t)index;
    b[2] = b[3] = 0;
    memcpy(b + 4, &n, 4);
    memcpy(b + 8, ptr, len);
    g_probe.blobs_len += 8 + len;
    g_probe.nblobs++;
}

/* A record: magic, the segment, its bin, its frequency, the frame in it,
 * the FIFO writes' count and bytes; the GP's mix buffer after its frame
 * (1024 words); the FIFO writes (dsp, index, 2 pad, length, data) */
void gp_probe_frame_end(MCPXAPUState *d)
{
    if (!g_probe.on || g_probe.done || g_probe.frame < g_probe.start) {
        return;
    }
    const Segment *s = &g_probe.segs[g_probe.seg];
    if (g_probe.recording && g_probe.out) {
        const uint32_t head[7] = {PROBE_MAGIC, (uint32_t)g_probe.seg, (uint32_t)s->bin, 0,
                                  (uint32_t)g_probe.seg_frame, g_probe.nblobs, (uint32_t)g_probe.blobs_len};
        uint32_t h[7];
        memcpy(h, head, sizeof(h));
        memcpy(&h[3], &s->hz, 4);
        fwrite(h, sizeof(h), 1, g_probe.out);
        fwrite(d->gp.dsp->core.mixbuffer, sizeof(uint32_t), DSP_MIXBUFFER_SIZE, g_probe.out);
        fwrite(g_probe.blobs, 1, g_probe.blobs_len, g_probe.out);
    }
    g_probe.recording = false;
    if (++g_probe.seg_frame >= TONE_FRAMES + s->gap) {
        g_probe.seg_frame = 0;
        g_probe.phase = 0;
        if (++g_probe.seg == g_probe.nsegs) {
            g_probe.done = true;
            if (g_probe.out) {
                fclose(g_probe.out);
                g_probe.out = NULL;
            }
            nc_log("hakuX-apu", "gp probe: done, %d tones recorded in %s", g_probe.nsegs, g_probe.dir);
        } else if (g_probe.segs[g_probe.seg].bin >= 0) {
            nc_log("hakuX-apu", "gp probe: bin %d at %.0f Hz", g_probe.segs[g_probe.seg].bin,
                   g_probe.segs[g_probe.seg].hz);
        }
    }
}
