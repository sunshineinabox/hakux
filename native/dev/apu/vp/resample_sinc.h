/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the voices' band-limited resampler, resample.h's sinc
 * (resample_sinc.c): its taps through NEON on AArch64, through AVX2 + FMA or
 * SSE2 on x86-64 (simd.h). Elsewhere VP_HAVE_SINC is undefined and
 * resample.c resamples linearly.
 */
#ifndef NATIVE_DEV_APU_VP_RESAMPLE_SINC_H
#define NATIVE_DEV_APU_VP_RESAMPLE_SINC_H

#include "resample.h"

#if defined(__aarch64__) || defined(__x86_64__)
#define VP_HAVE_SINC 1

typedef struct VpSinc VpSinc;

/* A converter for 1 or 2 channels; NULL when out of memory */
VpSinc *vp_sinc_new(int channels);
/* frames output frames at ratio (positive and finite), its input from cb;
 * fewer only when cb gives nothing */
long vp_sinc_read(VpSinc *s, vp_resample_cb cb, void *cb_data, double ratio, long frames, float *data);
/* vp_resample_ahead, vp_resample_reach */
double vp_sinc_ahead(const VpSinc *s);
double vp_sinc_reach(const VpSinc *s);
void vp_sinc_reset(VpSinc *s);
void vp_sinc_delete(VpSinc *s);
#endif

#endif
