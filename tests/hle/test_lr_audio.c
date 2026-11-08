/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The libretro core's audio output (native/libretro/audio.c) against a
 * frontend that takes all, part or none of what it is offered: the frames it
 * takes leave the ring, the rest are offered again first, in order, nothing
 * lost, repeated or overwritten. Stereo pairs are numbered (left n, right
 * -n - 1) so any loss, repeat or swap shows. Only the frontend, the core's
 * log and the Communicator (none) are stand-ins. */
#include "hle_test.h"

#include <stdarg.h>

#include "native/dev/apu/apu_int.h"
#include "native/dev/usb.h"
#include "native/libretro/libretro_int.h"

void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

/* No Communicator: the headphone has no samples to mix in. */
int nc_xblc_phone_read(unsigned player, int16_t *out, int frames, int rate)
{
    (void)player;
    (void)out;
    (void)frames;
    (void)rate;
    return 0;
}

#define FRAME_BYTES 4
#define EP_FRAMES 256           /* the EP's push: 1024 bytes */

static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static int g_cap;               /* the ring's capacity, bytes */
static uint32_t g_next_push;    /* the producer's next pair number */
static uint32_t g_next_got;     /* the pair the frontend expects next */
static uint32_t g_got, g_offered, g_calls, g_bad;

/* how much of an offer the frontend takes, call by call (-1: all, -2:
 * more than offered) */
static const int *g_plan;
static unsigned g_plan_len, g_plan_at;

static size_t frontend(const int16_t *pcm, size_t frames)
{
    g_calls++;
    g_offered += (uint32_t)frames;
    int take = g_plan_at < g_plan_len ? g_plan[g_plan_at++] : -1;
    size_t n = take == -1 ? frames : take == -2 ? frames + 100 : MIN((size_t)take, frames);
    const size_t check = MIN(n, frames);
    for (size_t i = 0; i < check; i++) {
        const int16_t l = (int16_t)(g_next_got & 0x7FFF), r = (int16_t)(-1 - (int16_t)(g_next_got & 0x7FFF));
        g_bad += pcm[2 * i] != l || pcm[2 * i + 1] != r;
        g_next_got++;
    }
    g_got += (uint32_t)check;
    return n;
}

static void plan(const int *p, unsigned n)
{
    g_plan = p;
    g_plan_len = n;
    g_plan_at = 0;
}

/* the producer as the frame thread is: an EP frame at a time, only into
 * room it has (its high watermark keeps it well below the capacity) */
static bool push_frame(void)
{
    if (apu_out_queued() + EP_FRAMES * FRAME_BYTES > g_cap) {
        return false;
    }
    int16_t pcm[EP_FRAMES][2];
    for (int i = 0; i < EP_FRAMES; i++) {
        pcm[i][0] = (int16_t)(g_next_push & 0x7FFF);
        pcm[i][1] = (int16_t)(-1 - (int16_t)(g_next_push & 0x7FFF));
        g_next_push++;
    }
    apu_out_push(pcm, sizeof(pcm));
    return true;
}


static void open_ring(int ep_frames)
{
    g_cap = ep_frames * EP_FRAMES * FRAME_BYTES;
    apu_out_open(g_cap, 512, &g_wake);
    g_next_push = g_next_got = g_got = g_offered = g_calls = g_bad = 0;
    plan(NULL, 0);
}

static void fill(void)
{
    while (push_frame()) {
    }
}

static uint32_t queued_frames(void)
{
    return (uint32_t)apu_out_queued() / FRAME_BYTES;
}

/* 1: all taken, a period a call (800 or 801 at 60000/1001 Hz) */
static void test_full(void)
{
    open_ring(48);
    fill();
    const uint32_t before = queued_frames();
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_got, 800);
    HT_EQ_INT(queued_frames(), before - 800);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_got, 1601);
    HT_EQ_INT(g_bad, 0);
    apu_out_close();
}

/* 2: none taken: the ring as it was, the next call from the same pair */
static void test_zero(void)
{
    open_ring(48);
    fill();
    const uint32_t before = queued_frames();
    static const int none[] = {0};
    plan(none, 1);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_got, 0);
    HT_EQ_INT(queued_frames(), before);
    nc_lr_audio_run(frontend);  /* takes all from here */
    HT_EQ_INT(g_got, 801);
    HT_EQ_INT(g_bad, 0);
    HT_EQ_INT(queued_frames(), before - 801);
    apu_out_close();
}

/* 3: 300 taken, then none: 300 leave; the next call starts at pair 300 */
static void test_partial_then_zero(void)
{
    open_ring(48);
    fill();
    const uint32_t before = queued_frames();
    static const int p[] = {300, 0};
    plan(p, 2);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_got, 300);
    HT_EQ_INT(queued_frames(), before - 300);
    HT_EQ_INT(g_calls, 2);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_next_got, 300 + 801);
    HT_EQ_INT(g_bad, 0);
    apu_out_close();
}

/* 4: small takes over many calls, the ring topped up between them: every
 * pair once, in order */
static void test_repeated_partial(void)
{
    open_ring(48);
    static const int p[] = {1, 31, 32, 33, 0, 256, 7, 0, 0, 500, -1, 3, 0};
    for (int call = 0; call < 200; call++) {
        fill();
        plan(p + call % 13, 13 - call % 13);
        nc_lr_audio_run(frontend);
    }
    HT_EQ_INT(g_bad, 0);
    HT_EQ_INT(g_got, g_next_got);
    HT_EQ_INT(g_next_push - g_got, queued_frames());
    apu_out_close();
}

/* 5: a small ring that wraps, the producer appending only into room, the
 * frontend backing up: order kept, nothing overwritten (apu_out_push
 * asserts its room) */
static void test_wrap_and_backpressure(void)
{
    open_ring(5);               /* 1280 pairs: wraps every second call */
    static const int p[] = {450, 0, 0, 0, 120, 0, -1, 0, 333, 0, 0, 0, 0, 0};
    unsigned refused = 0;
    for (int call = 0; call < 400; call++) {
        if (!push_frame()) {
            refused++;          /* full: the producer waits */
        }
        push_frame();
        plan(p + call % 14, 1);
        nc_lr_audio_run(frontend);
    }
    HT_CHECK(refused > 0);
    HT_EQ_INT(g_bad, 0);
    HT_EQ_INT(g_next_push - g_got, queued_frames());
    HT_CHECK(queued_frames() * FRAME_BYTES <= (uint32_t)g_cap);
    apu_out_close();
}

/* 6: nothing queued, no call; closed, no call; opened again, empty */
static void test_empty_close_reopen(void)
{
    open_ring(48);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_calls, 0);
    fill();
    apu_out_close();
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_calls, 0);
    open_ring(48);
    HT_EQ_INT(queued_frames(), 0);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_calls, 0);
    fill();
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_bad, 0);        /* from pair 0 of the new session */
    apu_out_close();
}

/* 7: the period: five calls ask 4004 pairs; 60000 calls (1001 s at
 * 60000/1001 Hz) ask 48000 x 1001 */
static void test_period(void)
{
    open_ring(48);
    for (int call = 0; call < 5; call++) {
        fill();
        nc_lr_audio_run(frontend);
    }
    HT_EQ_INT(g_offered, 4004);
    apu_out_close();
    open_ring(48);
    uint64_t asked = 0;
    for (int call = 0; call < 60000; call++) {
        fill();
        const uint32_t before = g_offered;
        nc_lr_audio_run(frontend);
        asked += g_offered - before;
    }
    HT_EQ_INT(asked, 48000ull * 1001);
    HT_EQ_INT(g_bad, 0);
    apu_out_close();
}

/* a frontend claiming more than it was offered takes what was offered */
static void test_overclaim(void)
{
    open_ring(48);
    fill();
    const uint32_t before = queued_frames();
    static const int p[] = {-2};
    plan(p, 1);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_calls, 1);
    HT_EQ_INT(queued_frames(), before - 800);
    nc_lr_audio_run(frontend);
    HT_EQ_INT(g_bad, 0);
    apu_out_close();
}

int main(void)
{
    HT_RUN(test_full);
    HT_RUN(test_zero);
    HT_RUN(test_partial_then_zero);
    HT_RUN(test_repeated_partial);
    HT_RUN(test_wrap_and_backpressure);
    HT_RUN(test_empty_close_reopen);
    HT_RUN(test_period);
    HT_RUN(test_overclaim);
    HT_DONE();
}
