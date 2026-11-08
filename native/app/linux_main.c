/*
 * hakuX native core, Linux front end (hakux): the game in a fullscreen SDL3
 * window, its frames presented by the HLE GPU's Vulkan swapchain
 * (present.c), every gamepad as player 1's Duke (gamepad.c), and the APU's
 * output through SDL audio (out_sdl.c). linux-v0.3.0's front end, on today's
 * core.
 *
 *   hakux <disc image> <data directory> [option...]
 *   hakux none <data directory> dash [option...]    the Xbox dashboard
 *
 * Options and the two operands come in any order (RetroForge's launcher puts
 * the options first): an option is name=value (a lowercase name) or one of
 * the core's bare options (uncap, relaunch, dash); anything else is an
 * operand.
 *
 * The disc image: an XISO, a full dump (.iso) or a DVD CHD. "none" with the
 * core's option dash boots C:\xboxdash.xbe from the hard disk, if the player
 * installed it (the boot says so otherwise). The data directory holds the
 * emulated hard disk (hdd/: Partition0-5, Partition1 the E: drive with the
 * game saves, 3-5 the X/Y/Z caches, .overwrite the core's crash-safe copies),
 * the HLE GPU's caches (cache/) and the log (hakux.log). Options: the core's
 * (native/core/options.h: scale=, video=, avpack=, lang=, asyncpipe=,
 * prebuild=, devtime=, ...), and the front end's:
 *   aspect=auto|4:3|16:9|stretch  the picture's shape
 *   fxaa=0|1         FXAA over each frame
 *   upscale=bilinear|sgsr  how a frame smaller than the screen is scaled up
 *   states=DIR       the quick savestate: DIR/<disc name>/quick.hxs
 *   exitafter=S      leave after S seconds (tests)
 *
 * Every gamepad SDL opens is player 1's, merged: a handheld's own controls
 * often show twice (the raw device, silent while InputPlumber grabs it, and
 * InputPlumber's virtual pad). The title's rumble goes to all of them.
 *
 * Keys (a handheld's hotkey layer sends them): F5 loads the quick state,
 * Shift+F5 saves it, Ctrl+Q leaves.
 *
 * Leaving (SIGTERM, SIGINT or SIGHUP, Ctrl+Q, the window closing, or the
 * title stopping): the core stops and its devices halt, the hard disk
 * synced, then the process ends. A title's relaunch (XLaunchNewImage, e.g.
 * into another of its XBEs) and loading a state run hakux again in this
 * process (execv: the PID stays), with "relaunch" or "loadstate=".
 *
 * The log: the core reports a lot (in a game, ~17 lines a second of
 * push-buffer, MMIO and IRQ statistics), too much for a session's journal,
 * which is where a launcher's stderr goes. Unless stderr is a terminal or
 * HAKUX_LOG=stderr, it goes to <data directory>/hakux.log (the previous
 * run's kept as hakux.log.1), and the caller's stderr gets one line saying
 * so. A relaunch or a state load keeps writing to the same file.
 */
#include "native/app/app.h"
#include "native/core/core.h"
#include "native/core/options.h"
#include "native/hle/hle_native.h"
#include "native/kernel/kernel.h"
#include "native/state/save.h"
#include "hw/xbox/hle/vk/display.h"
#include "hw/xbox/hle/vk/vkdev.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HAKUX_VERSION
#define HAKUX_VERSION "dev"
#endif

/* hw/xbox/hle/pb/pbmode.h: the title runs in push-buffer mode (savestates
 * work only there) */
bool hle_pb_active(void);

static NcCore g_core;
static int g_disc_fd = -1;
static SDL_AtomicInt g_core_state; /* 0 booting, 1 running, -1 stopped */
static pthread_t g_core_thread;
static bool g_core_started, g_core_joined;
static volatile sig_atomic_t g_signal;
static int g_argc;
static char **g_argv;
static const char *g_disc;
static const char *g_states;       /* states= */

#define MAX_PADS 8
static SDL_Gamepad *g_pads[MAX_PADS];
static unsigned g_rumbles;         /* the title's rumble changes (logged as it leaves) */

static void pad_added(SDL_JoystickID id)
{
    for (unsigned i = 0; i < MAX_PADS; i++) {
        if (!g_pads[i]) {
            SDL_Gamepad *p = g_pads[i] = SDL_OpenGamepad(id);
            if (!p) {
                nc_log("hakuX-linux", "gamepad: %s", SDL_GetError());
                return;
            }
            const bool rumble =
                SDL_GetBooleanProperty(SDL_GetGamepadProperties(p), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
            nc_log("hakuX-linux", "gamepad %u: %s (%04x:%04x)%s, player 1", i, SDL_GetGamepadName(p),
                   SDL_GetGamepadVendor(p), SDL_GetGamepadProduct(p), rumble ? ", rumble" : "");
            return;
        }
    }
}

static void pad_removed(SDL_JoystickID id)
{
    for (unsigned i = 0; i < MAX_PADS; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            nc_log("hakuX-linux", "gamepad %u: removed", i);
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
        }
    }
}

/* The title's rumble on every pad: sent as it changes, and again each half
 * second while it lasts (each send is good for a second, so a rumble left on
 * by a stopped title ends). */
static void rumble(Uint64 now)
{
    static uint16_t last_l, last_r;
    static Uint64 sent;
    uint16_t l = 0, r = 0;
    nc_pad_rumble(0, &l, &r);
    const bool changed = l != last_l || r != last_r;
    if (!changed && (!(l || r) || now - sent < 500)) {
        return;
    }
    for (unsigned i = 0; i < MAX_PADS; i++) {
        if (g_pads[i]) {
            SDL_RumbleGamepad(g_pads[i], l, r, l || r ? 1000 : 0);
        }
    }
    g_rumbles += changed;
    last_l = l;
    last_r = r;
    sent = now;
}

/* Guest RAM tracking, MMIO and the JIT's faults are the core's; any other is
 * a crash: the default action, as the fault repeats. */
static void on_fault(int sig, siginfo_t *info, void *ctx)
{
    if (nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    static const char msg[] = "hakuX-linux: host fault outside the guest\n";
    write(2, msg, sizeof(msg) - 1);
    struct sigaction dfl = {.sa_handler = SIG_DFL};
    sigaction(sig, &dfl, NULL);
}

/* SIGTERM, SIGINT, SIGHUP: the main loop leaves at its next pass. */
static void on_stop_signal(int sig)
{
    g_signal = sig;
}

/* The vCPU thread. As it leaves, the stall monitor's PC sampler stops
 * signalling it (a SIGPROF to the joined thread crashed every exit). */
static void *core_thread(void *arg)
{
    if (!nc_core_boot(&g_core, g_disc_fd)) {
        nc_log("hakuX-linux", "core: boot failed%s%s", nc_hle_fatal() ? ": " : "",
               nc_hle_fatal() ? nc_hle_fatal() : "");
        nc_hle_cpu_leave();
        nc_cpu_thread_done();
        SDL_SetAtomicInt(&g_core_state, -1);
        return NULL;
    }
    SDL_SetAtomicInt(&g_core_state, 1);
    nc_core_run(&g_core);
    nc_hle_cpu_leave();
    nc_log("hakuX-linux", "core: stopped%s", g_core.kernel.relaunch ? " (a relaunch)"
                                             : g_core.kernel.quit   ? " (the title left)"
                                                                    : "");
    nc_cpu_thread_done();
    SDL_SetAtomicInt(&g_core_state, -1);
    return NULL;
}

static bool make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

/* stderr to <data>/hakux.log (the header's "The log"). */
static void log_to_file(const char *data)
{
    const char *mode = getenv("HAKUX_LOG");
    if ((mode && (!strcmp(mode, "stderr") || !strcmp(mode, "kept"))) || isatty(STDERR_FILENO)) {
        return;                         /* kept: a respawn, already writing to the file */
    }
    char path[PATH_MAX + 16], old[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/hakux.log", data);
    snprintf(old, sizeof(old), "%s.1", path);
    rename(path, old);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "hakuX-linux: can't log to %s (%s): logging here\n", path, strerror(errno));
        return;
    }
    fprintf(stderr, "hakuX-linux: logging to %s\n", path);
    dup2(fd, STDERR_FILENO);            /* the copy is not close-on-exec: a respawn keeps it */
    close(fd);
    setenv("HAKUX_LOG", "kept", 1);
}

/* The quick state's file (states=DIR: DIR/<disc name>/quick.hxs, the folder
 * made), false without states=. */
static bool state_path(char *out, size_t n)
{
    if (!g_states) {
        return false;
    }
    const char *base = strrchr(g_disc, '/');
    base = base ? base + 1 : g_disc;
    char name[NAME_MAX + 1];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        *dot = 0;
    }
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/%s", g_states, name) >= (int)sizeof(dir) || !make_dir(g_states) ||
        !make_dir(dir)) {
        nc_log("hakuX-linux", "savestate folder %s/%s: %s", g_states, name, strerror(errno));
        return false;
    }
    return snprintf(out, n, "%s/quick.hxs", dir) < (int)n;
}

static void save_state(void)
{
    char path[PATH_MAX];
    if (!state_path(path, sizeof(path))) {
        return;
    }
    if (SDL_GetAtomicInt(&g_core_state) != 1 || !hle_pb_active()) {
        nc_log("hakuX-linux", "savestate: not available yet (the game isn't running in push-buffer mode)");
        return;
    }
    const Uint64 t0 = SDL_GetTicks();
    const char *err = nc_state_save(path);
    if (err) {
        nc_log("hakuX-linux", "savestate: not saved: %s", err);
        return;
    }
    nc_cache_gc(path);
    nc_log("hakuX-linux", "savestate: saved %s in %llu ms", path, (unsigned long long)(SDL_GetTicks() - t0));
}

/* The core stopped and its devices halted (the hard disk synced), waiting at
 * most timeout_ms for the guest to stop: false when it didn't. */
static bool stop_core(unsigned timeout_ms)
{
    if (!g_core_started || g_core_joined) {
        return g_core_joined;
    }
    nc_core_request_stop(&g_core);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec++;
    }
    const int err = pthread_timedjoin_np(g_core_thread, NULL, &until);
    if (err) {
        nc_log("hakuX-linux", "the core didn't stop within %u ms (%s): leaving without halting it", timeout_ms,
               strerror(err));
        return false;
    }
    g_core_joined = true;
    nc_core_halt(&g_core);
    return true;
}

/* The window, the swapchain and SDL let go (before an execv or the exit). */
static void drop_window(SDL_Window *win)
{
    uint32_t pw, ph;
    hle_present_begin(NULL, &pw, &ph);  /* no window: the swapchain and surface go */
    for (unsigned i = 0; i < MAX_PADS; i++) {
        if (g_pads[i]) {
            SDL_RumbleGamepad(g_pads[i], 0, 0, 0);
        }
    }
    SDL_DestroyWindow(win);
    SDL_Quit();
}

/* hakux again in this process, on the same command line without its
 * "relaunch" and "loadstate=", plus extra. Returns only if execv failed. */
static void respawn(SDL_Window *win, const char *extra)
{
    stop_core(8000);
    drop_window(win);
    char **argv = calloc((size_t)g_argc + 2, sizeof(*argv));
    int n = 0;
    for (int i = 0; i < g_argc; i++) {
        if (i > 0 && (!strcmp(g_argv[i], "relaunch") || !strncmp(g_argv[i], "loadstate=", 10))) {
            continue;
        }
        argv[n++] = g_argv[i];
    }
    argv[n++] = (char *)extra;
    argv[n] = NULL;
    nc_log("hakuX-linux", "starting again with %s", extra);
    execv("/proc/self/exe", argv);
    nc_log("hakuX-linux", "can't start again: %s", strerror(errno));
}

/* An operand (the disc or the data directory), not an option: options are
 * name=value (a lowercase name) or one of the core's bare ones. */
static bool is_operand(const char *a)
{
    size_t n = strspn(a, "abcdefghijklmnopqrstuvwxyz0123456789_");
    if (n && a[0] >= 'a' && a[0] <= 'z' && a[n] == '=') {
        return false;
    }
    return strcmp(a, "uncap") && strcmp(a, "relaunch") && strcmp(a, "dash");
}

int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    const char *operand[2] = {NULL, NULL};
    int operands = 0;
    for (int i = 1; i < argc; i++) {
        if (is_operand(argv[i]) && operands++ < 2) {
            operand[operands - 1] = argv[i];
        }
    }
    if (operands != 2) {
        fprintf(stderr, "usage: hakux <disc image | none> <data directory> [option...]\n");
        return 2;
    }
    g_disc = operand[0];
    char data[PATH_MAX], hdd[PATH_MAX + 16], cache[PATH_MAX + 16];
    if (!make_dir(operand[1]) || !realpath(operand[1], data)) {
        fprintf(stderr, "invalid data directory: %s\n", operand[1]);
        return 2;
    }
    log_to_file(data);
    snprintf(hdd, sizeof(hdd), "%s/hdd", data);
    snprintf(cache, sizeof(cache), "%s/cache", data);
    bool ok = make_dir(hdd) && make_dir(cache);
    for (int i = 0; ok && i <= 5; i++) {
        char p[PATH_MAX + 32];
        snprintf(p, sizeof(p), "%s/Partition%d", hdd, i);
        ok = make_dir(p);
    }
    if (!ok) {
        perror("data directories");
        return 2;
    }
    /* "none": no disc, for the dashboard (option dash) */
    const bool no_disc = !strcmp(g_disc, "none");
    g_disc_fd = no_disc ? -1 : open(g_disc, O_RDONLY | O_CLOEXEC);
    if (g_disc_fd < 0 && !no_disc) {
        perror(g_disc);
        return 2;
    }
    nc_io_set_hdd_root(hdd);
    setenv("HAKUX_HLE_MODE", "replace", 0);
    setenv("HAKUX_NATIVE_BASE", cache, 1);

    NcOptions opt;
    nc_options_init(&opt);
    nc_option(&opt, "version=" HAKUX_VERSION);   /* recorded in savestates */
    unsigned exit_after = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (is_operand(a)) {
            continue;
        }
        if (nc_option(&opt, a)) {
            continue;                           /* the core's (native/core/options.h) */
        }
        if (!strncmp(a, "aspect=", 7)) {
            nc_present_set_aspect(!strcmp(a + 7, "4:3")       ? NC_ASPECT_4_3
                                  : !strcmp(a + 7, "16:9")    ? NC_ASPECT_16_9
                                  : !strcmp(a + 7, "stretch") ? NC_ASPECT_STRETCH
                                                              : NC_ASPECT_AUTO);
        } else if (!strncmp(a, "fxaa=", 5)) {
            nc_present_set_fxaa(atoi(a + 5) != 0);
        } else if (!strncmp(a, "upscale=", 8)) {
            nc_present_set_upscale(!strcmp(a + 8, "sgsr") ? NC_UPSCALE_SGSR : NC_UPSCALE_BILINEAR);
        } else if (!strncmp(a, "states=", 7) && a[7]) {
            g_states = a + 7;
        } else if (!strncmp(a, "exitafter=", 10)) {
            exit_after = (unsigned)atoi(a + 10);
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        }
    }
    nc_options_apply(&opt);
    nc_present_set_widescreen(opt.video >= 0 && (opt.video & NC_VIDEO_WIDESCREEN));
    nc_log("hakuX-linux", "hakuX %s: disc %s, data %s", HAKUX_VERSION, g_disc, data);

    /* our own SIGTERM/SIGINT (below): SDL's would turn them into a quit event
     * only while events are polled */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetAppMetadata("hakuX", HAKUX_VERSION, "ch.portare.hakux");   /* the audio stream's name */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        nc_log("hakuX-linux", "SDL_Init: %s", SDL_GetError());
        return 1;
    }
    /* here on the main thread, before the core opens the output; without it
     * the game runs on, its sound drained unheard */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        nc_log("hakuX-linux", "SDL audio: %s", SDL_GetError());
    }
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    const int ww = dm ? dm->w : 1280, wh = dm ? dm->h : 960;
    /* SDL_WINDOW_VULKAN: SDL loads the Vulkan loader, the HLE GPU's too */
    SDL_Window *win = SDL_CreateWindow("hakuX", ww, wh, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
    if (!win) {
        nc_log("hakuX-linux", "window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_HideCursor();
    nc_hle_set_vk_loader((void *)SDL_Vulkan_GetVkGetInstanceProcAddr());
    Uint32 next = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&next);
    hle_vk_set_window_extensions(exts, next);
    nc_log("hakuX-linux", "video %s: %dx%d at %.0f Hz, %u Vulkan instance extension(s) for the window",
           SDL_GetCurrentVideoDriver(), ww, wh, dm ? dm->refresh_rate : 0.0f, (unsigned)next);
    if (!nc_present_init()) {
        SDL_Quit();
        return 1;
    }

    struct sigaction sa = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    struct sigaction st = {.sa_handler = on_stop_signal};
    sigemptyset(&st.sa_mask);
    sigaction(SIGTERM, &st, NULL);
    sigaction(SIGINT, &st, NULL);
    sigaction(SIGHUP, &st, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    g_core_started = pthread_create(&g_core_thread, &attr, core_thread, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (!g_core_started) {
        nc_log("hakuX-linux", "core thread: %s", strerror(errno));
        drop_window(win);
        return 1;
    }

    const Uint64 start = SDL_GetTicks();
    bool shown = false;
    Uint64 shown_at = 0, shown_frames = 0;  /* for the run's average at the end */
    const char *why = NULL;
    while (!why) {
        bool load = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                why = "quit requested";
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.repeat) {
                    break;
                }
                if (ev.key.key == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL)) {
                    why = "Ctrl+Q";
                } else if (ev.key.key == SDLK_F5 && (ev.key.mod & SDL_KMOD_SHIFT)) {
                    save_state();
                } else if (ev.key.key == SDLK_F5) {
                    load = true;
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                pad_added(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                pad_removed(ev.gdevice.which);
                break;
            default:
                break;
            }
        }
        NcPadState s = {0};
        for (unsigned i = 0; i < MAX_PADS; i++) {
            if (g_pads[i]) {
                nc_read_gamepad(g_pads[i], &s);
            }
        }
        nc_pad_set(0, &s);
        rumble(SDL_GetTicks());

        if (!nc_present_frame(win)) {
            SDL_Delay(8);               /* nothing to wait for the panel on */
        }
        if (!shown && nc_present_shown()) {
            shown = true;
            shown_at = SDL_GetTicks();
            shown_frames = hle_disp_stats().presented;
            nc_log("hakuX-linux", "first title frame on screen after %.1f s",
                   (double)(SDL_GetTicks() - start) / 1000.0);
        }
        if (load) {
            char path[PATH_MAX];
            if (state_path(path, sizeof(path))) {
                const char *err = access(path, R_OK) ? "no quick state saved for this game" : nc_state_compat(path);
                if (err) {
                    nc_log("hakuX-linux", "savestate: %s not loaded: %s", path, err);
                } else {
                    char arg[PATH_MAX + 16];
                    snprintf(arg, sizeof(arg), "loadstate=%s", path);
                    respawn(win, arg);
                    _exit(1);
                }
            }
        }
        if (g_signal) {
            why = g_signal == SIGTERM ? "SIGTERM" : g_signal == SIGINT ? "SIGINT" : "SIGHUP";
        } else if (SDL_GetAtomicInt(&g_core_state) < 0) {
            if (g_core.kernel.relaunch) {
                respawn(win, "relaunch");
                _exit(1);
            }
            why = nc_hle_fatal() ? nc_hle_fatal() : "the core stopped";
        } else if (nc_hle_fatal()) {
            why = nc_hle_fatal();
        } else if (exit_after && SDL_GetTicks() - start >= (Uint64)exit_after * 1000) {
            why = "exitafter";
        }
    }
    const Uint64 end = SDL_GetTicks();
    if (shown && end > shown_at) {
        const uint64_t frames = hle_disp_stats().presented - shown_frames;
        nc_log("hakuX-linux", "leaving (%s) after %.1f s: %llu frames presented in the %.1f s since the first, "
               "%.1f fps on average, %u rumble changes", why, (double)(end - start) / 1000.0,
               (unsigned long long)frames, (double)(end - shown_at) / 1000.0,
               (double)frames * 1000.0 / (double)(end - shown_at), g_rumbles);
    } else {
        nc_log("hakuX-linux", "leaving (%s) after %.1f s: no frame shown, %u rumble changes", why,
               (double)(end - start) / 1000.0, g_rumbles);
    }
    stop_core(8000);
    drop_window(win);
    _exit(shown || g_signal ? 0 : 1);
}
