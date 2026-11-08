/*
 * Inline voice mixer helpers for MCPX APU voice processor.
 *
 * Vectorizes the per-voice multiply-add into mixbins (vp.c::voice_process inner
 * loop) using NEON FMA on aarch64, SSE2 or AVX2 + FMA on x86-64 (simd.h);
 * falls back to a scalar reference on other targets. The fused and scalar
 * paths produce sub-LSB-different results because vfmaq_f32 is a
 * single-rounding fused multiply-add while the scalar path multiplies
 * then adds (two roundings). This precision difference is far below human
 * audible thresholds for the gain values used here (typically 0..2).
 *
 * The header is self-contained: it defines NUM_SAMPLES_PER_FRAME locally if
 * not already defined by the surrounding build (apu_regs.h provides it in the
 * QEMU build; tests provide nothing, so the local definition takes effect).
 *
 * See: docs/superpowers/specs/2026-04-28-apu-mixer-neon-design.md
 */
#ifndef HW_XBOX_MCPX_APU_VP_MIX_INLINE_H
#define HW_XBOX_MCPX_APU_VP_MIX_INLINE_H

#ifndef NUM_SAMPLES_PER_FRAME
#define NUM_SAMPLES_PER_FRAME 32
#endif

/* NEON loop steps by 4; this static_assert prevents past-end writes if a
 * future change to apu_regs.h desyncs NUM_SAMPLES_PER_FRAME. */
_Static_assert(NUM_SAMPLES_PER_FRAME % 4 == 0,
               "NUM_SAMPLES_PER_FRAME must be a multiple of 4 for NEON loop");

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include "simd.h"
#endif

/* Reference implementation, two-rounding semantics. Used as both the
 * fallback for non-aarch64 builds and the host unit test's ground truth. */
static inline void mix_one_bin_scalar(float *dst, const float *src, float g) {
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        dst[i] += g * src[i];
    }
}

/* Mix a route with a separate gain for each sample. */
static inline void mix_one_bin_ramp_scalar(float *dst, const float *src, const float *g) {
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        dst[i] += g[i] * src[i];
    }
}

#if defined(__x86_64__)
static inline void mix_one_bin_ramp_sse2(float *dst, const float *src, const float *g) {
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 4) {
        const __m128 d = _mm_loadu_ps(dst + i);
        _mm_storeu_ps(dst + i, _mm_add_ps(d, _mm_mul_ps(_mm_loadu_ps(g + i), _mm_loadu_ps(src + i))));
    }
}

static inline VP_AVX2 void mix_one_bin_ramp_avx2(float *dst, const float *src, const float *g) {
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 8) {
        const __m256 d = _mm256_loadu_ps(dst + i);
        _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(_mm256_loadu_ps(g + i), _mm256_loadu_ps(src + i), d));
    }
}

/* SSE2: the scalar's multiply, then add (bit for bit) */
static inline void mix_one_bin_sse2(float *dst, const float *src, float g) {
    const __m128 gv = _mm_set1_ps(g);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 4) {
        const __m128 d = _mm_loadu_ps(dst + i);
        _mm_storeu_ps(dst + i, _mm_add_ps(d, _mm_mul_ps(gv, _mm_loadu_ps(src + i))));
    }
}

/* AVX2 + FMA: NEON's fused multiply-add (bit for bit) */
static inline VP_AVX2 void mix_one_bin_avx2(float *dst, const float *src, float g) {
    const __m256 gv = _mm256_set1_ps(g);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 8) {
        const __m256 d = _mm256_loadu_ps(dst + i);
        _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(gv, _mm256_loadu_ps(src + i), d));
    }
}
#endif

/* Hot-path dispatcher: NEON FMA on aarch64; on x86-64 AVX2 + FMA or SSE2
 * (simd.h); scalar everywhere else. */
static inline void mix_one_bin(float *dst, const float *src, float g) {
#if defined(__aarch64__)
    float32x4_t gv = vdupq_n_f32(g);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 4) {
        float32x4_t d = vld1q_f32(dst + i);
        float32x4_t s = vld1q_f32(src + i);
        d = vfmaq_f32(d, gv, s);
        vst1q_f32(dst + i, d);
    }
#elif defined(__x86_64__)
    const int level = vp_simd_level();
    if (level == VP_SIMD_AVX2) {
        mix_one_bin_avx2(dst, src, g);
    } else if (level == VP_SIMD_SSE2) {
        mix_one_bin_sse2(dst, src, g);
    } else {
        mix_one_bin_scalar(dst, src, g);
    }
#else
    mix_one_bin_scalar(dst, src, g);
#endif
}

static inline void mix_one_bin_ramp(float *dst, const float *src, const float *g) {
#if defined(__aarch64__)
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i += 4) {
        vst1q_f32(dst + i, vfmaq_f32(vld1q_f32(dst + i), vld1q_f32(g + i), vld1q_f32(src + i)));
    }
#elif defined(__x86_64__)
    const int level = vp_simd_level();
    if (level == VP_SIMD_AVX2) {
        mix_one_bin_ramp_avx2(dst, src, g);
    } else if (level == VP_SIMD_SSE2) {
        mix_one_bin_ramp_sse2(dst, src, g);
    } else {
        mix_one_bin_ramp_scalar(dst, src, g);
    }
#else
    mix_one_bin_ramp_scalar(dst, src, g);
#endif
}

#endif /* HW_XBOX_MCPX_APU_VP_MIX_INLINE_H */
