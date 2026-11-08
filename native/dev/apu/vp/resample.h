/*
 * Voice resamplers pull 32 input frames per callback. A new sound resets
 * the phase, history and rate.
 *
 * Hardware (default): eight-point Lagrange interpolation with 1/65536
 * weights and a 16.16 cursor that can step by a different amount per sample.
 * Based on raddad772's console measurements in xemu #3105.
 * Sinc: 32-tap Kaiser-windowed filter with 16 to 129 frames of lookahead.
 * Linear: lower-cost interpolation.
 */
#ifndef NATIVE_DEV_APU_VP_RESAMPLE_H
#define NATIVE_DEV_APU_VP_RESAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct VpResampler VpResampler;
typedef long (*vp_resample_cb)(void *cb_data, float **data);

enum { VP_RESAMPLE_LINEAR = 0, VP_RESAMPLE_SINC = 1, VP_RESAMPLE_HARDWARE = 2 };

/* The sinc converter is in this build (AArch64) */
bool vp_resample_sinc_available(void);
const char *vp_resample_name(int quality);

/* Map a mode name to its enum value. Unknown names return -1. */
int vp_resample_quality_named(const char *name);

/* A resampler of the quality (linear where sinc isn't built in) for
 * channels channels, its input from cb; NULL when out of memory */
VpResampler *vp_resample_new(vp_resample_cb cb, int quality, int channels, void *cb_data);
/* Up to frames output frames at ratio (output rate / input rate) into
 * data; fewer only when the input has ended. Any positive ratio: the VP's
 * pitch with its envelope spans 1/512 .. 512 (NaN, 0 or less: 1) */
long vp_resample_read(VpResampler *r, double ratio, long frames, float *data);
/* Hardware mode advances by steps[n] after output frame n (16.16 input
 * frames). Other modes use a constant ratio. */
long vp_resample_read_steps(VpResampler *r, const uint32_t *steps, double ratio, long frames,
                            float *data);
/* The input frames the callback gave that the output hasn't reached yet,
 * and how far before its position an output still reads the input: a
 * sound whose input ended is fully played once its silence after the end
 * exceeds ahead + reach (vp.c voice_process) */
double vp_resample_ahead(const VpResampler *r);
double vp_resample_reach(const VpResampler *r);
void vp_resample_reset(VpResampler *r);
VpResampler *vp_resample_delete(VpResampler *r);    /* NULL */
int vp_resample_quality(const VpResampler *r);

/* float samples to 16-bit, clipped to [-1, 1], truncated (the monitor's) */
void vp_float_to_short_array(const float *in, short *out, int len);

#endif
