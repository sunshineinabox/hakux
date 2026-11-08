/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The libretro core's rumble (native/libretro/input.c) with the real
 * controller (native/dev/xid.c): a title's rumble report reaches RetroArch's
 * motors only with the core option on, each strength once, and they stop
 * when the option goes off, the port's controller is pulled out or the game
 * stops; the next boot's controller is still. No guest, renderer or
 * frontend: RetroArch's environment and rumble interface are stubbed here. */
#include "native/libretro/libretro_int.h"
#include "native/dev/usb.h"

#include <stdio.h>
#include <string.h>

static unsigned failures, asked;
static struct {
    unsigned port, effect;
    uint16_t strength;
} g_calls[32];
static unsigned g_ncalls;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

static bool set_rumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    if (g_ncalls < 32) {
        g_calls[g_ncalls].port = port;
        g_calls[g_ncalls].effect = effect;
        g_calls[g_ncalls].strength = strength;
    }
    g_ncalls++;
    return true;
}

bool nc_lr_env(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE) {
        asked++;
        ((struct retro_rumble_interface *)data)->set_rumble_state = set_rumble;
        return true;
    }
    return false;
}

static void poll(void)
{
}

static int16_t state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return 0;
}

/* The title's HID SET_REPORT: the left and right motors' strengths. */
static void report(NcUsbDev *pad, uint16_t left, uint16_t right)
{
    const uint8_t setup[8] = {0x21, 0x09, 0x00, 0x02, 0, 0, 6, 0};
    uint8_t r[6] = {0, 6, (uint8_t)left, (uint8_t)(left >> 8), (uint8_t)right, (uint8_t)(right >> 8)};
    pad->control(pad, setup, r, 6);
}

/* One retro_run's input; the rumble calls it made, as "port:effect=strength". */
static const char *run(void)
{
    static char out[256];
    g_ncalls = 0;
    nc_lr_input_run(poll, state);
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < g_ncalls && i < 32 && n < sizeof(out); i++) {
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s%u:%s=%04x", i ? " " : "", g_calls[i].port + 1,
                              g_calls[i].effect == RETRO_RUMBLE_STRONG ? "strong" : "weak",
                              g_calls[i].strength);
    }
    printf("  rumble calls: %s\n", g_ncalls ? out : "none");
    return out;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    nc_lr_input_reset();
    NcUsbDev *pad = nc_xid_gamepad_new(0);
    report(pad, 0x1234, 0x5678);

    printf("option off (the default), the title rumbling\n");
    run();
    check(g_ncalls == 0 && asked == 0, "nothing sent, RetroArch's interface not even asked for");

    printf("option on\n");
    nc_lr_input_set_rumble(true);
    check(!strcmp(run(), "1:strong=1234 1:weak=5678"), "the left motor as strong, the right as weak, port 1 only");
    run();
    check(g_ncalls == 0, "the same strengths are not sent again");
    report(pad, 0xffff, 0x5678);
    check(!strcmp(run(), "1:strong=ffff"), "a changed strength is sent alone");

    printf("port 1's controller pulled out, then plugged in\n");
    nc_lr_input_set_device(0, RETRO_DEVICE_NONE);
    check(!strcmp(run(), "1:strong=0000 1:weak=0000"), "pulled out: its motors stop");
    nc_lr_input_set_device(0, RETRO_DEVICE_JOYPAD);
    check(!strcmp(run(), "1:strong=ffff 1:weak=5678"), "plugged in: the title's strengths again");

    printf("option off during the game\n");
    nc_lr_input_set_rumble(false);
    check(!strcmp(run(), "1:strong=0000 1:weak=0000"), "the motors stop at the next retro_run");
    run();
    check(g_ncalls == 0, "and stay so");

    printf("option on, then the game stops\n");
    nc_lr_input_set_rumble(true);
    run();
    g_ncalls = 0;
    nc_lr_input_rumble_stop();
    check(g_ncalls == 2 && g_calls[0].strength == 0 && g_calls[1].strength == 0, "stop_core's stop: both motors 0");
    check(asked == 1, "RetroArch's interface asked for once");

    printf("the next boot's controller\n");
    nc_xid_gamepad_new(0);
    uint16_t left = 1, right = 1;
    nc_pad_rumble(0, &left, &right);
    check(left == 0 && right == 0, "starts still, not with the last title's strengths");
    run();
    check(g_ncalls == 0, "so nothing starts the motors again");
    return failures ? 1 : 0;
}
