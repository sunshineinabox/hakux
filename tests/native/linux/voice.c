/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The libretro core's Communicator (native/libretro/mic.c and audio.c) with
 * the real device (native/dev/xblc.c), the title's USB frames played here:
 * RetroArch's microphone opens only once the title reads the Communicator's
 * microphone (not for the headphone alone), at the title's rate; what it
 * hears reaches the title's microphone packets (resampled when RetroArch
 * gives another rate), silence without one or where its reads come up short;
 * it closes when the title stops and when the core stops. The
 * headphone's samples are mixed into the core's output 3 dB down in both
 * channels, none lost when the frontend takes part of a period, and without
 * a Communicator the output is the APU's as it was. No guest, renderer or
 * frontend: RetroArch's environment and microphone are stubbed. */
#include "native/dev/apu/apu_int.h"
#include "native/dev/usb.h"
#include "native/libretro/libretro_int.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

/* RetroArch's microphone, hearing a constant. */
static struct {
    bool have;                  /* RetroArch has the interface */
    unsigned give_rate;         /* the rate it opens at (0: the one asked for) */
    unsigned opened, closed, rate;
    bool on;
    bool pending;               /* its reads give nothing yet (0) */
    bool half;                  /* its reads give half of what's asked */
    int16_t value;
    size_t asked;               /* samples read, in all */
} g_fake;

static retro_microphone_t *open_mic(const retro_microphone_params_t *p)
{
    g_fake.opened++;
    g_fake.rate = p ? p->rate : 0;
    return (retro_microphone_t *)&g_fake;
}

static void close_mic(retro_microphone_t *m)
{
    g_fake.closed++;
    g_fake.on = false;
}

static bool get_params(const retro_microphone_t *m, retro_microphone_params_t *p)
{
    p->rate = g_fake.give_rate ? g_fake.give_rate : g_fake.rate;
    return true;
}

static bool set_state(retro_microphone_t *m, bool on)
{
    g_fake.on = on;
    return true;
}

static bool get_state(const retro_microphone_t *m)
{
    return g_fake.on;
}

static int read_mic(retro_microphone_t *m, int16_t *s, size_t n)
{
    if (!g_fake.on) {
        return -1;
    }
    g_fake.asked += n;
    n = g_fake.pending ? 0 : g_fake.half ? n / 2 : n;
    for (size_t i = 0; i < n; i++) {
        s[i] = g_fake.value;
    }
    return (int)n;
}

bool nc_lr_env(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE && g_fake.have) {
        struct retro_microphone_interface *i = data;
        if (i->interface_version != RETRO_MICROPHONE_INTERFACE_VERSION) {
            return false;
        }
        i->open_mic = open_mic;
        i->close_mic = close_mic;
        i->get_params = get_params;
        i->set_mic_state = set_state;
        i->get_mic_state = get_state;
        i->read_mic = read_mic;
        return true;
    }
    return false;
}

static NcUsbDev *g_c;
static bool g_phone_only;       /* the title's frames ask nothing of the microphone */

/* What the title's microphone packets carried since the last count. */
static struct {
    unsigned samples, value, zero, other, empty;
} g_in;

/* The title's 1 ms USB frames: 16 samples of `out` to the headphone, a
 * 16-sample packet asked of the microphone. */
static void title_frames(unsigned n, int16_t out)
{
    for (unsigned f = 0; f < n; f++) {
        uint8_t pkt[32];
        for (int i = 0; i < 16; i++) {
            nc_wr16(pkt + 2 * i, (uint16_t)out);
        }
        g_c->data(g_c, false, 4, pkt, 32);
        if (g_phone_only) {
            continue;
        }
        const int r = g_c->data(g_c, true, 5, pkt, 32);
        g_in.empty += r == 0;
        for (int i = 0; i + 1 < r; i += 2) {
            const int16_t v = (int16_t)nc_rd16(pkt + i);
            g_in.samples++;
            g_in.value += v == g_fake.value;
            g_in.zero += v == 0;
            g_in.other += v != 0 && v != g_fake.value;
        }
    }
}

/* retro_runs in real time (the microphone is read by the time gone), each
 * with its 16 ms of the title's frames. */
static void runs(unsigned n, int16_t out)
{
    for (unsigned i = 0; i < n; i++) {
        const struct timespec ts = {0, 16000000};
        nanosleep(&ts, NULL);
        nc_lr_mic_run();
        title_frames(16, out);
    }
}

/* The core's output, as the frontend took it. */
static struct {
    size_t limit;               /* frames a retro_run takes at most (then none) */
    bool full;                  /* this retro_run's limit reached */
    unsigned frames, voiced, odd;
    long long voice;            /* the headphone's part, summed */
} g_got;

static size_t batch(const int16_t *pcm, size_t frames)
{
    if (g_got.full) {
        return 0;
    }
    const size_t n = frames < g_got.limit ? frames : g_got.limit;
    g_got.full = n == g_got.limit;
    for (size_t i = 0; i < n; i++) {
        const int l = pcm[2 * i] - 1000, r = pcm[2 * i + 1] + 1000;
        g_got.frames++;
        g_got.voiced += l != 0;
        g_got.odd += l != r;    /* both channels get the same */
        g_got.voice += l;
    }
    return n;
}

static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;

/* retro_runs of the core's output: the APU's frames (1000, -1000) kept two
 * periods ahead. */
static void outputs(unsigned n, size_t limit)
{
    static int16_t apu[1024][2];
    for (unsigned i = 0; i < 1024; i++) {
        apu[i][0] = 1000;
        apu[i][1] = -1000;
    }
    for (unsigned k = 0; k < n; k++) {
        while (apu_out_queued() < 2 * 801 * 4) {
            apu_out_push(apu, 801 * 4);
        }
        g_got.limit = limit;
        g_got.full = false;
        nc_lr_audio_run(batch);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    nc_xblc_set_gate("off");                        /* the samples as they are */
    g_c = nc_xblc_new(0);
    g_fake.have = true;
    g_fake.value = 1234;

    /* Before the title moves audio (its menus): no microphone. */
    for (int i = 0; i < 10; i++) {
        nc_lr_mic_run();
    }
    check(g_fake.opened == 0, "the microphone stays closed until the title reads the Communicator's microphone");

    /* It plays to the headphone alone: still closed. */
    const uint8_t rate16k[8] = {0x41, 0x03, 2, 0, 0, 0, 0, 0};
    g_c->control(g_c, rate16k, NULL, 0);
    g_phone_only = true;
    runs(20, 0);
    g_phone_only = false;
    check(g_fake.opened == 0, "the headphone's packets alone don't open it");

    /* In its voice chat: open at the title's rate; after a lead of silence
     * the title's packets carry what it hears. */
    title_frames(1, 0);
    runs(40, 0);
    printf("  opened %u at %u Hz, read %zu samples; packets: %u samples (%u heard, %u silent, %u other), %u empty\n",
           g_fake.opened, g_fake.rate, g_fake.asked, g_in.samples, g_in.value, g_in.zero, g_in.other, g_in.empty);
    check(g_fake.opened == 1 && g_fake.rate == 16000 && g_fake.on, "opened once, at the title's 16 kHz");
    check(g_fake.asked > 40 * 16 * 16 * 8 / 10 && g_fake.asked < 40 * 16 * 16 * 2,
          "each retro_run reads the time since the last");
    check(g_in.value > 40 * 16 * 16 * 7 / 10 && g_in.other == 0 && g_in.zero <= 16 * 20 + 16,
          "the title's microphone packets carry what RetroArch's hears");

    /* The title stops (its menus again): closed after 2 s of retro_runs
     * (the first sees the last packets). */
    for (int i = 0; i < 120; i++) {
        nc_lr_mic_run();
    }
    check(g_fake.closed == 0, "still open 120 retro_runs after the title's last packets");
    nc_lr_mic_run();
    check(g_fake.closed == 1 && !g_fake.on, "closed at the next");

    /* Again, and RetroArch opens the microphone at 48 kHz: the device
     * resamples it to the title's. */
    g_fake.give_rate = 48000;
    memset(&g_in, 0, sizeof(g_in));
    title_frames(1, 0);
    runs(30, 0);
    printf("  opened %u; packets: %u samples (%u heard, %u silent, %u other)\n", g_fake.opened, g_in.samples,
           g_in.value, g_in.zero, g_in.other);
    check(g_fake.opened == 2 && g_in.value > 30 * 16 * 16 * 7 / 10 && g_in.other <= 2,
          "opened again as the title starts again; a 48 kHz microphone reaches it at 16 kHz");
    g_fake.give_rate = 0;

    /* The core stops: closed, and RetroArch asked again at the next boot. */
    nc_lr_mic_stop();
    check(g_fake.closed == 2, "closed as the core stops");

    /* No microphone in RetroArch: the title hears silence, in full packets. */
    g_fake.have = false;
    g_c->reset(g_c);                                /* a bus reset: the rings emptied */
    memset(&g_in, 0, sizeof(g_in));
    title_frames(1, 0);
    runs(30, 0);
    printf("  no microphone: packets: %u samples (%u silent), %u empty\n", g_in.samples, g_in.zero, g_in.empty);
    check(g_fake.opened == 2 && g_in.samples > 30 * 16 * 16 * 8 / 10 && g_in.zero == g_in.samples &&
          g_in.empty <= 2, "without a microphone the title hears silence");
    nc_lr_mic_stop();

    /* RetroArch's microphone opens but its reads give nothing yet (pending),
     * then half of what's asked: the rest is silence, the packets stay full. */
    g_fake.have = true;
    g_fake.pending = true;
    g_c->reset(g_c);
    memset(&g_in, 0, sizeof(g_in));
    title_frames(1, 0);
    runs(30, 0);
    printf("  pending microphone: packets: %u samples (%u silent), %u empty\n", g_in.samples, g_in.zero,
           g_in.empty);
    check(g_fake.opened == 3 && g_in.samples > 30 * 16 * 16 * 8 / 10 && g_in.zero == g_in.samples &&
          g_in.empty <= 2, "a microphone that gives nothing yet: silence in full packets");
    g_fake.pending = false;
    g_fake.half = true;
    memset(&g_in, 0, sizeof(g_in));
    runs(30, 0);
    printf("  half reads: packets: %u samples (%u heard, %u silent, %u other), %u empty\n", g_in.samples,
           g_in.value, g_in.zero, g_in.other, g_in.empty);
    check(g_in.samples > 30 * 16 * 16 * 8 / 10 && g_in.value > g_in.samples * 4 / 10 &&
          g_in.zero > g_in.samples * 4 / 10 && g_in.other == 0 && g_in.empty <= 2,
          "a microphone that gives half: half heard, half silence, the packets full");
    g_fake.half = false;
    nc_lr_mic_stop();

    /* The headphone: 60 ms of the title's 8000 into the core's output, 3 dB
     * down (5663) in both channels, then the APU's frames alone again. */
    const int out_frames = apu_out_open(1 << 16, 0, &g_wake);
    check(out_frames > 0, "the core's output opened");
    g_c->reset(g_c);
    title_frames(60, 8000);
    outputs(6, (size_t)-1);
    printf("  headphone: %u frames out, %u with its sound (%lld in all), %u channels apart\n", g_got.frames,
           g_got.voiced, g_got.voice, g_got.odd);
    const long long whole = g_got.voice;
    const unsigned voiced = g_got.voiced;
    check(voiced > 2700 && voiced < 3000 && g_got.odd == 0 && whole > (long long)voiced * 5600 &&
          whole <= (long long)voiced * 5663, "the title's headphone plays in both channels, 3 dB down");

    /* The frontend takes 300 frames a retro_run: the rest keep theirs (the
     * same sound, give or take the resampler's phase and step). */
    memset(&g_got, 0, sizeof(g_got));
    title_frames(60, 8000);
    outputs(16, 300);
    printf("  300 frames a retro_run: %u frames out, %u with its sound (%lld in all)\n", g_got.frames,
           g_got.voiced, g_got.voice);
    check(g_got.voiced + 8 >= voiced && g_got.voiced <= voiced + 8 && g_got.voice + 8 * 5663 >= whole &&
          g_got.voice <= whole + 8 * 5663 && g_got.odd == 0, "nothing lost when the frontend takes part of a period");

    /* Without a Communicator: the APU's frames exactly. */
    nc_usb_dev_free(g_c);
    g_c = NULL;
    memset(&g_got, 0, sizeof(g_got));
    outputs(3, (size_t)-1);
    check(g_got.frames > 2000 && g_got.voiced == 0 && g_got.odd == 0, "without one the output is the APU's");
    nc_lr_mic_run();
    check(g_fake.opened == 3, "and nothing opens the microphone");
    apu_out_close();
    return failures ? 1 : 0;
}
