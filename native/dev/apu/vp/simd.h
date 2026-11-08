/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the VP's vector paths on x86-64 (AArch64 has NEON, at
 * build time). SSE2 is x86-64's baseline, always there; AVX2 with FMA
 * (x86-64-v3: Haswell, Zen and later) is taken where the CPU has both,
 * checked at run time, so the core runs on any x86-64 CPU. The FMA paths
 * round as NEON's do: the same samples as on ARM64. NC_APU_SIMD=sse2 or
 * scalar caps the level at boot.
 */
#ifndef NATIVE_DEV_APU_VP_SIMD_H
#define NATIVE_DEV_APU_VP_SIMD_H

#if defined(__x86_64__)
#include <immintrin.h>

enum { VP_SIMD_SCALAR, VP_SIMD_SSE2, VP_SIMD_AVX2 };

/* a function of the AVX2 + FMA paths (called only at VP_SIMD_AVX2) */
#define VP_AVX2 __attribute__((target("avx2,fma")))

extern int g_vp_simd;           /* the level; -1 until vp_simd_init */

/* The CPU's level capped by NC_APU_SIMD (the VP's start, each boot) */
int vp_simd_init(void);
/* tests, benches: the level, at most the CPU's; returns it */
int vp_simd_set(int level);
const char *vp_simd_name(int level);

static inline int vp_simd_level(void)
{
    const int level = __atomic_load_n(&g_vp_simd, __ATOMIC_RELAXED);
    return level >= 0 ? level : vp_simd_init();
}

/* NEON's vaddvq_f32: (v0 + v1) + (v2 + v3) */
static inline float vp_sse_sum(__m128 v)
{
    const __m128 s = _mm_add_ps(v, _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtss_f32(_mm_add_ss(s, _mm_movehl_ps(s, s)));
}

/* NEON's vaddvq_f32(vaddq_f32(lo, hi)) of a register's halves */
static inline VP_AVX2 float vp_avx_sum(__m256 v)
{
    return vp_sse_sum(_mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1)));
}
#endif

#endif
