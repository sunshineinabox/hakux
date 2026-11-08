#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#define LINEAR_STEREO_SIMD 1
#elif defined(__x86_64__)
#include "simd.h"
#define LINEAR_STEREO_SIMD (vp_simd_level() >= VP_SIMD_SSE2)
#endif

#include "resample.h"
#include "resample_sinc.h"

/*
 * Linear-interpolation resampler stub used in place of libsamplerate.
 *
 * Per-refill malloc/memcpy/free was the bulk of the cost in profiling
 * (PGR2 race, 2026-06-01 ~3.4% host CPU). Replace with a fixed inline
 * buffer + memmove-compact on refill. The lerp inner loop gets a NEON
 * (SSE2 on x86-64) 2-channel specialization; ch=2 is the only used case
 * (vp.c:1255 hardcodes channels=2). Scalar fallback preserved for other
 * channel counts and other builds.
 *
 * Buffer sizing: the only producer is voice_resample_callback which
 * returns at most NUM_SAMPLES_PER_FRAME=32 frames per call. Worst-case
 * residue carried across a refill is < 1 frame (we compact whole
 * frames), so 2 * 32 = 64 frames is a safe upper bound. We use 128 to
 * give comfortable headroom and absorb any future bumps.
 */
#define STUB_MAX_CHANNELS 2
#define STUB_BUFFER_FRAMES 128

struct VpResampler {
    int quality;       /* VP_RESAMPLE_*: the linear fields below, sinc, or hardware */
    void *sinc;        /* resample_sinc.c's converter, a VpSinc (sinc) */
    vp_resample_cb cb;
    void *cb_data;
    int channels;

    /* Inline buffer. Layout: interleaved frames, frame i at
     * input_buf[i * channels + c]. After every refill, the live region
     * starts at index 0 and runs for input_len frames. */
    float input_buf[STUB_BUFFER_FRAMES * STUB_MAX_CHANNELS];
    int input_len;     /* frames currently live in input_buf */
    float input_pos;   /* fractional read cursor in frames; >=0, < input_len */

    /* Hardware mode keeps eight frames around the 16.16 cursor at interp[3].
     * hw_pos and hw_len track the callback buffer. */
    float interp[8][STUB_MAX_CHANNELS];
    uint32_t interp_pos;
    const float *hw_buf;
    int hw_len, hw_pos;
};

/* ---- hardware: 8-point Lagrange interpolation (xemu #3105, raddad772) */

/* Round Lagrange weights to 1/65536. Hardware probes match at eighth-sample
 * offsets. Interpolation between 1/256 steps is an approximation. */
static float g_lagrange[257][8];
static pthread_once_t g_lagrange_once = PTHREAD_ONCE_INIT;

static void lagrange_init(void)
{
    for (int r = 0; r <= 256; r++) {
        for (int k = -3; k <= 4; k++) {
            double w = 1.0;
            for (int j = -3; j <= 4; j++) {
                if (j != k) {
                    w *= (r / 256.0 - j) / (k - j);
                }
            }
            g_lagrange[r][k + 3] = round(w * 65536.0) / 65536.0;
        }
    }
}

static void hardware_reset(VpResampler *r)
{
    memset(r->interp, 0, sizeof(r->interp));
    r->interp_pos = 5 << 16;            /* the first output is the first frame */
    r->hw_buf = NULL;
    r->hw_len = r->hw_pos = 0;
}

static long hardware_read(VpResampler *r, const uint32_t *steps, double ratio, long frames,
                          float *data)
{
    const int ch = r->channels;
    const uint32_t step = (uint32_t)(65536.0 / ratio);
    for (long n = 0; n < frames; n++) {
        while (r->interp_pos >= 0x10000) {
            if (r->hw_pos >= r->hw_len) {
                float *in = NULL;
                long got = r->cb(r->cb_data, &in);
                if (got <= 0 || !in) {
                    return n;
                }
                r->hw_buf = in;
                r->hw_len = (int)got;
                r->hw_pos = 0;
            }
            memmove(r->interp[0], r->interp[1], sizeof(r->interp[0]) * 7);
            for (int c = 0; c < ch; c++) {
                r->interp[7][c] = r->hw_buf[r->hw_pos * ch + c];
            }
            r->hw_pos++;
            r->interp_pos -= 0x10000;
        }
        const uint32_t frac = r->interp_pos;
        const float *lo = g_lagrange[frac >> 8], *hi = g_lagrange[(frac >> 8) + 1];
        const float a = (frac & 0xFF) / 256.0f;
        for (int c = 0; c < ch; c++) {
            float acc = r->interp[3][c];
            if (frac) {
                acc = 0.0f;
                for (int k = 0; k < 8; k++) {
                    acc += (lo[k] + (hi[k] - lo[k]) * a) * r->interp[k][c];
                }
            }
            data[n * ch + c] = acc;
        }
        r->interp_pos += steps ? steps[n] : step;
    }
    return frames;
}

static VpResampler *linear_new(vp_resample_cb cb, int channels, void *cb_data)
{
    if (channels < 1 || channels > STUB_MAX_CHANNELS) {
        return NULL;
    }
    VpResampler *state = (VpResampler *)calloc(1, sizeof(*state));
    if (!state) {
        return NULL;
    }
    state->cb = cb;
    state->cb_data = cb_data;
    state->channels = channels;
    state->input_len = 0;
    state->input_pos = 0.0f;
    return state;
}

/*
 * Compact already-consumed frames out of the buffer and pull new data
 * from the callback.
 *
 * Returns 1 if the buffer now has at least 2 frames (the minimum
 * needed to produce a single interpolated output), 0 otherwise.
 */
static int src_refill_input(VpResampler *state)
{
    int ch = state->channels;

    /* Number of whole frames already consumed. The fractional part of
     * input_pos stays with us; we will rebase it onto frame 0. */
    int consumed = (int)state->input_pos;
    if (consumed < 0) {
        consumed = 0;
    }
    if (consumed > state->input_len) {
        consumed = state->input_len;
    }

    int residue = state->input_len - consumed;
    if (residue < 0) {
        residue = 0;
    }

    /* Compact residue to slot 0. memmove because the regions may
     * overlap when consumed < residue. */
    if (residue > 0 && consumed > 0) {
        memmove(state->input_buf,
                state->input_buf + consumed * ch,
                sizeof(float) * residue * ch);
    }
    state->input_len = residue;
    state->input_pos -= (float)consumed;
    if (state->input_pos < 0.0f) {
        state->input_pos = 0.0f;
    }

    /* Pull new data. Caller (voice_resample_callback) returns at most
     * NUM_SAMPLES_PER_FRAME=32 frames; the buffer always has room. */
    float *new_data = NULL;
    long got = state->cb(state->cb_data, &new_data);
    if (got <= 0 || new_data == NULL) {
        /* End-of-stream: no new data. Caller can still drain whatever
         * residue we have (>=2 frames required for one lerp output). */
        return residue >= 2;
    }

    int max_new = STUB_BUFFER_FRAMES - residue;
    if (got > max_new) {
        /* Defensive clamp; the callsite never produces this in
         * practice, but we must not overrun the inline buffer. */
        got = max_new;
    }

    if (got > 0) {
        memcpy(state->input_buf + residue * ch,
               new_data,
               sizeof(float) * (size_t)got * (size_t)ch);
        state->input_len = residue + (int)got;
    }

    return state->input_len >= 2;
}

static long linear_read(VpResampler *state, double ratio, long frames, float *data)
{
    int ch = state->channels;
    float step = (float)(1.0 / ratio);
    long out_frames = 0;

#ifdef LINEAR_STEREO_SIMD
    if (ch == 2 && LINEAR_STEREO_SIMD) {
        /* Fast path: 2-channel NEON lerp (SSE2: a multiply and an add, as
         * the x86-64 scalar form, bit for bit). One float32x2_t FMA per
         * output frame. Bit-identical to the scalar form
         *   out[c] = s0[c] + frac * (s1[c] - s0[c])
         * because vfma_n_f32(a, b, n) computes a + b*n with a single
         * rounding (same as the scalar expression after CSE on this
         * target's IEEE-754 FMA-or-not behavior — both forms compile
         * to the same FMLA on AArch64 with -ffp-contract=fast, which
         * is the project default; otherwise both produce the same
         * unfused mul+add). */
        float pos = state->input_pos;
        while (out_frames < frames) {
            int need = (int)pos + 1;
            if (need >= state->input_len) {
                /* Flush pos back to state before refill so refill's
                 * consumed-frame math sees the latest cursor. */
                state->input_pos = pos;
                if (!src_refill_input(state)) {
                    break;
                }
                pos = state->input_pos;
                if (state->input_len < 2) {
                    break;
                }
                continue;
            }

            int idx0 = (int)pos;
            int idx1 = idx0 + 1;
            if (idx1 >= state->input_len) {
                /* Need refill — same path as above. */
                state->input_pos = pos;
                if (!src_refill_input(state)) {
                    break;
                }
                pos = state->input_pos;
                if (state->input_len < 2) {
                    break;
                }
                continue;
            }

            float frac = pos - (float)idx0;
            const float *base = state->input_buf;
#if defined(__aarch64__)
            float32x2_t s0 = vld1_f32(base + idx0 * 2);
            float32x2_t s1 = vld1_f32(base + idx1 * 2);
            float32x2_t d  = vsub_f32(s1, s0);
            float32x2_t r  = vfma_n_f32(s0, d, frac);
            vst1_f32(data + out_frames * 2, r);
#else
            const __m128 s0 = _mm_castpd_ps(_mm_load_sd((const double *)(base + idx0 * 2)));
            const __m128 s1 = _mm_castpd_ps(_mm_load_sd((const double *)(base + idx1 * 2)));
            const __m128 r = _mm_add_ps(s0, _mm_mul_ps(_mm_set1_ps(frac), _mm_sub_ps(s1, s0)));
            _mm_store_sd((double *)(data + out_frames * 2), _mm_castps_pd(r));
#endif

            out_frames++;
            pos += step;
        }
        state->input_pos = pos;
        return out_frames;
    }
#endif

    /* Scalar fallback: any channel count, other builds. */
    while (out_frames < frames) {
        int need = (int)state->input_pos + 1;
        if (need >= state->input_len) {
            if (!src_refill_input(state)) {
                break;
            }
            if (state->input_len < 2) {
                break;
            }
            continue;
        }

        int idx0 = (int)state->input_pos;
        int idx1 = idx0 + 1;
        if (idx1 >= state->input_len) {
            if (!src_refill_input(state)) {
                break;
            }
            if (state->input_len < 2) {
                break;
            }
            continue;
        }

        float frac = state->input_pos - (float)idx0;
        const float *s0 = state->input_buf + idx0 * ch;
        const float *s1 = state->input_buf + idx1 * ch;

        for (int c = 0; c < ch; c++) {
            data[out_frames * ch + c] = s0[c] + frac * (s1[c] - s0[c]);
        }

        out_frames++;
        state->input_pos += step;
    }

    return out_frames;
}

/* ---- the qualities (resample.h) */

bool vp_resample_sinc_available(void)
{
#ifdef VP_HAVE_SINC
    return true;
#else
    return false;
#endif
}

int vp_resample_quality_named(const char *name)
{
    return !name                      ? -1 :
           !strcmp(name, "hardware")  ? VP_RESAMPLE_HARDWARE :
           !strcmp(name, "sinc")      ? VP_RESAMPLE_SINC :
           !strcmp(name, "linear")    ? VP_RESAMPLE_LINEAR : -1;
}

const char *vp_resample_name(int quality)
{
    if (quality == VP_RESAMPLE_HARDWARE) {
        return "hardware (8-point Lagrange)";
    }
    if (quality != VP_RESAMPLE_SINC) {
        return "linear";
    }
#if defined(__x86_64__)
    static const char *const names[] = {"sinc (32-tap Kaiser-windowed, scalar)",
                                        "sinc (32-tap Kaiser-windowed, SSE2)",
                                        "sinc (32-tap Kaiser-windowed, AVX2+FMA)"};
    return names[vp_simd_level()];
#else
    return "sinc (32-tap Kaiser-windowed, NEON)";
#endif
}

VpResampler *vp_resample_new(vp_resample_cb cb, int quality, int channels, void *cb_data)
{
    VpResampler *r = linear_new(cb, channels, cb_data);
    if (r && quality == VP_RESAMPLE_HARDWARE) {
        pthread_once(&g_lagrange_once, lagrange_init);
        r->quality = VP_RESAMPLE_HARDWARE;
        hardware_reset(r);
        return r;
    }
#ifdef VP_HAVE_SINC
    if (r && quality == VP_RESAMPLE_SINC) {
        r->sinc = vp_sinc_new(channels);
        if (!r->sinc) {
            free(r);
            return NULL;
        }
        r->quality = VP_RESAMPLE_SINC;
    }
#else
    (void)quality;
#endif
    return r;
}

long vp_resample_read(VpResampler *r, double ratio, long frames, float *data)
{
    return vp_resample_read_steps(r, NULL, ratio, frames, data);
}

long vp_resample_read_steps(VpResampler *r, const uint32_t *steps, double ratio, long frames,
                            float *data)
{
    if (!r || !r->cb || !data || frames <= 0) {
        return 0;
    }
    if (!(ratio > 0.0) || !isfinite(ratio)) {
        ratio = 1.0;
    }
    if (r->quality == VP_RESAMPLE_HARDWARE) {
        return hardware_read(r, steps, ratio, frames, data);
    }
#ifdef VP_HAVE_SINC
    if (r->sinc) {
        return vp_sinc_read(r->sinc, r->cb, r->cb_data, ratio, frames, data);
    }
#endif
    return linear_read(r, ratio, frames, data);
}

double vp_resample_ahead(const VpResampler *r)
{
    if (r->quality == VP_RESAMPLE_HARDWARE) {
        /* Count the current frame, four future frames and unread input,
         * minus the cursor's offset from interp[3]. */
        return (r->hw_len - r->hw_pos) + 5.0 - r->interp_pos / 65536.0;
    }
#ifdef VP_HAVE_SINC
    if (r->sinc) {
        return vp_sinc_ahead(r->sinc);
    }
#endif
    return r->input_len - r->input_pos;
}

/* The hardware window reaches three frames behind interp[3]. */
double vp_resample_reach(const VpResampler *r)
{
    if (r->quality == VP_RESAMPLE_HARDWARE) {
        return 3.0 + (r->interp_pos & 0xFFFF) / 65536.0;
    }
#ifdef VP_HAVE_SINC
    if (r->sinc) {
        return vp_sinc_reach(r->sinc);
    }
#endif
    return 0.0;
}

void vp_resample_reset(VpResampler *r)
{
    if (!r) {
        return;
    }
#ifdef VP_HAVE_SINC
    if (r->sinc) {
        vp_sinc_reset(r->sinc);
    }
#endif
    r->input_len = 0;
    r->input_pos = 0.0f;
    if (r->quality == VP_RESAMPLE_HARDWARE) {
        hardware_reset(r);
    }
}

VpResampler *vp_resample_delete(VpResampler *r)
{
#ifdef VP_HAVE_SINC
    if (r && r->sinc) {
        vp_sinc_delete(r->sinc);
    }
#endif
    free(r);
    return NULL;
}

int vp_resample_quality(const VpResampler *r)
{
    return r ? r->quality : VP_RESAMPLE_LINEAR;
}

void vp_float_to_short_array(const float *in, short *out, int len)
{
    if (!in || !out || len <= 0) {
        return;
    }

#if defined(__aarch64__)
    float32x4_t scale = vdupq_n_f32(32767.0f);
    float32x4_t hi = vdupq_n_f32(1.0f);
    float32x4_t lo = vdupq_n_f32(-1.0f);
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        float32x4_t v = vld1q_f32(&in[i]);
        v = vminq_f32(vmaxq_f32(v, lo), hi);
        v = vmulq_f32(v, scale);
        int32x4_t iv = vcvtq_s32_f32(v);
        int16x4_t sv = vqmovn_s32(iv);
        vst1_s16(&out[i], sv);
    }
    for (; i < len; i++) {
        float v = in[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        out[i] = (short)(v * 32767.0f);
    }
#elif defined(__x86_64__)
    /* SSE2, as NEON's (and the scalar form's): clipped, scaled, truncated;
     * a NaN 0 */
    int i = 0;
    if (vp_simd_level() >= VP_SIMD_SSE2) {
        const __m128 scale = _mm_set1_ps(32767.0f), hi = _mm_set1_ps(1.0f), lo = _mm_set1_ps(-1.0f);
        for (; i + 8 <= len; i += 8) {
            __m128 a = _mm_loadu_ps(&in[i]), b = _mm_loadu_ps(&in[i + 4]);
            a = _mm_and_ps(a, _mm_cmpord_ps(a, a));
            b = _mm_and_ps(b, _mm_cmpord_ps(b, b));
            a = _mm_mul_ps(_mm_min_ps(_mm_max_ps(a, lo), hi), scale);
            b = _mm_mul_ps(_mm_min_ps(_mm_max_ps(b, lo), hi), scale);
            _mm_storeu_si128((__m128i *)&out[i], _mm_packs_epi32(_mm_cvttps_epi32(a), _mm_cvttps_epi32(b)));
        }
    }
    for (; i < len; ++i) {
        float v = in[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = v == v ? (short)(v * 32767.0f) : 0;
    }
#else
    for (int i = 0; i < len; ++i) {
        float v = in[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = (short)(v * 32767.0f);
    }
#endif
}
