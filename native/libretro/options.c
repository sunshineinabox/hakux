/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Libretro core options (LIBRETRO_SPEC.md section 10).
 * RetroArch handles display filters and aspect ratio. Crop (video.c),
 * rumble (input.c) and frame pacing (the decoder reads it at each swap)
 * update immediately; other options apply at the next boot (the
 * Communicator is plugged into player 1's controller as it boots).
 */
#include "native/libretro/libretro_int.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"
#include "native/dev/dev.h"
#include "native/dev/usb.h"
#include "hw/xbox/hle/shader/psh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEXT " Restart the game to apply."

#if NC_HAVE_DSP_JIT
#define AUDIO_PROCESSING_DEFAULT "dsp"
#else
#define AUDIO_PROCESSING_DEFAULT "fast"
#endif

#if defined(__x86_64__) && !defined(__ANDROID__)
#define COMBINER_PRECISION_DEFAULT "enabled"
#else
#define COMBINER_PRECISION_DEFAULT "disabled"
#endif

enum { OPT_SCALE, OPT_CROP, OPT_FORMAT, OPT_LANGUAGE, OPT_REGION, OPT_RUMBLE, OPT_NETWORK, OPT_COMMUNICATOR,
       OPT_MIC_GATE, OPT_RESAMPLER, OPT_PROCESSING, OPT_PRECISION, OPT_PACE, OPT_COUNT };

/* The language values follow XC_LANGUAGE after auto: english is 1. */
static struct retro_core_option_v2_definition g_defs[OPT_COUNT + 1] = {
    [OPT_SCALE] = {
        "hakux_render_scale", "Render Scale", NULL,
        "1x uses the Xbox's native resolution (usually 640x480). The default, 2x, doubles it. "
        "Higher values sharpen the picture but need more GPU power." NEXT,
        NULL, NULL,
        {{"1", "1x (Xbox)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {NULL, NULL}},
        "2"},
    [OPT_CROP] = {
        "hakux_crop_overscan", "Crop Overscan", NULL,
        "Removes 3.75% from each edge to hide the TV overscan border. Off shows the full frame.",
        NULL, NULL,
        {{"enabled", "On"}, {"disabled", "Off"}, {NULL, NULL}},
        "enabled"},
    [OPT_FORMAT] = {
        "hakux_screen_format", "Screen Format", NULL,
        "Sets the console's screen format. Widescreen uses 16:9; Letterbox fits 16:9 inside 4:3. "
        "Only games that support these formats use them." NEXT,
        NULL, NULL,
        {{"normal", "Normal (4:3)"}, {"widescreen", "Widescreen (16:9)"}, {"letterbox", "Letterbox"},
         {NULL, NULL}},
        "normal"},
    [OPT_LANGUAGE] = {
        "hakux_language", "Language", NULL,
        "Sets the language in games that support it. Auto uses RetroArch's language, "
        "falling back to English if the Xbox does not support it." NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"english", "English"}, {"japanese", "Japanese"}, {"german", "German"},
         {"french", "French"}, {"spanish", "Spanish"}, {"italian", "Italian"}, {"korean", "Korean"},
         {"chinese", "Chinese (Traditional)"}, {"portuguese", "Portuguese"}, {NULL, NULL}},
        "auto"},
    [OPT_REGION] = {
        "hakux_video_region", "Video Region", NULL,
        "Sets the console's TV standard. Auto selects PAL for European games and NTSC-M otherwise. "
        "PAL games use 60 Hz when supported; 50 Hz games keep their speed but may look less smooth."
        NEXT,
        NULL, NULL,
        {{"auto", "Auto"}, {"ntsc-m", "NTSC-M (Americas)"}, {"ntsc-j", "NTSC-J (Japan)"},
         {"pal", "PAL (Europe)"}, {NULL, NULL}},
        "auto"},
    [OPT_RUMBLE] = {
        "hakux_rumble", "Rumble", NULL,
        "Enables vibration when supported by RetroArch and the controller.",
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_NETWORK] = {
        "hakux_network", "Network", NULL,
        "LAN enables system link: turn it on on each device; discovery uses UDP port 3075. "
        "For peers on another subnet or a VPN, add their addresses to lan-peers.txt in the save directory. "
        "Xbox Live (Insignia) connects to Insignia, the Xbox Live revival, through the Internet; register the "
        "console with Insignia's Setup Assistant first. If your provider blocks Insignia's DNS server, put the "
        "address of an insigniaDNS server in insignia-dns.txt in the save directory." NEXT,
        NULL, NULL,
        {{"disabled", "Off (no cable)"}, {"lan", "LAN (system link)"}, {"insignia", "Xbox Live (Insignia)"},
         {NULL, NULL}},
        "disabled"},
    [OPT_COMMUNICATOR] = {
        "hakux_communicator", "Xbox Live Communicator", NULL,
        "Plugs a Communicator headset into player 1's controller, for voice chat over Xbox Live (Insignia) or "
        "system link in games that have it. It hears RetroArch's microphone (Settings > Audio > Microphone), "
        "silence without one, and its headphone plays with the game's sound. The microphone is used only while "
        "a game's voice chat runs. Experimental." NEXT,
        NULL, NULL,
        {{"disabled", "Off"}, {"enabled", "On"}, {NULL, NULL}},
        "disabled"},
    [OPT_MIC_GATE] = {
        "hakux_mic_sensitivity", "Communicator Microphone Sensitivity", NULL,
        "With the Communicator on: how quiet a sound still counts as talking. Quieter sounds, and sounds that "
        "aren't a voice (buttons, sticks, a fan), are muted, so the game doesn't show you as talking for them. "
        "Always on sends everything the microphone hears." NEXT,
        NULL, NULL,
        {{"medium", "Medium"}, {"low", "Low (only clear speech)"}, {"high", "High (quiet voices too)"},
         {"off", "Always on (no noise gate)"}, {NULL, NULL}},
        "medium"},
    [OPT_RESAMPLER] = {
        "hakux_audio_resampler", "Audio Resampler", NULL,
        "Converts game audio to 48 kHz. Hardware interpolates as the Xbox does. Sinc preserves "
        "high frequencies and reduces aliasing. Linear uses the least CPU but softens high "
        "frequencies." NEXT,
        NULL, NULL,
        {{"hardware", "Hardware (as the Xbox)"}, {"sinc", "Sinc"}, {"linear", "Linear"}, {NULL, NULL}},
        "hardware"},
    [OPT_PROCESSING] = {
        "hakux_audio_processing", "Audio Processing", NULL,
        "Full DSP runs the game's Xbox reverb and other audio effects with stereo output. "
        "It uses more CPU. Fast plays the dry mix; try it if Full DSP crackles or runs slowly." NEXT,
        NULL, NULL,
        {{"fast", "Fast (dry mix)"}, {"dsp", "Full DSP (experimental)"}, {NULL, NULL}},
        AUDIO_PROCESSING_DEFAULT},
    [OPT_PRECISION] = {
        "hakux_combiner_precision", "Combiner Precision", NULL,
        "Accurate rounds pixel-shader combiner outputs like the Xbox. Fast uses "
        "floating-point outputs and may improve GPU performance." NEXT,
        NULL, NULL,
        {{"disabled", "Fast"}, {"enabled", "Accurate"}, {NULL, NULL}},
        COMBINER_PRECISION_DEFAULT},
    /* The value is the decoder's depth: the swaps back it waits for the GPU
     * to finish (pb/pbdraw.c g_pace). */
    [OPT_PACE] = {
        "hakux_frame_pace", "Frame Pacing", NULL,
        "Holds the game to the GPU's rendering pace when the GPU can't keep up, instead of letting "
        "the game run ahead of the picture and add input lag. On keeps up to two frames waiting for "
        "the GPU; Lowest Latency keeps one, which may lower the frame rate. Off lets the game run ahead.",
        NULL, NULL,
        {{"disabled", "Off"}, {"2", "On"}, {"1", "Lowest Latency"}, {NULL, NULL}},
        "disabled"},
    {NULL},
};

static int g_applied[OPT_COUNT];   /* the value each option booted with (its index) */

/* Frame pacing's depth for the option's value index (0: off), and as the
 * log says it. */
static int pace_depth(int i)
{
    return atoi(g_defs[OPT_PACE].values[i].value);
}

static const char *pace_text(int i)
{
    static char text[16];
    if (!pace_depth(i)) {
        return "off";
    }
    snprintf(text, sizeof(text), "%d back", pace_depth(i));
    return text;
}

/* A frontend without the v2 options (or v1, which adds nothing used here):
 * the legacy variables, "Description; default|other|...". */
static void declare_legacy(retro_environment_t env)
{
    static char text[OPT_COUNT][512];
    static struct retro_variable vars[OPT_COUNT + 1];
    for (int i = 0; i < OPT_COUNT; i++) {
        const struct retro_core_option_v2_definition *d = &g_defs[i];
        size_t n = (size_t)snprintf(text[i], sizeof(text[i]), "%s; %s", d->desc, d->default_value);
        for (const struct retro_core_option_value *v = d->values; v->value && n < sizeof(text[i]); v++) {
            if (strcmp(v->value, d->default_value)) {
                n += (size_t)snprintf(text[i] + n, sizeof(text[i]) - n, "|%s", v->value);
            }
        }
        vars[i] = (struct retro_variable){d->key, text[i]};
    }
    env(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

void nc_lr_options_declare(retro_environment_t env)
{
    static struct retro_core_option_v2_category none[] = {{NULL, NULL, NULL}};
    static struct retro_core_options_v2 opts = {none, g_defs};
    unsigned version = 0;
    if (!env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) || version < 2 ||
        !env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts)) {
        declare_legacy(env);
    }
}

/* The option's value now, as the index of one of its values (the default's
 * for one the frontend doesn't have, or doesn't know). */
static int value_of(int opt)
{
    const struct retro_core_option_v2_definition *d = &g_defs[opt];
    struct retro_variable var = {d->key, NULL};
    const char *want = nc_lr_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value ? var.value : d->default_value;
    int def = 0;
    for (int i = 0; d->values[i].value; i++) {
        if (!strcmp(d->values[i].value, want)) {
            return i;
        }
        def = strcmp(d->values[i].value, d->default_value) ? def : i;
    }
    return def;
}

/* Auto: RetroArch's language as XC_LANGUAGE, English where the Xbox has none. */
static int auto_language(void)
{
    unsigned lang = RETRO_LANGUAGE_ENGLISH;
    nc_lr_env(RETRO_ENVIRONMENT_GET_LANGUAGE, &lang);
    switch (lang) {
    case RETRO_LANGUAGE_JAPANESE:
        return 2;
    case RETRO_LANGUAGE_GERMAN:
        return 3;
    case RETRO_LANGUAGE_FRENCH:
        return 4;
    case RETRO_LANGUAGE_SPANISH:
        return 5;
    case RETRO_LANGUAGE_ITALIAN:
        return 6;
    case RETRO_LANGUAGE_KOREAN:
        return 7;
    case RETRO_LANGUAGE_CHINESE_TRADITIONAL:
    case RETRO_LANGUAGE_CHINESE_SIMPLIFIED:
        return 8;
    case RETRO_LANGUAGE_PORTUGUESE_BRAZIL:
    case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL:
        return 9;
    default:
        return 1;
    }
}

void nc_lr_options_apply(void)
{
    for (int i = 0; i < OPT_COUNT; i++) {
        g_applied[i] = value_of(i);
    }
    const int scale = atoi(g_defs[OPT_SCALE].values[g_applied[OPT_SCALE]].value);
    nc_hle_set_render_scale(scale);
    const bool crop = strcmp(g_defs[OPT_CROP].values[g_applied[OPT_CROP]].value, "disabled") != 0;
    nc_lr_video_set_crop(crop);
    const bool rumble = strcmp(g_defs[OPT_RUMBLE].values[g_applied[OPT_RUMBLE]].value, "enabled") == 0;
    nc_lr_input_set_rumble(rumble);

    /* PAL60 always: RetroArch runs at 59.94 Hz, and an NTSC title ignores it.
     * No 480p, 720p or 1080i, as before the options. */
    const char *format = g_defs[OPT_FORMAT].values[g_applied[OPT_FORMAT]].value;
    const uint32_t flags = NC_VIDEO_PAL60 | (!strcmp(format, "widescreen") ? NC_VIDEO_WIDESCREEN
                                             : !strcmp(format, "letterbox") ? NC_VIDEO_LETTERBOX
                                                                            : 0);
    nc_kernel_set_video(flags, NC_AV_PACK_HDTV);
    nc_lr_video_set_widescreen(flags & NC_VIDEO_WIDESCREEN);

    const int language = g_applied[OPT_LANGUAGE] ? g_applied[OPT_LANGUAGE] : auto_language();
    nc_kernel_set_language(language);
    /* Always emulate console drive timing, as upstream's release app does
     * (only debug builds can toggle it). Disc and hard disk reads take at
     * least as long as on the console; instant loads froze Morrowind when
     * leaving a building. NC_DEVTIME tunes the timing. */
    nc_io_set_device_time(true);

    const char *region = g_defs[OPT_REGION].values[g_applied[OPT_REGION]].value;
    nc_kernel_set_av_region(!strcmp(region, "ntsc-m")   ? NC_AV_REGION_NTSC_M
                            : !strcmp(region, "ntsc-j") ? NC_AV_REGION_NTSC_J
                            : !strcmp(region, "pal")    ? NC_AV_REGION_PAL_I
                                                        : 0);

    const char *network = g_defs[OPT_NETWORK].values[g_applied[OPT_NETWORK]].value;
    nc_lr_net_apply(network);
    /* the noise gate's threshold in dBFS, as the app's sensitivities */
    const bool communicator = !strcmp(g_defs[OPT_COMMUNICATOR].values[g_applied[OPT_COMMUNICATOR]].value, "enabled");
    const char *sensitivity = g_defs[OPT_MIC_GATE].values[g_applied[OPT_MIC_GATE]].value;
    const char *gate = !strcmp(sensitivity, "low")    ? "-30"
                       : !strcmp(sensitivity, "high") ? "-46"
                       : !strcmp(sensitivity, "off")  ? "off"
                                                      : "-38";
    nc_dev_ohci_set_voice(communicator ? 1 : 0);
    nc_xblc_set_gate(gate);
    const char *resampler = g_defs[OPT_RESAMPLER].values[g_applied[OPT_RESAMPLER]].value;
    nc_dev_apu_set_resampler(resampler);
    const bool dsp = !strcmp(g_defs[OPT_PROCESSING].values[g_applied[OPT_PROCESSING]].value, "dsp");
    nc_dev_apu_set_dsp(dsp);
    nc_hle_set_frame_pace(pace_depth(g_applied[OPT_PACE]));
    const bool precise = !strcmp(g_defs[OPT_PRECISION].values[g_applied[OPT_PRECISION]].value, "enabled");
    hle_psh_set_precision(precise);
    nc_log("hakuX-libretro", "options: render scale %dx, overscan %s, screen %s, language %d (%s), video region %s, "
           "rumble %s, network %s, resampler %s, audio processing %s, frame pacing %s, communicator %s%s%s, combiner %s",
           scale, crop ? "cropped" : "shown", format, language,
           g_defs[OPT_LANGUAGE].values[g_applied[OPT_LANGUAGE]].value, region, rumble ? "on" : "off",
           strcmp(network, "disabled") ? network : "off", resampler, dsp ? "dsp" : "fast",
           pace_text(g_applied[OPT_PACE]), communicator ? "on (noise gate " : "off", communicator ? gate : "",
           communicator ? ")" : "", precise ? "accurate" : "fast");
}

void nc_lr_options_run(void)
{
    bool changed = false;
    if (!nc_lr_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) || !changed) {
        return;
    }
    const int crop = value_of(OPT_CROP);
    if (crop != g_applied[OPT_CROP]) {
        g_applied[OPT_CROP] = crop;
        nc_lr_video_set_crop(strcmp(g_defs[OPT_CROP].values[crop].value, "disabled") != 0);
    }
    const int rumble = value_of(OPT_RUMBLE);
    if (rumble != g_applied[OPT_RUMBLE]) {
        g_applied[OPT_RUMBLE] = rumble;
        nc_lr_input_set_rumble(strcmp(g_defs[OPT_RUMBLE].values[rumble].value, "enabled") == 0);
    }
    /* the decoder takes it at its next swap; a stall it is in ends as it would */
    const int pace = value_of(OPT_PACE);
    if (pace != g_applied[OPT_PACE]) {
        g_applied[OPT_PACE] = pace;
        nc_hle_set_frame_pace(pace_depth(pace));
        nc_log("hakuX-libretro", "options: frame pacing %s", pace_text(pace));
    }
    char names[256] = "";
    size_t n = 0;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (i != OPT_CROP && i != OPT_RUMBLE && i != OPT_PACE && value_of(i) != g_applied[i] &&
            n < sizeof(names)) {
            n += (size_t)snprintf(names + n, sizeof(names) - n, "%s%s", n ? ", " : "", g_defs[i].desc);
        }
    }
    if (!n) {
        return;
    }
    static char text[320];
    snprintf(text, sizeof(text), "%s: restart the game to apply", names);
    const struct retro_message msg = {text, 240};
    nc_lr_env(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&msg);
    nc_log("hakuX-libretro", "options: %s", text);
}
