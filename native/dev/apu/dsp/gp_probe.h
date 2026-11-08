/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — a diagnostic probe of the guest's GP program
 * (gp_probe.c): NC_APU_GP_PROBE=DIR, with NC_APU_DSP=1. Off, each call
 * returns at once.
 */
#ifndef NATIVE_DEV_APU_DSP_GP_PROBE_H
#define NATIVE_DEV_APU_DSP_GP_PROBE_H

#include <stddef.h>
#include <stdint.h>

struct MCPXAPUState;

void gp_probe_init(void);
/* The frame's mix buffer (the GP's X:0x1400 words, 32 bins of 32 samples)
 * replaced with the probe's signal, before the GP runs */
void gp_probe_frame_begin(struct MCPXAPUState *d, uint32_t *mixbuf);
/* A DSP's output FIFO write, 'G' or 'E', as the guest's DMA gives it */
void gp_probe_fifo(char dsp, unsigned index, const uint8_t *ptr, size_t len);
/* After the GP and EP ran: the frame recorded */
void gp_probe_frame_end(struct MCPXAPUState *d);

#endif
