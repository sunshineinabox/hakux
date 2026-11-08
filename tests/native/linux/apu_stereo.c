/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The VP's own mix (MON_VP, the default without the DSP) through the real
 * voice processing: voices set up and started by the PIO methods
 * DirectSound sends (mcpx_apu_vp_write), their PCM read from guest RAM
 * through the scatter-gather table, a frame mixed by mcpx_apu_vp_frame and
 * read from the monitor's output. 2D and 3D voices are mixed by the
 * stereo downmix of their speaker and 3D sends (vp/stereo_route.h), their
 * LFE and effect sends left out; multipass groups and voices sending only to
 * the LFE or effect bins keep the monitor's mix; the full-DSP monitor leaves the
 * monitor's output alone and fills the mix bins. A voice reused for a new
 * sound starts its HRTF filter from silence. One worker and several give
 * the same output. No guest executable or sound device: the frame thread
 * stays parked (SECTL off) and this test runs the frames. */
#include "apu_fixture.h"
#include "native/dev/apu/vp/stereo_route.h"
#include "native/state/lock.h"
#include "native/state/sections.h"
#include "native/state/state.h"

static bool all(int ch, int want, int tol)
{
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        if (abs(g_out[i][ch] - want) > tol) {
            printf("  sample %d channel %d: %d, expected %d\n", i, ch, g_out[i][ch], want);
            return false;
        }
    }
    return true;
}

static bool channels_equal(void)
{
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        if (g_out[i][0] != g_out[i][1]) {
            return false;
        }
    }
    return true;
}

#define HALF 16384              /* 0.5 */

/* Drop the named final APU field to simulate an older state. Return the
 * new length, or zero if that field cannot be removed. */
static size_t cut_last_field(NcBuf *b, size_t len, const char *name)
{
    const size_t nl = strlen(name);
    size_t count = 0, field = 0;
    for (size_t at = 0; at + 11 <= len; at++) {
        if (b->p[at + 4] == 6 && !memcmp(b->p + at + 5, "gp.dsp", 6)) {
            count = at;
            break;
        }
    }
    for (size_t at = len - nl - 1; at > count; at--) {
        if (b->p[at] == nl && !memcmp(b->p + at + 1, name, nl)) {
            field = at;
            break;
        }
    }
    uint32_t n;
    memcpy(&n, b->p + count, 4);
    if (!field || n < 2) {
        return 0;
    }
    n--;
    memcpy(b->p + count, &n, 4);
    return field;
}

static void test_2d_mono_panned(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice v = voice(100, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0;               /* front left, 0 dB; front right muted */
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f), 1) && all(1, 0, 0), "2D mono to front left only: left 0.5, right exactly 0");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    v.vol[0] = 0x040;           /* -1 dB */
    v.vol[1] = 0x300;           /* -12 dB */
    start(&v);
    frames(4);
    const float l = 0.5f * attenuation(0x040), r = 0.5f * attenuation(0x300);
    check(all(0, pcm(l), 1) && all(1, pcm(r), 1),
          "2D mono, unequal sends: each speaker at its own send's attenuation");
    apu_down();
}

static void test_2d_stereo(void)
{
    apu_up(false, true, 1);
    pcm_page(0, true, HALF, -HALF / 2);
    Voice v = voice(101, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, true, 0);
    v.vol[0] = 0x080;           /* slot 0 (source left) to front left */
    v.vol[1] = 0x100;           /* slot 1 (source right) to front right */
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f * attenuation(0x080)), 1) && all(1, pcm(-0.25f * attenuation(0x100)), 1),
          "2D stereo: each source channel to its speaker at its gain");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, true, HALF, -HALF / 2);
    v.bin[0] = 1;               /* reordered: slot 0 to front right, slot 1 to front left */
    v.bin[1] = 0;
    start(&v);
    frames(4);
    check(all(1, pcm(0.5f * attenuation(0x080)), 1) && all(0, pcm(-0.25f * attenuation(0x100)), 1),
          "2D stereo, reordered bins: destination by bin, source by slot");
    apu_down();
}

static void test_2d_headroom(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    pio(NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * 0, 1);
    pio(NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * 1, 2);
    Voice v = voice(102, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = v.vol[1] = 0;
    start(&v);
    frames(4);
    check(all(0, pcm(0.25f), 1) && all(1, pcm(0.125f), 1), "2D headroom: each bin's own, kept");
    apu_down();
}

/* The stereo downmix of a 2D voice's speaker sends (before, any send past
 * front left and right gave the monitor's mix: the loudest send's gain on
 * both channels) */
static void test_2d_downmix(void)
{
    const float k = 0.70710678f;
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice v = voice(103, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0x300;           /* front left, -12 dB */
    v.vol[2] = 0;               /* front centre, 0 dB */
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f * (attenuation(0x300) + k)), 1) && all(1, pcm(0.5f * k), 1),
          "2D with a centre send: the centre on both speakers at -3 dB, front left on the left");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    v = voice(103, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[1] = 0;               /* front right, 0 dB */
    v.vol[5] = 0;               /* back right, 0 dB */
    start(&v);
    frames(4);
    check(all(0, 0, 0) && all(1, pcm(0.5f * (1 + k)), 1),
          "2D panned right by front and back right: right only, the back at -3 dB");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    v = voice(103, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0x300;           /* front left, -12 dB */
    v.bin[2] = 10;              /* I3DL2 (reverb), 0 dB: louder than the dry send */
    v.vol[2] = 0;
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f * attenuation(0x300)), 1) && all(1, 0, 0),
          "2D with a reverb send: panned by its speaker send, the reverb send left out");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    v = voice(103, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[3] = 0;               /* LFE only, 0 dB */
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f), 1) && channels_equal(), "2D with only an LFE send: the monitor's mix kept");
    apu_down();
}

/* A 2D voice keeps its sound's route as its volumes change: a dry send
 * fading to mute beside a reverb send fades the voice out (not to the
 * reverb send's level, centred), a dry send fading in beside one stays in
 * the monitor's mix (no step), a sound starting muted takes its route at
 * its first audible frame, and a new sound (VOICE_ON) chooses again */
static void test_2d_route_kept(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice v = voice(105, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0;               /* front left, 0 dB */
    v.bin[2] = 10;              /* I3DL2 (reverb), 0 dB throughout */
    v.vol[2] = 0;
    start(&v);
    frames(4);
    bool ok = all(0, pcm(0.5f), 1) && all(1, 0, 0);
    v.vol[0] = 0x900;           /* -36 dB */
    volumes(&v);
    frames(2);
    ok = ok && all(0, pcm(0.5f * attenuation(0x900)), 1) && all(1, 0, 0);
    v.vol[0] = 0xFFE;           /* past 63 dB: nothing (measured), as muted */
    volumes(&v);
    frames(2);
    ok = ok && all(0, 0, 0) && all(1, 0, 0);
    check(ok, "2D dry send fading to mute beside a reverb send: the left at 0.5, 0.008, then 0, the right 0");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    v.vol[0] = 0xFFF;           /* front left muted at the start, the reverb send audible */
    start(&v);
    frames(4);
    ok = all(0, pcm(0.5f), 1) && all(1, pcm(0.5f), 1);
    v.vol[0] = 0xFFE;
    volumes(&v);
    frames(2);
    ok = ok && all(0, pcm(0.5f), 1) && all(1, pcm(0.5f), 1);
    v.vol[0] = 0;
    volumes(&v);
    frames(2);
    ok = ok && all(0, pcm(0.5f), 1) && all(1, pcm(0.5f), 1);
    check(ok, "2D dry send fading in beside a reverb send: the monitor's mix throughout, 0.5 on both, no step");

    d->regs[NV_PAPU_TVL2D] = 0xFFFF;    /* the list again empty: a new sound, front left audible */
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f), 1) && all(1, 0, 0), "2D voice's new sound (VOICE_ON): its route chosen again, panned");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    v = voice(106, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.bin[2] = 10;              /* every send muted at the start */
    start(&v);
    frames(2);
    ok = all(0, 0, 0) && all(1, 0, 0);
    v.vol[0] = 0x300;           /* front left fades in, -12 dB */
    volumes(&v);
    frames(2);
    ok = ok && all(0, pcm(0.5f * attenuation(0x300)), 1) && all(1, 0, 0);
    v.vol[2] = 0;               /* then the reverb send, 0 dB: left out */
    volumes(&v);
    frames(2);
    ok = ok && all(0, pcm(0.5f * attenuation(0x300)), 1) && all(1, 0, 0);
    check(ok, "2D sound starting muted: routed at its first audible frame, panned left, its later reverb send left out");
    apu_down();
}

/* A 2D voice's route across a savestate (state.c's vp.route): its dry send
 * faded to mute beside a reverb send, saved and loaded, it stays silent
 * (chosen again it would play the reverb send's level, centred); the same
 * state without the routes, as an older one, loads with every voice
 * undecided */
static void test_2d_route_saved(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice v = voice(107, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0;               /* front left, 0 dB, then muted */
    v.bin[2] = 10;              /* I3DL2, 0 dB */
    v.vol[2] = 0;
    start(&v);
    frames(4);
    v.vol[0] = 0xFFF;
    volumes(&v);
    frames(2);
    const bool before = all(0, 0, 0) && all(1, 0, 0);
    NcBuf b = {0};
    nc_apu_state_save(&b);
    NcRd r = {b.p, b.len, 0, false};
    const bool loaded = nc_apu_state_load(&r, NC_STATE_V_APU);
    const bool kept = d->vp.route[107] == VP_ROUTE_SPEAKERS;
    frames(2);
    check(!b.fail && before && loaded && kept && all(0, 0, 0) && all(1, 0, 0),
          "2D voice's route across a savestate: faded to mute beside a reverb send, silent before and after");

    /* Simulate a state saved before vp.route and vp.filters. */
    size_t len = cut_last_field(&b, b.len, "vp.filters");
    len = len ? cut_last_field(&b, len, "vp.route") : 0;
    NcRd old = {b.p, len, 0, false};
    const bool old_loaded = len && nc_apu_state_load(&old, NC_STATE_V_APU);
    bool undecided = true;
    for (int k = 0; k < MCPX_HW_MAX_VOICES; k++) {
        undecided &= d->vp.route[k] == VP_ROUTE_UNDECIDED;
    }
    check(old_loaded && undecided, "a state without the routes (an older one): loads, every voice undecided");
    nc_buf_free(&b);
    apu_down();
}

/* Check sample-for-sample LFO restoration and VOICE_ON defaults for old states. */
static void test_lfo_saved(void)
{
    apu_up(false, false, 1);
    pcm_page(0, false, HALF, 0);
    Voice v = voice(108, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0;               /* front left, 0 dB */
    start(&v);
    pio(NV1BA0_PIO_SET_CURRENT_VOICE, v.handle);
    pio(NV1BA0_PIO_SET_VOICE_LFO_ENV, 0x40u << 12);    /* LFO A: delta 40h */
    pio(NV1BA0_PIO_SET_VOICE_LFO_MOD, 127);            /* amplitude +127 */
    frames(5);
    NcBuf b = {0};
    nc_apu_state_save(&b);
    static int16_t after_save[8 * NUM_SAMPLES_PER_FRAME][2], after_load[8 * NUM_SAMPLES_PER_FRAME][2];
    frames_into(8, after_save);
    NcRd r = {b.p, b.len, 0, false};
    const bool loaded = nc_apu_state_load(&r, NC_STATE_V_APU);
    frames_into(8, after_load);
    const bool moving = after_save[0][0] != after_save[7 * NUM_SAMPLES_PER_FRAME][0];
    check(!b.fail && loaded && moving && !memcmp(after_save, after_load, sizeof(after_save)),
          "a voice's LFO across a savestate: the same frames after the load as after the save");

    const size_t len = cut_last_field(&b, b.len, "vp.filters");
    NcRd old = {b.p, len, 0, false};
    const bool old_loaded = len && nc_apu_state_load(&old, NC_STATE_V_APU);
    const MCPXAPUVoiceFilter *f = &d->vp.filters[v.handle];
    check(old_loaded && !f->lfo_armed && !f->gains && !f->pitched,
          "a state without the voices' state (an older one): loads, each voice as at its VOICE_ON");
    nc_buf_free(&b);
    apu_down();
}

/* DirectSound's HRTF submixes, as Dead or Alive 3 and The Thing set them:
 * the VP's HRTF outputs 0 to 3 into 3D front left, back left, front right
 * and back right (bins 6, 8, 7, 9) */
#define DS_HRTF_SUBMIXES (6 | 8 << 8 | 7 << 16 | 9 << 24)

/* A 3D voice through DirectSound's 3D bins (its HRTF submixes, crosstalk
 * front left, back left, front right, back right): its ears by its sends,
 * the left ear the first two and the right ear the last two, the front
 * pair at 0 dB, the back pair at -3 dB, as the guest DSP image puts them on
 * a stereo console; the HRTF filter off, or on with an entry that passes
 * its input (tap 0 both ears, 7Fh: 31/32); without an entry its HRTF
 * sends silent, and at their bins' headroom, not the HRTF headroom
 * (measured on hardware: xemu #3105) */
static void test_3d(void)
{
    const float k = 0.70710678f;
    for (int hrtf = 0; hrtf < 2; hrtf++) {
        apu_up(false, hrtf, 1);
        pcm_page(0, false, HALF, 0);
        pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, DS_HRTF_SUBMIXES);
        pio(NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 5);
        pio(NV1BA0_PIO_SET_HRIR, 0x7F | 0x7F << 8);     /* taps 0: 31/32 */
        Voice v = voice(7, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
        v.vol[0] = 0;               /* left ear to 3D front left, 0 dB */
        v.vol[2] = 0x300;           /* right ear to 3D front right, -12 dB */
        v.hrtf = 5;
        start(&v);
        frames(4);
        const float ear = hrtf ? 0.5f * TAP_7F : 0.5f;
        check(all(0, pcm(ear), 1) && all(1, pcm(ear * attenuation(0x300)), 1),
              hrtf ? "3D voice, HRTF on: its ears by its front sends, left 0.48, right -12 dB"
                   : "3D voice, HRTF off: its ears by its front sends, left 0.5, right -12 dB");
        apu_down();
    }

    /* an entry that hears the source with the left ear only (the right
     * ear's taps 0): left 0.5, right exactly 0. The left ear taken from the
     * even slots put it on both speakers, the same sound on each */
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, DS_HRTF_SUBMIXES);
    pio(NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 5);
    pio(NV1BA0_PIO_SET_HRIR, 0x7F);             /* left tap 0 only */
    Voice v = voice(7, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
    v.vol[0] = v.vol[2] = 0;    /* the front pair, 0 dB */
    v.hrtf = 5;
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f * TAP_7F), 1) && all(1, 0, 0),
          "3D voice heard by the left ear only: left 0.48, right exactly 0");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, DS_HRTF_SUBMIXES);
    pio(NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 5);
    pio(NV1BA0_PIO_SET_HRIR, 0x7F | 0x7F << 8);
    v = voice(7, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
    v.vol[1] = v.vol[3] = 0;    /* behind the listener: the back pair, 0 dB */
    v.hrtf = 5;
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f * TAP_7F * k), 1) && all(1, pcm(0.5f * TAP_7F * k), 1),
          "3D voice behind the listener: -3 dB on both sides");
    apu_down();

    /* no HRTF entry: its HRTF sends silent */
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, DS_HRTF_SUBMIXES);
    v = voice(7, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
    v.vol[0] = v.vol[2] = 0;
    start(&v);
    frames(4);
    check(all(0, 0, 0) && all(1, 0, 0), "3D voice without an HRTF entry: its HRTF sends silent");
    apu_down();

    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, DS_HRTF_SUBMIXES);
    pio(NV1BA0_PIO_SET_HRTF_HEADROOM, 1);           /* no effect */
    pio(NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * 6, 3); /* 3D front left's: / 8 */
    pio(NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 5);
    pio(NV1BA0_PIO_SET_HRIR, 0x7F | 0x7F << 8);
    v = voice(7, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
    v.vol[0] = v.vol[2] = 0;
    v.hrtf = 5;
    start(&v);
    frames(4);
    check(all(0, pcm(0.5f * TAP_7F / 8), 1) && all(1, pcm(0.5f * TAP_7F), 1),
          "3D voice's HRTF sends at their bins' headroom, not the HRTF headroom");
    apu_down();
}

/* A multipass group: a source voice into the multipass bin at -6 dB, its
 * destination from it to both front bins at 0 dB: the destination's mix
 * of the source, once */
static void test_multipass(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice dst = voice(200, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP, false, 1);
    dst.fmt_extra = NV_PAVS_VOICE_CFG_FMT_MULTIPASS | MULTIPASS_BIN << 16 | NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX;
    dst.vol[0] = dst.vol[1] = 0;
    Voice src = voice(201, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP, false, 0);
    src.bin[0] = MULTIPASS_BIN;
    src.vol[0] = 0x180;
    start(&dst);                /* the list's top is the last started: src, then dst */
    start(&src);
    frames(4);
    check(all(0, pcm(0.5f * attenuation(0x180)), 1) && channels_equal(),
          "multipass group: the destination's mix of its source, once");
    apu_down();
}

/* The full-DSP monitor: the voice's sends into the mix bins, nothing in the
 * monitor's output */
static void test_full_dsp(void)
{
    apu_up(true, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice v = voice(104, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    v.vol[0] = 0;
    v.vol[1] = 0x300;
    start(&v);
    frames(4);
    bool bins = true;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        bins &= fabsf(g_mixbins[0][i] - 0.5f) < 1e-4f && fabsf(g_mixbins[1][i] - 0.5f * attenuation(0x300)) < 1e-4f;
    }
    check(bins && all(0, 0, 0) && all(1, 0, 0), "full-DSP monitor: mix bins by send, monitor output untouched");
    apu_down();

    /* a mono 3D voice heard by the left ear only: the left ear into its
     * sends to 3D front left and back left, the right ear (silent) into
     * front right and back right. Its right ear was lost (each send took
     * channel slot % 1, the left ear) */
    apu_up(true, true, 1);
    pcm_page(0, false, HALF, 0);
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, DS_HRTF_SUBMIXES);
    pio(NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 5);
    pio(NV1BA0_PIO_SET_HRIR, 0x7F);             /* left tap 0 only */
    v = voice(7, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
    for (int b = 0; b < 4; b++) {
        v.vol[b] = 0;
    }
    v.hrtf = 5;
    start(&v);
    frames(4);
    bins = true;
    const float ear = 0.5f * TAP_7F;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        bins &= fabsf(g_mixbins[6][i] - ear) < 1e-3f && fabsf(g_mixbins[8][i] - ear) < 1e-3f &&
                g_mixbins[7][i] == 0.0f && g_mixbins[9][i] == 0.0f;
    }
    check(bins, "full-DSP monitor: a mono 3D voice's left ear in the left 3D bins, its right ear in the right");
    apu_down();
}

/* A 3D voice that played through a delayed HRTF entry, started again on
 * silence: silence from the first frame (its previous sound's tail is
 * cleared at VOICE_ON) */
static void test_voice_reuse_hrtf_history(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    pcm_page(1, false, 0, 0);
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, 0 | 1 << 8 | 2 << 16 | 3 << 24);
    pio(NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 9);
    pio(NV1BA0_PIO_SET_HRIR_X, 127 | 127 << 8);     /* tap 30 both ears: a 30-sample delay */
    Voice v = voice(8, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP, false, 0);
    v.vol[0] = v.vol[1] = 0;
    v.hrtf = 9;
    start(&v);
    frames(60);
    const bool sounding = g_out[NUM_SAMPLES_PER_FRAME - 1][0] > 1000;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;                /* the list again empty: started anew */
    v.page = 1;                                     /* on silence */
    start(&v);
    frames(1);
    check(sounding && all(0, 0, 0) && all(1, 0, 0), "reused 3D voice: its new sound starts from silence");
    apu_down();
}

/* Twelve voices (eligible, a centre send, 3D): one worker and four give the
 * same output, to the rounding of the sums' order */
static void twelve_voices(unsigned workers, int16_t out[NUM_SAMPLES_PER_FRAME][2])
{
    apu_up(false, true, workers);
    for (uint32_t p = 0; p < 12; p++) {
        pcm_page(p, p % 3 == 1, (int16_t)(1000 + 700 * p), (int16_t)(-300 * p));
    }
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, 0 | 1 << 8 | 2 << 16 | 3 << 24);
    for (uint16_t k = 0; k < 12; k++) {
        const bool three_d = k % 4 == 3;
        Voice v = voice(three_d ? k : 100 + k,
                        three_d ? NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP : NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP,
                        k % 3 == 1, k);
        v.vol[0] = (uint16_t)(0x40 * k);
        v.vol[1] = (uint16_t)(0x300 - 0x30 * k);
        if (k % 5 == 2) {
            v.vol[2] = 0x100;   /* a centre send: downmixed */
        }
        start(&v);
    }
    frames(4);
    memcpy(out, g_out, sizeof(g_out));
    apu_down();
}

static void *idle_race_frame(void *opaque)
{
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    mcpx_apu_vp_frame(opaque, bins);
    return NULL;
}

/* DirectSound may lock an idle voice after the list walk and release its
 * software object before the next report. Reporting the old candidate then
 * makes its ISR dereference the freed object (DOA3's voice table). */
static void test_idle_voice_locked_after_walk(void)
{
    apu_up(false, true, 1);
    pcm_page(0, false, HALF, 0);
    Voice later = voice(5, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    Voice idle = voice(4, NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP, false, 0);
    start(&later);
    start(&idle);
    d->regs[NV_PAPU_FETFORCE1] |= NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
    pio(NV1BA0_PIO_VOICE_OFF, idle.handle);

    apu_spin_lock(&d->vp.voice_spinlocks[later.handle]);
    pthread_t frame;
    pthread_create(&frame, NULL, idle_race_frame, d);
    const int64_t deadline = apu_host_us() + 1000000;
    while (apu_ld(&d->regs[NV_PAPU_CVL2D]) != later.handle && apu_host_us() < deadline) {
        usleep(100);
    }
    const bool walked = apu_ld(&d->regs[NV_PAPU_CVL2D]) == later.handle;
    check(walked, "idle race: list walk reached the later voice");
    pio(NV1BA0_PIO_SET_CURRENT_VOICE, idle.handle);
    pio(NV1BA0_PIO_VOICE_LOCK, 1);
    apu_spin_unlock(&d->vp.voice_spinlocks[later.handle]);
    pthread_join(frame, NULL);
    check((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) != NV_PAPU_FECTL_FEMETHMODE_TRAPPED,
          "idle voice locked after the walk: no stale release trap");

    pio(NV1BA0_PIO_VOICE_LOCK, 0);
    d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    mcpx_apu_vp_frame(d, bins);
    check((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == NV_PAPU_FECTL_FEMETHMODE_TRAPPED &&
          d->regs[NV_PAPU_FEDECMETH] == SE2FE_IDLE_VOICE && d->regs[NV_PAPU_FEDECPARAM] == idle.handle,
          "idle voice unlocked again: its valid release trap is delivered");
    apu_down();
}

static void test_workers(void)
{
    int16_t one[NUM_SAMPLES_PER_FRAME][2], four[NUM_SAMPLES_PER_FRAME][2];
    twelve_voices(1, one);
    twelve_voices(4, four);
    int worst = 0;
    bool sound = false;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        for (int ch = 0; ch < 2; ch++) {
            worst = MAX(worst, abs(one[i][ch] - four[i][ch]));
            sound |= one[i][ch] != 0;
        }
    }
    printf("  workers: largest difference %d (of 12 voices)\n", worst);
    check(sound && worst <= 12, "one worker and four: the same output (within a step a voice)");
}

/* `apu-stereo-smoke bench`: a fixed workload's throughput (not a test, and
 * not a Nova figure): 64 ordinary 2D voices, 32 with a centre send, 32 3D,
 * one worker, 3000 frames */
static int bench(void)
{
    apu_up(false, true, 1);
    for (uint32_t p = 0; p < PCM_PAGES; p++) {
        pcm_page(p, p % 2, (int16_t)(500 + 97 * p), (int16_t)(-200 - 31 * p));
    }
    pio(NV1BA0_PIO_SET_HRTF_SUBMIXES, 0 | 1 << 8 | 2 << 16 | 3 << 24);
    for (uint16_t k = 0; k < 128; k++) {
        const bool three_d = k >= 96;
        Voice v = voice(three_d ? k - 96 : 64 + k,
                        three_d ? NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP : NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP,
                        k % 2, k % PCM_PAGES);
        v.vol[0] = (uint16_t)(0x100 + 3 * k);
        v.vol[1] = (uint16_t)(0x200 - k);
        if (k >= 64 && k < 96) {
            v.vol[2] = 0x180;
        }
        start(&v);
    }
    frames(100);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    frames(3000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("bench: 128 voices, 3000 frames in %.3f s: %.0f frames/s (%.1fx real time), %.2f us a frame\n", s,
           3000 / s, 3000 / s / 1500.0, s / 3000 * 1e6);
    apu_down();
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(30);
    g_ram = calloc(1, RAM_SIZE);
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        return bench();
    }
    test_2d_mono_panned();
    test_2d_stereo();
    test_2d_headroom();
    test_2d_downmix();
    test_2d_route_kept();
    test_2d_route_saved();
    test_lfo_saved();
    test_3d();
    test_multipass();
    test_full_dsp();
    test_voice_reuse_hrtf_history();
    test_workers();
    test_idle_voice_locked_after_walk();
    free(g_ram);
    return failures ? 1 : 0;
}
