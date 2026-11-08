/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The VP's own stereo mix for 2D voices (native/dev/apu/vp/stereo_route.h,
 * which vp.c's voice_process calls): the route a voice's sends ask for, and
 * each send's gain into the speakers, the centre and back bins by the
 * stereo downmix (-3 dB), the LFE and effect bins left out. The route
 * chosen once a sound, the 3D, multipass and full-DSP cases, and the
 * dispatch through voice processing, are the native core's apu-stereo-smoke
 * (tests/native/linux/apu_stereo.c). */
#include "hle_test.h"

#include <math.h>

#include "native/dev/apu/vp/stereo_route.h"

#define N NUM_SAMPLES_PER_FRAME

/* the VP's attenuation (vp.c attenuation_init): 0xFFF is mute */
static float att_of(uint16_t vol)
{
    return vol == 0xFFF ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

typedef struct {
    int bin[VP_SENDS];
    uint16_t vol[VP_SENDS];
    uint8_t headroom[NUM_MIXBINS];
    unsigned channels;
    bool three_d;               /* mono; its first four sends its HRTF ears' */
    float ea;
} Voice;

/* every send muted, to bins 0..7 in turn */
static Voice voice(unsigned channels)
{
    Voice v;
    memset(&v, 0, sizeof(v));
    for (int b = 0; b < VP_SENDS; b++) {
        v.bin[b] = b;
        v.vol[b] = 0xFFF;
    }
    v.channels = channels;
    v.ea = 1.0f;
    return v;
}

static int classify(const Voice *v)
{
    float att[VP_SENDS];
    for (int b = 0; b < VP_SENDS; b++) {
        att[b] = att_of(v->vol[b]);
    }
    return vp_stereo_classify(v->bin, att);
}

/* the sends' headroom their bins' (a 2D voice's, as vp.c gives it) */
static void gains(const Voice *v, float g[2][VP_SOURCES])
{
    float att[VP_SENDS];
    uint8_t shift[VP_SENDS];
    for (int b = 0; b < VP_SENDS; b++) {
        att[b] = att_of(v->vol[b]);
        shift[b] = v->headroom[v->bin[b]];
    }
    vp_stereo_gains(v->bin, att, shift, v->channels, v->three_d, false, v->ea, g);
}

/* a frame of the voice's samples: channel 0 a ramp, channel 1 another (a
 * mono voice's channel 1 is channel 0's copy, as voice_get_samples makes it) */
static void source(float in[N][2], unsigned channels)
{
    for (int i = 0; i < N; i++) {
        in[i][0] = 0.25f + i / 64.0f;
        in[i][1] = channels == 2 ? -0.5f + i / 128.0f : in[i][0];
    }
}

/* a 3D voice's ears: source()'s two channels */
static void mix(const Voice *v, float out[N][2])
{
    float g[2][VP_SOURCES], in[N][2], ears[N][2];
    HT_CHECK(classify(v) == VP_ROUTE_SPEAKERS);
    gains(v, g);
    source(in, v->channels);
    source(ears, 2);
    memset(out, 0, sizeof(float[N][2]));
    vp_stereo_mix(out, in, v->three_d ? (const float (*)[2])ears : NULL, NULL, g, v->channels);
}

static void test_mono_left_only(void)
{
    Voice v = voice(1);
    v.vol[0] = 0;               /* slot 0 to bin 0 (front left), 0 dB */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 1);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == in[i][0]);
        HT_CHECK(out[i][1] == 0.0f);
    }
}

static void test_mono_right_only(void)
{
    Voice v = voice(1);
    v.vol[1] = 0;               /* slot 1 to bin 1 (front right) */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 1);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == 0.0f);
        HT_CHECK(out[i][1] == in[i][0]);
    }
}

static void test_mono_equal(void)
{
    Voice v = voice(1);
    v.vol[0] = v.vol[1] = 0;
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 1);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == out[i][1]);
        HT_CHECK(out[i][0] == in[i][0]);      /* each speaker's own send: not doubled */
    }
}

static void test_mono_unequal(void)
{
    Voice v = voice(1);
    v.vol[0] = 0x040;           /* -1 dB */
    v.vol[1] = 0x300;           /* -12 dB */
    float out[N][2];
    mix(&v, out);
    const float want = att_of(0x300) / att_of(0x040);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][1] / out[i][0] - want) < 1e-6f * want);
    }
}

static void test_stereo_standard(void)
{
    Voice v = voice(2);
    v.vol[0] = 0x080;           /* slot 0 (source left) to front left, -2 dB */
    v.vol[1] = 0x100;           /* slot 1 (source right) to front right, -4 dB */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 2);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][0] - att_of(0x080) * in[i][0]) < 1e-6f);
        HT_CHECK(fabsf(out[i][1] - att_of(0x100) * in[i][1]) < 1e-6f);
    }
}

/* destination by the bin a send names, source by its slot */
static void test_stereo_reordered(void)
{
    Voice v = voice(2);
    v.bin[0] = VP_BIN_FRONT_RIGHT;
    v.bin[1] = VP_BIN_FRONT_LEFT;
    v.vol[0] = 0x080;
    v.vol[1] = 0x100;
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 2);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][1] - att_of(0x080) * in[i][0]) < 1e-6f);
        HT_CHECK(fabsf(out[i][0] - att_of(0x100) * in[i][1]) < 1e-6f);
    }
}

static void test_headroom_per_bin(void)
{
    Voice v = voice(1);
    v.vol[0] = v.vol[1] = 0;
    v.headroom[0] = 1;          /* front left: / 2 */
    v.headroom[1] = 2;          /* front right: / 4 */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 1);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == in[i][0] / 2);
        HT_CHECK(out[i][1] == in[i][0] / 4);
    }
}

/* muted sends to any bin, the effect bins and the multipass bin among them,
 * change nothing */
static void test_muted_sends_ignored(void)
{
    Voice a = voice(2), b = voice(2);
    a.vol[0] = b.vol[0] = 0x080;
    a.vol[1] = b.vol[1] = 0x100;
    b.bin[2] = 2;               /* centre */
    b.bin[3] = 3;               /* LFE */
    b.bin[5] = 12;              /* an effect send */
    b.bin[7] = MULTIPASS_BIN;
    float oa[N][2], ob[N][2];
    mix(&a, oa);
    mix(&b, ob);
    HT_CHECK(memcmp(oa, ob, sizeof(oa)) == 0);
}

/* every send muted: undecided (the voice silent whichever mix it gets); the
 * amplitude envelope at 0: silence */
static void test_silent(void)
{
    Voice v = voice(2);
    HT_CHECK(classify(&v) == VP_ROUTE_UNDECIDED);
    float out[N][2];
    v.vol[0] = v.vol[1] = 0;
    v.ea = 0.0f;                /* the amplitude envelope at 0 */
    mix(&v, out);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == 0.0f && out[i][1] == 0.0f);
    }
}

/* two voices add, with no clipping of their own */
static void test_two_voices_add(void)
{
    Voice a = voice(1), b = voice(2);
    a.vol[0] = a.vol[1] = 0;
    b.vol[0] = b.vol[1] = 0;
    a.ea = b.ea = 1.0f;
    float ga[2][VP_SOURCES], gb[2][VP_SOURCES], ia[N][2], ib[N][2], both[N][2] = {{0}}, oa[N][2] = {{0}},
          ob[N][2] = {{0}};
    HT_CHECK(classify(&a) == VP_ROUTE_SPEAKERS && classify(&b) == VP_ROUTE_SPEAKERS);
    gains(&a, ga);
    gains(&b, gb);
    source(ia, 1);
    source(ib, 2);
    for (int i = 0; i < N; i++) {     /* past full scale together */
        ia[i][0] = ia[i][1] = 0.9f;
        ib[i][0] = 0.8f;
    }
    vp_stereo_mix(both, ia, NULL, NULL, ga, 1);
    vp_stereo_mix(both, ib, NULL, NULL, gb, 2);
    vp_stereo_mix(oa, ia, NULL, NULL, ga, 1);
    vp_stereo_mix(ob, ib, NULL, NULL, gb, 2);
    for (int i = 0; i < N; i++) {
        HT_CHECK(both[i][0] == oa[i][0] + ob[i][0]);
        HT_CHECK(both[i][1] == oa[i][1] + ob[i][1]);
    }
    HT_CHECK(both[0][0] > 1.0f);
}

/* the centre to both speakers at -3 dB: a centred sound's power kept
 * (L^2 + R^2 = in^2) */
static void test_centre(void)
{
    Voice v = voice(1);
    v.vol[2] = 0;               /* slot 2 to bin 2 (front centre), 0 dB */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 1);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == out[i][1]);
        HT_CHECK(fabsf(out[i][0] - VP_DOWNMIX_SIDE * in[i][0]) < 1e-6f);
        HT_CHECK(fabsf(out[i][0] * out[i][0] + out[i][1] * out[i][1] - in[i][0] * in[i][0]) < 1e-5f);
    }
}

/* each back bin to its own side at -3 dB, the other speaker exactly 0; a
 * stereo voice's slots 4 and 5 are its left and right source channels */
static void test_backs(void)
{
    Voice v = voice(2);
    v.vol[4] = 0;               /* slot 4 (source left) to bin 4 (back left) */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 2);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][0] - VP_DOWNMIX_SIDE * in[i][0]) < 1e-6f);
        HT_CHECK(out[i][1] == 0.0f);
    }
    v = voice(2);
    v.vol[5] = 0;               /* slot 5 (source right) to bin 5 (back right) */
    mix(&v, out);
    for (int i = 0; i < N; i++) {
        HT_CHECK(out[i][0] == 0.0f);
        HT_CHECK(fabsf(out[i][1] - VP_DOWNMIX_SIDE * in[i][1]) < 1e-6f);
    }
}

/* a sound panned right through the front-right and back-right bins stays
 * on the right; the speakers' sends add, each at its own weight and its
 * bin's headroom */
static void test_five_speakers(void)
{
    Voice v = voice(1);
    v.vol[0] = 0x300;           /* front left, -12 dB */
    v.vol[1] = 0;               /* front right, 0 dB */
    v.vol[2] = 0x180;           /* centre, -6 dB */
    v.vol[4] = 0xFFF;           /* back left muted */
    v.vol[5] = 0x0C0;           /* back right, -3 dB */
    v.headroom[5] = 1;          /* back right: / 2 */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 1);
    const float k = VP_DOWNMIX_SIDE;
    const float l = att_of(0x300) + k * att_of(0x180);
    const float r = 1.0f + k * att_of(0x180) + k * att_of(0x0C0) / 2;
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][0] - l * in[i][0]) < 1e-5f);
        HT_CHECK(fabsf(out[i][1] - r * in[i][0]) < 1e-5f);
        HT_CHECK(out[i][1] > out[i][0]);
    }
}

/* audible LFE, effect and multipass sends beside the speaker sends are left
 * out: the same gains as the speaker sends alone, whatever their level */
static void test_other_sends_left_out(void)
{
    static const int others[] = {VP_BIN_LFE, 10, 12, 20, MULTIPASS_BIN};
    for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++) {
        Voice a = voice(1), b = voice(1);
        a.vol[0] = b.vol[0] = 0x300;
        a.vol[1] = b.vol[1] = 0;
        b.bin[3] = others[k];
        b.vol[3] = 0;           /* louder than the left send */
        float ga[2][VP_SOURCES], gb[2][VP_SOURCES];
        HT_CHECK(classify(&a) == VP_ROUTE_SPEAKERS && classify(&b) == VP_ROUTE_SPEAKERS);
        gains(&a, ga);
        gains(&b, gb);
        HT_CHECK(memcmp(ga, gb, sizeof(ga)) == 0);
    }
}

/* a speaker-routed voice's dry send fading to mute beside a louder effect
 * send: its gains fall with the dry send, to exactly 0 at mute (the voice
 * keeps its route as the volume changes, vp.c, so none of this switches to
 * the effect send's level) */
static void test_gains_fade_to_mute(void)
{
    static const uint16_t steps[] = {0x000, 0x400, 0xC00, 0xFFE, 0xFFF};
    float last = INFINITY;
    for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
        Voice v = voice(1);
        v.vol[0] = steps[k];    /* front left */
        v.bin[2] = 10;          /* I3DL2, 0 dB */
        v.vol[2] = 0;
        float g[2][VP_SOURCES];
        gains(&v, g);
        HT_CHECK(g[0][0] < last && g[1][0] == 0.0f);
        HT_CHECK(fabsf(g[0][0] - att_of(steps[k])) < 1e-6f);
        last = g[0][0];
    }
    HT_CHECK(last == 0.0f);
}

/* DirectSound's HRTF submixes (SET_HRTF_SUBMIXES 0x09070806 in Dead or
 * Alive 3 and The Thing): the VP's HRTF outputs 0 to 3 into 3D front left,
 * back left, front right and back right */
static const int k_ds_hrtf_bins[4] = {VP_BIN_3D_FRONT_LEFT, VP_BIN_3D_BACK_LEFT,
                                      VP_BIN_3D_FRONT_RIGHT, VP_BIN_3D_BACK_RIGHT};

static Voice voice_3d(void)
{
    Voice v = voice(1);         /* a 3D voice plays mono; its ears are mix()'s */
    v.three_d = true;
    for (int b = 0; b < 4; b++) {
        v.bin[b] = k_ds_hrtf_bins[b];
    }
    return v;
}

/* A 3D voice's sends to the 3D bins (its HRTF submixes): its two ears the
 * sources, the left ear the first two slots and the right ear the last
 * two; the front pair to its own speaker at 0 dB, the back pair at -3 dB
 * (the GP's crosstalk stage, then the EP's downmix, as measured), each send
 * at its own headroom */
static void test_3d_bins(void)
{
    Voice v = voice_3d();
    v.vol[0] = 0;               /* left ear, 3D front left, 0 dB */
    v.vol[2] = 0x300;           /* right ear, 3D front right, -12 dB */
    v.vol[1] = 0;               /* left ear, 3D back left, 0 dB */
    float out[N][2], in[N][2];
    mix(&v, out);
    source(in, 2);
    const float l = 1.0f + VP_DOWNMIX_SIDE, r = att_of(0x300);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][0] - l * in[i][0]) < 1e-5f);
        HT_CHECK(fabsf(out[i][1] - r * in[i][1]) < 1e-5f);
    }
    /* every HRTF send at 0 dB: each ear on its own side only (the even
     * slots taken as the left ear put the same mix on both speakers) */
    Voice all4 = voice_3d();
    for (int b = 0; b < 4; b++) {
        all4.vol[b] = 0;
    }
    mix(&all4, out);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][0] - l * in[i][0]) < 1e-5f);
        HT_CHECK(fabsf(out[i][1] - l * in[i][1]) < 1e-5f);
    }
    /* behind the listener only: 3 dB under the same sound in front */
    Voice back = voice_3d(), front = voice_3d();
    front.vol[0] = front.vol[2] = 0;
    back.vol[1] = back.vol[3] = 0;
    float gf[2][VP_SOURCES], gb[2][VP_SOURCES];
    HT_CHECK(classify(&back) == VP_ROUTE_SPEAKERS);
    gains(&front, gf);
    gains(&back, gb);
    const int el = VP_SOURCE_EARS, er = VP_SOURCE_EARS + 1;
    HT_CHECK(fabsf(gb[0][el] / gf[0][el] - VP_DOWNMIX_SIDE) < 1e-6f &&
             fabsf(gb[1][er] / gf[1][er] - VP_DOWNMIX_SIDE) < 1e-6f);
    HT_CHECK(gb[0][er] == 0.0f && gb[1][el] == 0.0f);
    HT_CHECK(gb[0][0] == 0.0f && gb[1][0] == 0.0f);     /* the dry voice not at all */
    /* sends 4-7 carry the voice itself, not its ears (measured on hardware) */
    Voice dry = voice_3d();
    dry.bin[4] = VP_BIN_FRONT_LEFT;
    dry.vol[4] = 0;
    mix(&dry, out);
    float mono[N][2];
    source(mono, 1);
    for (int i = 0; i < N; i++) {
        HT_CHECK(fabsf(out[i][0] - mono[i][0]) < 1e-6f && out[i][1] == 0.0f);
    }
    /* with its I3DL2 filter, sends 5 and 7 carry the room path */
    HT_EQ_INT(vp_send_channel(5, true, true, 1), VP_SOURCE_ROOM);
    HT_EQ_INT(vp_send_channel(7, true, true, 1), VP_SOURCE_ROOM);
    HT_EQ_INT(vp_send_channel(6, true, true, 1), 0);
    HT_EQ_INT(vp_send_channel(5, true, false, 1), 0);
    HT_EQ_INT(vp_send_channel(1, true, true, 1), VP_SOURCE_EARS);
}

/* only LFE or non-speaker sends audible: the monitor's mix */
static void test_no_speaker_send(void)
{
    static const int others[] = {VP_BIN_LFE, 10, 12, 20, MULTIPASS_BIN};
    for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++) {
        Voice v = voice(1);
        v.bin[4] = others[k];
        v.vol[4] = 0x200;
        HT_CHECK(classify(&v) == VP_ROUTE_MONITOR);
    }
}

int main(void)
{
    HT_RUN(test_mono_left_only);
    HT_RUN(test_mono_right_only);
    HT_RUN(test_mono_equal);
    HT_RUN(test_mono_unequal);
    HT_RUN(test_stereo_standard);
    HT_RUN(test_stereo_reordered);
    HT_RUN(test_headroom_per_bin);
    HT_RUN(test_muted_sends_ignored);
    HT_RUN(test_silent);
    HT_RUN(test_two_voices_add);
    HT_RUN(test_centre);
    HT_RUN(test_backs);
    HT_RUN(test_five_speakers);
    HT_RUN(test_other_sends_left_out);
    HT_RUN(test_gains_fade_to_mute);
    HT_RUN(test_3d_bins);
    HT_RUN(test_no_speaker_send);
    HT_DONE();
}
