/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the VP's own stereo mix (MON_VP, the default without
 * the DSP) for 2D and 3D voices: a voice routed to the speakers is mixed
 * into the left and right speakers by its sends to the bins the guest's DSP
 * image puts on the speakers, each with its own gain, as that image does
 * for a stereo console: front left and right to their own speaker, the
 * centre to both at -3 dB, a back bin to its own side at -3 dB (the EP's
 * downmix), and the 3D bins (DirectSound's crosstalk bins, where the 3D
 * voices' HRTF submixes go) to the speaker bins of the same place, the
 * front pair at 0 dB, the back pair at -3 dB (the GP's crosstalk stage,
 * then the downmix). The LFE bin and the bins that aren't speakers (I3DL2
 * and the effect sends, the multipass bin) are the guest DSP program's,
 * whose effects this mix doesn't produce: they are left out of it.
 *
 * The route is a sound's, chosen once (vp.c): at its first frame with an
 * audible send, the speakers if one of them goes to a speaker bin, else
 * the monitor's existing mix (a voice sending only to the LFE or effect
 * bins, which would otherwise go silent), until its next VOICE_ON. Not
 * chosen again as its volumes change: a dry send fading to mute beside an
 * effect send fades the voice out, rather than switching it to the
 * monitor's mix with the effect send's level, and a dry send fading in
 * beside an effect send stays in the monitor's mix. Multipass groups keep
 * the monitor's mix.
 *
 * Bins 0 to 5 are DirectSound's speaker conventions (DSMIXBIN_FRONT_LEFT to
 * DSMIXBIN_BACK_RIGHT; Cxbx-Reloaded's XbDSoundTypes.h), not a rule of the
 * hardware's. The weights are the ones DirectSound's own DSP image uses for
 * a stereo console, measured in Halo (2026-10-30, with the full DSP): its
 * EP downmixes the speaker bins for the analog output by a 5x2 table in
 * its Y memory (Y:0000-0009, in AC-3's order L, C, R, Ls, Rs: 1 and 0,
 * 0.7071 and 0.7071, 0 and 1, 0.7071 and 0, 0 and 0.7071; no LFE), and
 * tones through each bin came out at 0 and -3.01 dB alike at 100 Hz, 1 kHz
 * and 8 kHz. Bins 6 to 9 the same way: the GP put a tone in bin 6, 7, 8 or
 * 9 into bin 0, 1, 4 or 5 at -0.03 dB, with a crosstalk-cancelling term on
 * the other side at -24.4 dB (at 1 kHz) that this mix leaves out. What the
 * hardware's analog stage and level do isn't measured.
 *
 * A 3D voice sends its left ear through slots 0–1 and its right ear through
 * slots 2–3 (xemu #3105). Those four go to the bins
 * the guest names with SET_HRTF_SUBMIXES, and DirectSound names them in the
 * order 6, 8, 7, 9: 3D front left, back left, front right, back right
 * (0x09070806 in both Dead or Alive 3 and The Thing), so each ear reaches
 * its own side's front and back bins. Taken as the even slots for the left
 * ear, the left ear went to front left and front right and the right ear
 * to both back bins: every 3D sound came out the same on both speakers.
 * Its destination is the bin it names; its headroom the shift the VP's
 * mix-bin path gives that send (vp.c).
 */
#ifndef NATIVE_DEV_APU_VP_STEREO_ROUTE_H
#define NATIVE_DEV_APU_VP_STEREO_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "native/dev/apu/apu_regs.h"

#define VP_BIN_FRONT_LEFT   0   /* DSMIXBIN_FRONT_LEFT */
#define VP_BIN_FRONT_RIGHT  1   /* DSMIXBIN_FRONT_RIGHT */
#define VP_BIN_FRONT_CENTER 2   /* DSMIXBIN_FRONT_CENTER */
#define VP_BIN_LFE          3   /* DSMIXBIN_LOW_FREQUENCY */
#define VP_BIN_BACK_LEFT    4   /* DSMIXBIN_BACK_LEFT */
#define VP_BIN_BACK_RIGHT   5   /* DSMIXBIN_BACK_RIGHT */
#define VP_BIN_3D_FRONT_LEFT  6 /* DirectSound's crosstalk bins (XTLK), the 3D voices' */
#define VP_BIN_3D_FRONT_RIGHT 7
#define VP_BIN_3D_BACK_LEFT   8
#define VP_BIN_3D_BACK_RIGHT  9
#define VP_SENDS            8
#define VP_DOWNMIX_SIDE     0.70710678f     /* -3 dB */

/* Sources: two voice channels, two HRTF ears and the I3DL2 room path. */
#define VP_SOURCES 5
#define VP_SOURCE_EARS 2
#define VP_SOURCE_ROOM 4

/* Route 3D sends 0–3 to the HRTF ears and I3DL2 sends 5 and 7 to the room.
 * Other sends select a voice channel by slot modulo channel count. */
static inline unsigned vp_send_channel(int slot, bool three_d, bool room, unsigned channels)
{
    if (three_d && slot < 4) {
        return VP_SOURCE_EARS + ((unsigned)slot >> 1);
    }
    if (room && (slot == 5 || slot == 7)) {
        return VP_SOURCE_ROOM;
    }
    return (unsigned)slot % channels;
}

/* A 2D voice's route in the VP's own mix, a sound's (vp.c) */
enum { VP_ROUTE_UNDECIDED = 0, VP_ROUTE_SPEAKERS = 1, VP_ROUTE_MONITOR = 2 };

/* A bin's share of the left and right speakers in the stereo downmix;
 * false for a bin that isn't heard on them (the LFE, and every bin past the
 * 3D ones) */
static inline bool vp_downmix_weights(int bin, float w[2])
{
    switch (bin) {
    case VP_BIN_FRONT_LEFT:
    case VP_BIN_3D_FRONT_LEFT:
        w[0] = 1.0f, w[1] = 0.0f;
        return true;
    case VP_BIN_FRONT_RIGHT:
    case VP_BIN_3D_FRONT_RIGHT:
        w[0] = 0.0f, w[1] = 1.0f;
        return true;
    case VP_BIN_FRONT_CENTER:
        w[0] = w[1] = VP_DOWNMIX_SIDE;
        return true;
    case VP_BIN_BACK_LEFT:
    case VP_BIN_3D_BACK_LEFT:
        w[0] = VP_DOWNMIX_SIDE, w[1] = 0.0f;
        return true;
    case VP_BIN_BACK_RIGHT:
    case VP_BIN_3D_BACK_RIGHT:
        w[0] = 0.0f, w[1] = VP_DOWNMIX_SIDE;
        return true;
    default:
        return false;
    }
}

/* The route a voice's sends ask for now: the speakers with an audible send
 * to a speaker bin, the monitor's mix with audible sends to none, undecided
 * with every send muted. att[] is the sends' attenuate(volume), 0 for a
 * muted send (a muted send may go anywhere). */
static inline int vp_stereo_classify(const int bin[VP_SENDS], const float att[VP_SENDS])
{
    int route = VP_ROUTE_UNDECIDED;
    float w[2];
    for (int b = 0; b < VP_SENDS; b++) {
        if (att[b] != 0.0f) {
            if (vp_downmix_weights(bin[b], w)) {
                return VP_ROUTE_SPEAKERS;
            }
            route = VP_ROUTE_MONITOR;
        }
    }
    return route;
}

/* The gains of a speaker-routed voice's sends to the two speakers: g[o][s]
 * from source s (vp_send_channel) to output o (0 left, 1 right), each
 * speaker send's ea * attenuation / 2^shift[send] (its headroom) added in by
 * its downmix weights; every gain 0 when its speaker sends are muted.
 * channels: the voice's; three_d: a 3D voice, its first four sends its
 * ears'; room: its I3DL2 room path on sends 5 and 7. */
static inline void vp_stereo_gains(const int bin[VP_SENDS], const float att[VP_SENDS],
                                   const uint8_t shift[VP_SENDS], unsigned channels, bool three_d,
                                   bool room, float ea, float g[2][VP_SOURCES])
{
    float w[2];
    for (int s = 0; s < VP_SOURCES; s++) {
        g[0][s] = g[1][s] = 0.0f;
    }
    for (int b = 0; b < VP_SENDS; b++) {
        if (att[b] == 0.0f || !vp_downmix_weights(bin[b], w)) {
            continue;
        }
        const float a = ea * att[b] / (float)(1u << shift[b]);
        const unsigned src = vp_send_channel(b, three_d, room, channels);
        g[0][src] += a * w[0];
        g[1][src] += a * w[1];
    }
}

/* The voice's frame into the speakers by its gains (vp_stereo_gains), added
 * to out: a mono voice's one channel, a stereo voice's two, a 3D voice's
 * ears and room path (ears, room: NULL without). The gains move from
 * the last frame's (from) to this frame's (to) across the frame, sample i
 * i/32 of the way, as the routes' gains do in the mix bins (without
 * their rounding to 1/16384). */
static inline void vp_stereo_mix_ramp(float out[NUM_SAMPLES_PER_FRAME][2],
                                      const float in[NUM_SAMPLES_PER_FRAME][2],
                                      const float (*ears)[2], const float *room,
                                      const float from[2][VP_SOURCES],
                                      const float to[2][VP_SOURCES], unsigned channels)
{
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        const float t = i / (float)NUM_SAMPLES_PER_FRAME;
        float g[2][VP_SOURCES];
        for (int o = 0; o < 2; o++) {
            for (int s = 0; s < VP_SOURCES; s++) {
                g[o][s] = from[o][s] + (to[o][s] - from[o][s]) * t;
            }
        }
        float l = g[0][0] * in[i][0], r = g[1][0] * in[i][0];
        if (channels == 2) {
            l += g[0][1] * in[i][1];
            r += g[1][1] * in[i][1];
        }
        if (ears) {
            l += g[0][2] * ears[i][0] + g[0][3] * ears[i][1];
            r += g[1][2] * ears[i][0] + g[1][3] * ears[i][1];
        }
        if (room) {
            l += g[0][VP_SOURCE_ROOM] * room[i];
            r += g[1][VP_SOURCE_ROOM] * room[i];
        }
        out[i][0] += l;
        out[i][1] += r;
    }
}

/* Mix with constant gains. */
static inline void vp_stereo_mix(float out[NUM_SAMPLES_PER_FRAME][2],
                                 const float in[NUM_SAMPLES_PER_FRAME][2],
                                 const float (*ears)[2], const float *room,
                                 const float g[2][VP_SOURCES], unsigned channels)
{
    vp_stereo_mix_ramp(out, in, ears, room, g, g, channels);
}

#endif
