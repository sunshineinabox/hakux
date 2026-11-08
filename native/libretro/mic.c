/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the Xbox Live Communicator's microphone in the
 * libretro core (the hakux_communicator option): RetroArch's microphone
 * (RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE) into player 1's Communicator
 * (native/dev/xblc.c), read each retro_run.
 *
 * The microphone is open only while the title reads the Communicator's
 * microphone (in a game's voice chat; not in its menus, nor in a game
 * without voice, nor for the headphone alone): opened as its packets start,
 * at the title's rate (RetroArch resamples to it, the device whatever still
 * differs), and closed once they stop for IDLE_RUNS, as the core stops
 * (nc_lr_mic_stop) and as the Communicator goes. Each retro_run feeds the
 * samples of the time since the last one, so the read doesn't wait on the
 * microphone; what it doesn't give (none, or not yet) is silence. With no
 * microphone (RetroArch has none, or none opens) the Communicator hears
 * silence.
 */
#include "native/libretro/libretro_int.h"
#include "native/dev/usb.h"

#include <time.h>

#define IDLE_RUNS 120           /* retro_runs without a packet (2 s): the title stopped */
#define MAX_RUN_MS 50           /* a longer gap (RetroArch's menu) reads this long */
#define LEAD_MS 20              /* silence first: a retro_run's samples come at once, the title takes 1 ms a frame */

static struct {
    struct retro_microphone_interface mic;  /* RetroArch's (open_mic NULL: none) */
    bool asked;                 /* for it, since the core started */
    retro_microphone_t *handle;
    unsigned rate;              /* what is read: the microphone's, else the title's (silence) */
    bool active;                /* the title reads the microphone */
    uint64_t packets;           /* its microphone packets, as last seen */
    unsigned idle;              /* retro_runs since they last grew */
    uint64_t last_ns;           /* the last read */
    uint64_t acc;               /* the samples' remainder, in 1/1e9 of one */
} g_mic;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void mic_close(void)
{
    if (g_mic.handle) {
        g_mic.mic.close_mic(g_mic.handle);
        g_mic.handle = NULL;
        nc_log("hakuX-libretro", "communicator: microphone closed");
    }
}

static void mic_open(int rate)
{
    if (!g_mic.asked) {
        g_mic.asked = true;
        struct retro_microphone_interface mic = {.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION};
        if (nc_lr_env(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &mic) &&
            mic.interface_version == RETRO_MICROPHONE_INTERFACE_VERSION && mic.open_mic && mic.close_mic &&
            mic.get_params && mic.set_mic_state && mic.read_mic) {
            g_mic.mic = mic;
        } else {
            nc_log("hakuX-libretro", "communicator: RetroArch has no microphone (Settings > Audio > Microphone): "
                   "it hears silence");
        }
    }
    g_mic.rate = (unsigned)rate;
    if (!g_mic.mic.open_mic) {
        return;
    }
    const retro_microphone_params_t want = {(unsigned)rate};
    g_mic.handle = g_mic.mic.open_mic(&want);
    if (!g_mic.handle) {
        nc_log("hakuX-libretro", "communicator: RetroArch opened no microphone: it hears silence");
        return;
    }
    retro_microphone_params_t got = want;
    if (!g_mic.mic.get_params(g_mic.handle, &got) || !got.rate) {
        got.rate = (unsigned)rate;
    }
    if (!g_mic.mic.set_mic_state(g_mic.handle, true)) {
        nc_log("hakuX-libretro", "communicator: RetroArch's microphone didn't start: it hears silence");
        mic_close();
        return;
    }
    g_mic.rate = got.rate;
    nc_log("hakuX-libretro", "communicator: microphone open, %u Hz (the title's %d)", got.rate, rate);
}

void nc_lr_mic_run(void)
{
    uint64_t packets;
    const int rate = nc_xblc_state(0, &packets);
    if (packets != g_mic.packets) {
        g_mic.packets = packets;
        g_mic.idle = 0;
    } else if (g_mic.idle < IDLE_RUNS) {
        g_mic.idle++;
    }
    const uint64_t now = now_ns();
    if (!rate || !packets || g_mic.idle >= IDLE_RUNS) {
        if (g_mic.active) {
            g_mic.active = false;
            nc_log("hakuX-libretro", "communicator: the title stopped reading the microphone");
            mic_close();
        }
        return;
    }
    static int16_t buf[4096];
    if (!g_mic.active) {
        g_mic.active = true;
        mic_open(rate);
        g_mic.last_ns = now;
        g_mic.acc = 0;
        const int lead = (int)(g_mic.rate * LEAD_MS / 1000);
        memset(buf, 0, sizeof(buf));
        nc_xblc_mic_put(0, buf, lead < 4096 ? lead : 4096, (int)g_mic.rate);
        return;
    }
    /* the time since the last read, in the microphone's samples */
    const uint64_t dt = MIN(now - g_mic.last_ns, (uint64_t)MAX_RUN_MS * 1000000u);
    g_mic.last_ns = now;
    g_mic.acc += dt * g_mic.rate;
    uint64_t n = g_mic.acc / 1000000000u;
    g_mic.acc -= n * 1000000000u;
    n = MIN(n, (uint64_t)(sizeof(buf) / sizeof(buf[0])));
    if (!n) {
        return;
    }
    /* none, failing, or a short read (pending, or not captured yet): the rest
     * silence, so the title's packets keep up with the time */
    int got = g_mic.handle ? g_mic.mic.read_mic(g_mic.handle, buf, (size_t)n) : -1;
    got = got < 0 ? 0 : MIN(got, (int)n);
    memset(buf + got, 0, ((size_t)n - (size_t)got) * sizeof(buf[0]));
    nc_xblc_mic_put(0, buf, (int)n, (int)g_mic.rate);
}

void nc_lr_mic_stop(void)
{
    mic_close();
    memset(&g_mic, 0, sizeof(g_mic));
}
