/*
 * hakuX native core — the APU's output on Linux: 48 kHz 16-bit stereo
 * through SDL3's audio (PipeWire on PortareOS).
 *
 * As out.c's AAudio output: the frame thread pushes each EP frame into a
 * ring, and the device's callback drains it (one producer, one consumer: two
 * counters). SDL follows the default device itself (headphones in or out).
 * No device opened: a thread drains the ring at the device's rate instead
 * (the sound goes nowhere, the APU keeps its timing) and tries to open one
 * again each second.
 *
 * SDL holds its device's lock while it calls on_data, which takes g_out.mu:
 * SDL's pause, resume and open are never called with g_out.mu held.
 */
#include "native/dev/apu/apu_int.h"

#include <SDL3/SDL.h>

#define OUT_RATE 48000
#define OUT_FRAME_BYTES 4   /* a stereo sample: two 16-bit channels */
#define REOPEN_NS 1000000000LL  /* a failed open is tried again after this */

static struct {
    uint8_t *buf;
    uint32_t cap;
    uint64_t head, tail;     /* bytes written, bytes read */
    int samples;             /* the callback's size, in frames */
    pthread_cond_t *wake;
    pthread_mutex_t mu;      /* stream, app_paused; held by each fill */
    SDL_AudioStream *stream; /* NULL: none (drain_thread drains the ring) */
    bool app_paused;
    bool draining;           /* drain_thread runs (joined by apu_out_close) */
    int quit;                /* apu_out_close: drain_thread leaves */
    pthread_t drain;
    uint32_t callbacks, short_callbacks;
} g_out = { .mu = PTHREAD_MUTEX_INITIALIZER };

int apu_out_queued(void)
{
    return (int)(apu_ld_acq(&g_out.head) - apu_ld_acq(&g_out.tail));
}

void apu_out_push(const void *frame, int bytes)
{
    const uint64_t head = g_out.head;
    assert(head + (uint64_t)bytes - apu_ld_acq(&g_out.tail) <= g_out.cap);
    const uint32_t at = (uint32_t)(head % g_out.cap);
    const uint32_t first = MIN((uint32_t)bytes, g_out.cap - at);
    memcpy(g_out.buf + at, frame, first);
    memcpy(g_out.buf, (const uint8_t *)frame + first, (uint32_t)bytes - first);
    apu_st_rel(&g_out.head, head + (uint64_t)bytes);
}

static int pop(uint8_t *out, int bytes)
{
    const uint64_t tail = g_out.tail;
    const int n = (int)MIN((uint64_t)bytes, apu_ld_acq(&g_out.head) - tail);
    const uint32_t at = (uint32_t)(tail % g_out.cap);
    const uint32_t first = MIN((uint32_t)n, g_out.cap - at);
    memcpy(out, g_out.buf + at, first);
    memcpy(out + first, g_out.buf, (uint32_t)n - first);
    apu_st_rel(&g_out.tail, tail + (uint64_t)n);
    return n;
}

void apu_out_stats(uint32_t *callbacks, uint32_t *short_callbacks)
{
    *callbacks = apu_xchg(&g_out.callbacks, 0);
    *short_callbacks = apu_xchg(&g_out.short_callbacks, 0);
}

/* g_out.mu held: one callback's worth, what the ring has and the rest
 * silence (a short callback). Paused (nc_audio_set_paused, nc_pause_set):
 * silence, the ring kept, no callback counted. */
static void fill(uint8_t *out, int bytes)
{
    if (g_out.app_paused || nc_paused()) {
        memset(out, 0, (size_t)bytes);
        return;
    }
    const int n = pop(out, bytes);
    apu_inc(&g_out.callbacks);
    if (n < bytes) {
        apu_inc(&g_out.short_callbacks);
        memset(out + n, 0, (size_t)(bytes - n));
    }
    pthread_cond_broadcast(g_out.wake);
}

/* SDL's audio thread: the device wants `more` bytes. */
static void SDLCALL on_data(void *user, SDL_AudioStream *s, int more, int total)
{
    static uint8_t buf[16384];
    pthread_mutex_lock(&g_out.mu);
    while (more > 0) {
        const int n = MIN(more, (int)sizeof(buf));
        fill(buf, n);
        SDL_PutAudioStreamData(s, buf, n);
        more -= n;
    }
    pthread_mutex_unlock(&g_out.mu);
}

/* A stream on the default playback device, paused; NULL if none opens. */
static SDL_AudioStream *open_stream(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, OUT_RATE };
    char frames[16];
    snprintf(frames, sizeof(frames), "%d", g_out.samples);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    return SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_data, NULL);
}

/* The stream opened: it takes over from the drain thread, and plays unless
 * the app has paused. */
static void start(SDL_AudioStream *s)
{
    pthread_mutex_lock(&g_out.mu);
    g_out.stream = s;
    const bool paused = g_out.app_paused;
    pthread_mutex_unlock(&g_out.mu);
    if (!paused) {
        SDL_ResumeAudioStreamDevice(s);
    }
    nc_log("hakuX-apu", "output: SDL %s, %s, %d frames a callback", SDL_GetCurrentAudioDriver(),
           SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s)), g_out.samples);
}

/* No stream: the ring drained at the device's rate, and a stream opened
 * again every REOPEN_NS until one does. */
static void *drain_thread(void *arg)
{
    static uint8_t buf[16384];
    const int len = MIN(g_out.samples * OUT_FRAME_BYTES, (int)sizeof(buf));
    const long period_ns = (long)((int64_t)(len / OUT_FRAME_BYTES) * 1000000000LL / OUT_RATE);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int64_t waited_ns = 0;
    while (!apu_ld_acq(&g_out.quit)) {
        if (waited_ns >= REOPEN_NS) {
            waited_ns = 0;
            SDL_AudioStream *s = open_stream();
            if (s) {
                start(s);
                return NULL;
            }
        }
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&g_out.mu);
        fill(buf, len);
        pthread_mutex_unlock(&g_out.mu);
        waited_ns += period_ns;
    }
    return NULL;
}

int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake)
{
    g_out.buf = calloc(1, (size_t)capacity_bytes);
    g_out.cap = (uint32_t)capacity_bytes;
    g_out.samples = samples;
    g_out.wake = wake;

    /* the front end's (hakux initializes it on its main thread) */
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        nc_log("hakuX-apu", "output: SDL audio: %s", SDL_GetError());
    }
    SDL_AudioStream *s = open_stream();
    if (s) {
        start(s);
    } else {
        nc_log("hakuX-apu", "no output stream (SDL: %s); draining without sound, trying again each second",
               SDL_GetError());
        g_out.quit = 0;
        g_out.draining = true;
        apu_thread_start(&g_out.drain, "apu.drain", drain_thread, NULL, false);
    }
    return samples * OUT_FRAME_BYTES;
}

/* The APU's teardown (nc_core_halt): the drain thread joined, the stream
 * closed (no more callbacks once SDL_DestroyAudioStream returns), the ring
 * freed. */
void apu_out_close(void)
{
    if (g_out.draining) {
        apu_st_rel(&g_out.quit, 1);
        pthread_join(g_out.drain, NULL);
        g_out.draining = false;
    }
    pthread_mutex_lock(&g_out.mu);
    SDL_AudioStream *s = g_out.stream;
    g_out.stream = NULL;
    pthread_mutex_unlock(&g_out.mu);
    if (s) {
        SDL_DestroyAudioStream(s);
    }
    free(g_out.buf);
    g_out.buf = NULL;
    g_out.cap = 0;
    g_out.head = g_out.tail = 0;
    g_out.wake = NULL;
}

/* The front end paused the emulation: no more consumed once this returns
 * (fills take g_out.mu), and the device stops pulling silence. */
void nc_audio_set_paused(bool on)
{
    pthread_mutex_lock(&g_out.mu);
    g_out.app_paused = on;
    SDL_AudioStream *s = g_out.stream;
    pthread_mutex_unlock(&g_out.mu);
    if (s && on) {
        SDL_PauseAudioStreamDevice(s);
    } else if (s) {
        SDL_ResumeAudioStreamDevice(s);
    }
}
