/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hakuX native core — the VP's vector level on x86-64 (simd.h) */
#include "simd.h"

#if defined(__x86_64__)
#include <stdlib.h>
#include <string.h>

int g_vp_simd = -1;

static int cpu_level(void)
{
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") ? VP_SIMD_AVX2 : VP_SIMD_SSE2;
}

int vp_simd_init(void)
{
    int level = cpu_level();
    const char *cap = getenv("NC_APU_SIMD");
    if (cap && !strcmp(cap, "scalar")) {
        level = VP_SIMD_SCALAR;
    } else if (cap && !strcmp(cap, "sse2")) {
        level = VP_SIMD_SSE2;
    }
    __atomic_store_n(&g_vp_simd, level, __ATOMIC_RELAXED);
    return level;
}

int vp_simd_set(int level)
{
    const int cpu = cpu_level();
    level = level < VP_SIMD_SCALAR ? VP_SIMD_SCALAR : level > cpu ? cpu : level;
    __atomic_store_n(&g_vp_simd, level, __ATOMIC_RELAXED);
    return level;
}

const char *vp_simd_name(int level)
{
    return level == VP_SIMD_AVX2 ? "AVX2+FMA" : level == VP_SIMD_SSE2 ? "SSE2" : "scalar";
}
#endif
