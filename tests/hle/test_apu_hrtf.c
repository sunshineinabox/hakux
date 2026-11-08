/* Compare the native HRTF filter with xemu #3105's scalar reference for
 * steady and changing targets, with delays up to 42 samples. */
#include "hle_test.h"

#include <math.h>
#include <stdlib.h>

#include "native/dev/apu/vp/hrtf.h"

/* ---- xemu's filter */
#define REF_BUFLEN (HRTF_NUM_TAPS + HRTF_MAX_DELAY_SAMPLES)

typedef struct {
    int buf_pos;
    struct {
        float buf[REF_BUFLEN];
        float hrir_coeff_cur[HRTF_NUM_TAPS];
        float hrir_coeff_tar[HRTF_NUM_TAPS];
    } ch[2];
    float itd_cur;
    float itd_tar;
    bool params_set;
} RefFilter;

static void ref_set_target_params(RefFilter *f, float hrir_coeff[2][HRTF_NUM_TAPS], float itd)
{
    f->itd_tar = fmaxf(-HRTF_MAX_DELAY_SAMPLES, fminf(itd, HRTF_MAX_DELAY_SAMPLES));
    for (int ch = 0; ch < 2; ch++) {    /* taps as written (xemu #3105) */
        memcpy(f->ch[ch].hrir_coeff_tar, hrir_coeff[ch], sizeof(f->ch[ch].hrir_coeff_tar));
    }
}

static void ref_settle(RefFilter *f)
{
    for (int ch = 0; ch < 2; ch++) {
        memcpy(f->ch[ch].hrir_coeff_cur, f->ch[ch].hrir_coeff_tar, sizeof(f->ch[ch].hrir_coeff_cur));
    }
    f->itd_cur = f->itd_tar;
}

static void ref_process(RefFilter *f, float in[HRTF_SAMPLES_PER_FRAME][2], float out[HRTF_SAMPLES_PER_FRAME][2])
{
    if (!f->params_set) {
        ref_settle(f);
        f->params_set = true;
    }
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        float w = n / (float)HRTF_SAMPLES_PER_FRAME;
        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;
            float *coeff_cur = f->ch[ch].hrir_coeff_cur;
            float *coeff_tar = f->ch[ch].hrir_coeff_tar;
            buf[f->buf_pos] = in[n][ch];
            /* Match the core's rounding by preventing FMA contraction. */
            float move = (f->itd_tar - f->itd_cur) * w;
            float itd = f->itd_cur + move;
            float d = itd * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) {
                d = 0.0f;
            }
            int di = d;
            float dfrac = d - di;
            float acc = 0.0f;
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                int idx1 = (f->buf_pos - di - k + REF_BUFLEN) % REF_BUFLEN;
                float s = buf[idx1];
                if (dfrac > 0.0f) {
                    int idx2 = (idx1 - 1 + REF_BUFLEN) % REF_BUFLEN;
                    s = s * (1 - dfrac) + buf[idx2] * dfrac;
                }
                acc += (coeff_cur[k] + (coeff_tar[k] - coeff_cur[k]) * w) * s;
            }
            out[n][ch] = acc;
        }
        f->buf_pos = (f->buf_pos + 1) % REF_BUFLEN;
    }
    ref_settle(f);
}

/* ---- the run */
static uint32_t g_rng = 12345;

static float rnd(void)   /* [-1, 1) */
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(int32_t)g_rng / 2147483648.0f;
}

static void random_hrir(float h[2][HRTF_NUM_TAPS])
{
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            h[ch][k] = rnd() * (k < 8 ? 1.0f : 0.25f);
        }
    }
}

static double g_max_err;

static void run(int frames, int retarget_every, const float *itds, int n_itds, bool settle)
{
    static RefFilter ref;
    static HrtfFilter f;
    memset(&ref, 0, sizeof(ref));
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS];
    int itd_i = 0;
    for (int fr = 0; fr < frames; fr++) {
        if (fr % retarget_every == 0 && !(settle && fr > 0)) {
            random_hrir(h);
            const float itd = itds[itd_i++ % n_itds];
            ref_set_target_params(&ref, h, itd);
            hrtf_filter_set_target_params(&f, h, itd);
        }
        float in[HRTF_SAMPLES_PER_FRAME][2], a[HRTF_SAMPLES_PER_FRAME][2], b[HRTF_SAMPLES_PER_FRAME][2];
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            in[n][0] = rnd();
            in[n][1] = rnd();
        }
        memcpy(b, in, sizeof(in));
        ref_process(&ref, in, a);
        hrtf_filter_process(&f, b, b);      /* in place, as the VP calls it */
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            for (int ch = 0; ch < 2; ch++) {
                /* relative to the output (up to ~10: taps reach their
                 * targets within a frame, the sums' order differs) */
                const double e = fabs((double)a[n][ch] - (double)b[n][ch]) / fmax(1.0, fabs(a[n][ch]));
                if (e > g_max_err) {
                    g_max_err = e;
                }
            }
        }
    }
    /* the parameters moved the same way */
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < HRTF_NUM_TAPS; k++) {
            HT_CHECK(fabsf(ref.ch[ch].hrir_coeff_cur[k] - f.ch[ch].hrir_coeff_cur[HRTF_NUM_TAPS - 1 - k]) < 1e-6f);
        }
        HT_CHECK(f.ch[ch].hrir_coeff_cur[HRTF_NUM_TAPS] == 0.0f);
    }
    HT_CHECK(fabsf(ref.itd_cur - f.itd_cur) < 1e-5f);
    HT_EQ_INT(f.buf_pos, ref.buf_pos);
}

/* ---- history (hrtf_filter_clear_history: a voice's new sound) */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* the parameters at their targets: the filter's fast path */
static void settle(HrtfFilter *f)
{
    for (int ch = 0; ch < 2; ch++) {
        memcpy(f->ch[ch].hrir_coeff_cur, f->ch[ch].hrir_coeff_tar, sizeof(f->ch[ch].hrir_coeff_cur));
    }
    f->itd_cur = f->itd_tar;
}

static void random_frame(float in[HRTF_SAMPLES_PER_FRAME][2])
{
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        in[n][0] = rnd();
        in[n][1] = rnd();
    }
}

static bool params_equal(const HrtfFilter *a, const HrtfFilter *b)
{
    for (int ch = 0; ch < 2; ch++) {
        if (memcmp(a->ch[ch].hrir_coeff_cur, b->ch[ch].hrir_coeff_cur, sizeof(a->ch[ch].hrir_coeff_cur)) ||
            memcmp(a->ch[ch].hrir_coeff_tar, b->ch[ch].hrir_coeff_tar, sizeof(a->ch[ch].hrir_coeff_tar))) {
            return false;
        }
    }
    return a->itd_cur == b->itd_cur && a->itd_tar == b->itd_tar;
}

/* A voice reused for a new sound after one that played through a delayed
 * impulse response: from silence, silence, not the previous sound's tail.
 * Without the clear (the old reset) its tail comes out. */
static void test_history_new_voice(void)
{
    static HrtfFilter f, stale;
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS] = {{0}};
    h[0][5] = 1.0f;
    h[1][9] = 1.0f;
    hrtf_filter_set_target_params(&f, h, 12.0f);
    settle(&f);
    float in[HRTF_SAMPLES_PER_FRAME][2], out[HRTF_SAMPLES_PER_FRAME][2];
    for (int fr = 0; fr < 3; fr++) {
        random_frame(in);
        hrtf_filter_process(&f, in, out);
    }
    stale = f;
    hrtf_filter_clear_history(&f);
    float zero[HRTF_SAMPLES_PER_FRAME][2] = {{0}};
    unsigned leaked = 0, nonzero = 0;
    for (int fr = 0; fr < 4; fr++) {
        float a[HRTF_SAMPLES_PER_FRAME][2], b[HRTF_SAMPLES_PER_FRAME][2];
        hrtf_filter_process(&f, zero, a);
        hrtf_filter_process(&stale, zero, b);
        for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
            nonzero += a[n][0] != 0.0f || a[n][1] != 0.0f;
            leaked += b[n][0] != 0.0f || b[n][1] != 0.0f;
        }
    }
    HT_EQ_INT(nonzero, 0);
    HT_CHECK(leaked > 0);       /* what the old reset left */
}

/* From every ring position (frames move it 32 of 73 at a time, so 73
 * frames visit each): the history, its mirror and its padding all zero, the
 * position at the start, the parameters as they were; and from there the
 * filter runs exactly as a clean one with the same parameters. */
static void test_history_every_position(void)
{
    static HrtfFilter f, c, clean;
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS];
    random_hrir(h);
    hrtf_filter_set_target_params(&f, h, 7.5f);
    settle(&f);
    unsigned seen = 0;
    uint8_t visited[HRTF_BUFLEN] = {0};
    for (int fr = 0; fr < HRTF_BUFLEN; fr++) {
        float in[HRTF_SAMPLES_PER_FRAME][2], a[HRTF_SAMPLES_PER_FRAME][2], b[HRTF_SAMPLES_PER_FRAME][2];
        random_frame(in);
        hrtf_filter_process(&f, in, a);
        if (!visited[f.buf_pos]) {
            visited[f.buf_pos] = 1;
            seen++;
        }
        c = f;
        hrtf_filter_clear_history(&c);
        HT_EQ_INT(c.buf_pos, 0);
        for (int ch = 0; ch < 2; ch++) {
            for (size_t i = 0; i < ARRAY_LEN(c.ch[ch].buf); i++) {
                HT_CHECK(c.ch[ch].buf[i] == 0.0f);
            }
        }
        HT_CHECK(params_equal(&c, &f));
        hrtf_filter_init(&clean);
        for (int ch = 0; ch < 2; ch++) {
            memcpy(clean.ch[ch].hrir_coeff_cur, c.ch[ch].hrir_coeff_cur, sizeof(clean.ch[ch].hrir_coeff_cur));
            memcpy(clean.ch[ch].hrir_coeff_tar, c.ch[ch].hrir_coeff_tar, sizeof(clean.ch[ch].hrir_coeff_tar));
        }
        clean.itd_cur = c.itd_cur;
        clean.itd_tar = c.itd_tar;
        random_frame(in);
        hrtf_filter_process(&c, in, a);
        hrtf_filter_process(&clean, in, b);
        HT_CHECK(memcmp(a, b, sizeof(a)) == 0);
    }
    HT_EQ_INT(seen, HRTF_BUFLEN);
}

/* Clearing history preserves a pending parameter ramp. */
static void test_history_keeps_moving_parameters(void)
{
    static HrtfFilter f, before;
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS];
    random_hrir(h);
    hrtf_filter_set_target_params(&f, h, -20.25f);
    float in[HRTF_SAMPLES_PER_FRAME][2], out[HRTF_SAMPLES_PER_FRAME][2];
    random_frame(in);
    hrtf_filter_process(&f, in, out);
    HT_CHECK(hrtf_filter_settled(&f));      /* a frame ends at the target */
    random_hrir(h);
    hrtf_filter_set_target_params(&f, h, 9.5f);
    HT_CHECK(!hrtf_filter_settled(&f));
    before = f;
    hrtf_filter_clear_history(&f);
    HT_CHECK(params_equal(&f, &before));
}

/* New targets for a voice that keeps playing (no VOICE_ON): its history
 * goes on, so the last frames' samples still sound through the delay */
static void test_retarget_keeps_history(void)
{
    static HrtfFilter f, before;
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS];
    random_hrir(h);
    hrtf_filter_set_target_params(&f, h, 3.0f);
    float in[HRTF_SAMPLES_PER_FRAME][2], out[HRTF_SAMPLES_PER_FRAME][2];
    for (int fr = 0; fr < 4; fr++) {
        random_frame(in);
        hrtf_filter_process(&f, in, out);
    }
    before = f;
    random_hrir(h);
    hrtf_filter_set_target_params(&f, h, -9.0f);
    HT_EQ_INT(f.buf_pos, before.buf_pos);
    for (int ch = 0; ch < 2; ch++) {
        HT_CHECK(memcmp(f.ch[ch].buf, before.ch[ch].buf, sizeof(f.ch[ch].buf)) == 0);
    }
    float zero[HRTF_SAMPLES_PER_FRAME][2] = {{0}};
    hrtf_filter_process(&f, zero, out);
    unsigned nonzero = 0;
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        nonzero += out[n][0] != 0.0f || out[n][1] != 0.0f;
    }
    HT_CHECK(nonzero > 0);
}

/* Check each ear's unnormalized taps and delay against an impulse response. */
static void impulse_response(const float h[2][HRTF_NUM_TAPS], float itd, float y[2][HRTF_SAMPLES_PER_FRAME])
{
    static HrtfFilter f;
    hrtf_filter_init(&f);
    float hh[2][HRTF_NUM_TAPS];
    memcpy(hh, h, sizeof(hh));
    hrtf_filter_set_target_params(&f, hh, itd);
    settle(&f);
    float in[HRTF_SAMPLES_PER_FRAME][2] = {{0}}, out[HRTF_SAMPLES_PER_FRAME][2];
    in[0][0] = in[0][1] = 1.0f;
    hrtf_filter_process(&f, in, out);
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        y[0][n] = out[n][0];
        y[1][n] = out[n][1];
    }
}

static void expect(const float y[HRTF_SAMPLES_PER_FRAME], const float want[HRTF_SAMPLES_PER_FRAME])
{
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        HT_CHECK(fabsf(y[n] - want[n]) < 1e-6f);
    }
}

static void test_coefficients_by_ear(void)
{
    float h[2][HRTF_NUM_TAPS], y[2][HRTF_SAMPLES_PER_FRAME], w0[HRTF_SAMPLES_PER_FRAME], w1[HRTF_SAMPLES_PER_FRAME];
    /* asymmetric: 2 at tap 0, 0.25 at taps 1 and 2 */
    memset(h, 0, sizeof(h));
    h[0][0] = 2.0f;
    h[1][1] = h[1][2] = 0.25f;
    impulse_response(h, 0.0f, y);
    memset(w0, 0, sizeof(w0));
    memset(w1, 0, sizeof(w1));
    w0[0] = 2.0f;
    w1[1] = w1[2] = 0.25f;
    expect(y[0], w0);
    expect(y[1], w1);
    /* the first ear silent; the second 3 at tap 3 */
    memset(h, 0, sizeof(h));
    h[1][3] = 3.0f;
    impulse_response(h, 0.0f, y);
    memset(w0, 0, sizeof(w0));
    memset(w1, 0, sizeof(w1));
    w1[3] = 3.0f;
    expect(y[0], w0);
    expect(y[1], w1);
    /* 0.5 at tap 0 and -0.5 at tap 4; 4 at tap 2 */
    memset(h, 0, sizeof(h));
    h[0][0] = 0.5f;
    h[0][4] = -0.5f;
    h[1][2] = 4.0f;
    impulse_response(h, 0.0f, y);
    memset(w0, 0, sizeof(w0));
    memset(w1, 0, sizeof(w1));
    w0[0] = 0.5f;
    w0[4] = -0.5f;
    w1[2] = 4.0f;
    expect(y[0], w0);
    expect(y[1], w1);
    /* a delay of +3 holds the first ear back 3 samples, not the second */
    memset(h, 0, sizeof(h));
    h[0][0] = 1.0f;
    h[1][0] = 1.0f;
    impulse_response(h, 3.0f, y);
    memset(w0, 0, sizeof(w0));
    memset(w1, 0, sizeof(w1));
    w0[3] = 1.0f;
    w1[0] = 1.0f;
    expect(y[0], w0);
    expect(y[1], w1);
}

/* A voice's first frame starts at its target; a new target is reached
 * across exactly one frame, linearly (measured on hardware) */
static void test_move_across_one_frame(void)
{
    static HrtfFilter f;
    hrtf_filter_init(&f);
    float h[2][HRTF_NUM_TAPS] = {{0}};
    h[0][0] = h[1][0] = 1.0f;
    hrtf_filter_set_target_params(&f, h, 0.0f);
    float dc[HRTF_SAMPLES_PER_FRAME][2], out[HRTF_SAMPLES_PER_FRAME][2];
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        dc[n][0] = dc[n][1] = 1.0f;
    }
    hrtf_filter_process(&f, dc, out);
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        HT_CHECK(out[n][0] == 1.0f && out[n][1] == 1.0f);
    }
    h[0][0] = h[1][0] = 0.5f;
    hrtf_filter_set_target_params(&f, h, 0.0f);
    hrtf_filter_process(&f, dc, out);
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        const float want = 1.0f - 0.5f * n / HRTF_SAMPLES_PER_FRAME;
        HT_CHECK(fabsf(out[n][0] - want) < 1e-6f && fabsf(out[n][1] - want) < 1e-6f);
    }
    hrtf_filter_process(&f, dc, out);
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        HT_CHECK(out[n][0] == 0.5f && out[n][1] == 0.5f);
    }
}

static void run_suite(void)
{
    g_max_err = 0;
    HT_RUN(test_history_new_voice);
    HT_RUN(test_history_every_position);
    HT_RUN(test_history_keeps_moving_parameters);
    HT_RUN(test_retarget_keeps_history);
    HT_RUN(test_coefficients_by_ear);
    HT_RUN(test_move_across_one_frame);
    const float moving[] = {0.0f, 7.25f, -13.5f, 42.0f, -42.0f, 41.9f, -0.3f, 30.0f, 100.0f, -100.0f};
    const float whole[] = {0.0f, 5.0f, -12.0f, 42.0f, -42.0f};
    run(400, 3, moving, 10, false);         /* parameters always moving */
    run(400, 40, moving, 10, false);        /* moving, then near their targets */
    run(3000, 1, whole, 5, true);           /* one target: settles (the fast path) */
    run(3000, 1, moving + 1, 1, true);      /* settles on a fractional delay */
    run(3000, 1, moving + 5, 1, true);      /* ... next to the delay limit */
    fprintf(stderr, "hrtf: max relative difference %.3g\n", g_max_err);
    HT_CHECK(g_max_err < 2e-6);
}

int main(void)
{
#if defined(__x86_64__)
    /* each vector level the host has (vp/simd.h) */
    for (int level = vp_simd_set(VP_SIMD_AVX2); level >= VP_SIMD_SCALAR; level--) {
        vp_simd_set(level);
        fprintf(stderr, "hrtf: %s\n", vp_simd_name(level));
        run_suite();
    }
#else
    run_suite();
#endif
    return ht_failures != 0;
}
