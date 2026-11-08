/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DSP56300 JIT bridge; the C interpreter remains available in every build. */
#ifndef NATIVE_APU_DSP_JIT_H
#define NATIVE_APU_DSP_JIT_H

#ifndef NC_HAVE_DSP_JIT
#define NC_HAVE_DSP_JIT 0
#endif

#if NC_HAVE_DSP_JIT
#include <dsp56300.h>
#include "dsp.h"

Dsp56300Jit *dsp_jit_create(DSPState *dsp);
void dsp_jit_run(DSPState *dsp, int cycles);
void dsp_jit_save(DSPState *dsp);
void dsp_jit_load(DSPState *dsp);
#endif

#endif
