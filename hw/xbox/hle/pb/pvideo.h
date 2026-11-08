/* Copyright (C) 2026-present PortareOS */
/* Decode PVIDEO buffer 0 registers for display composition. */
#ifndef HW_XBOX_HLE_PB_PVIDEO_H
#define HW_XBOX_HLE_PB_PVIDEO_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define HLE_PVIDEO_UNITY (1u << 20)     /* DS_DX / DT_DY: one input texel per output pixel */
#define HLE_PVIDEO_FORMAT_COLOR_KEY (1u << 20)
#define HLE_PVIDEO_FORMAT_BT709 (1u << 24)
#define HLE_PVIDEO_ONE 0x1000           /* LUMINANCE's contrast, CHROMINANCE's: 1.0 */

typedef struct HlePvideoRegs {
    uint32_t size_in, point_in, ds_dx, dt_dy, point_out, size_out, format, color_key;
    uint32_t luminance, chrominance;
} HlePvideoRegs;

/* Overlay colour controls, with HLE_PVIDEO_ONE representing 1.0. */
typedef struct HlePvideoColor {
    int32_t contrast, brightness, sat_cos, sat_sin;
    bool bt709;
} HlePvideoColor;

typedef struct HlePvideo {
    uint32_t in_w, in_h;                /* input texels read from the buffer */
    float in_x, in_y;                   /* where the output's top left samples (POINT_IN) */
    float scale_x, scale_y;             /* input texels per output pixel */
    uint32_t out_x, out_y, out_w, out_h;    /* screen pixels */
    bool color_key_on;
    uint32_t color_key;                 /* a framebuffer pixel's raw value */
    HlePvideoColor color;
} HlePvideo;

/* DS_DX / DT_DY: input texels per output pixel, with 20 fractional bits. */
static inline float hle_pvideo_scale(uint32_t din_dout, uint32_t out)
{
    if (din_dout == HLE_PVIDEO_UNITY || out == 0) {
        return 1.0f;
    }
    const double in = floor((double)din_dout * (out - 1) / HLE_PVIDEO_UNITY + 0.5);
    return (float)((in + 1.0) / out);
}

/* Upload the input extent sampled by the output, bounded by SIZE_IN. */
static inline uint32_t hle_pvideo_in_extent(uint32_t size_in, float in_pos, uint32_t out,
                                            float scale)
{
    const float need = ceilf(in_pos + (float)out * scale);
    return need < (float)size_in ? (uint32_t)need : size_in;
}

/* POINT_IN is 12.4 fixed point; the low Y fraction bit is unused.
 * LUMINANCE: contrast (low 16), signed brightness (high 16).
 * CHROMINANCE: signed saturation*cos (low 16), saturation*sin (high 16). */
static inline HlePvideo hle_pvideo_decode(const HlePvideoRegs *r)
{
    HlePvideo p;
    p.out_x = r->point_out & 0xFFF;
    p.out_y = (r->point_out >> 16) & 0xFFF;
    p.out_w = r->size_out & 0xFFF;
    p.out_h = (r->size_out >> 16) & 0xFFF;
    p.scale_x = hle_pvideo_scale(r->ds_dx, p.out_w);
    p.scale_y = hle_pvideo_scale(r->dt_dy, p.out_h);
    p.in_x = (float)(r->point_in & 0x7FFF) / 16.0f;
    p.in_y = (float)((r->point_in >> 16) & 0xFFFE) / 16.0f;
    p.in_w = hle_pvideo_in_extent(r->size_in & 0x7FF, p.in_x, p.out_w, p.scale_x);
    p.in_h = hle_pvideo_in_extent((r->size_in >> 16) & 0x7FF, p.in_y, p.out_h, p.scale_y);
    p.color_key_on = (r->format & HLE_PVIDEO_FORMAT_COLOR_KEY) != 0;
    p.color_key = r->color_key;
    p.color.contrast = (int32_t)(r->luminance & 0xFFFF);
    p.color.brightness = (int16_t)(r->luminance >> 16);
    p.color.sat_cos = (int16_t)(r->chrominance & 0xFFFF);
    p.color.sat_sin = (int16_t)(r->chrominance >> 16);
    p.color.bt709 = (r->format & HLE_PVIDEO_FORMAT_BT709) != 0;
    return p;
}

/* Default D3D overlay settings preserve BT.601 conversion. */
static inline bool hle_pvideo_color_default(const HlePvideoColor *c)
{
    return c->contrast == HLE_PVIDEO_ONE && c->brightness == 0 && c->sat_cos == HLE_PVIDEO_ONE &&
           c->sat_sin == 0 && !c->bt709;
}

static inline uint8_t hle_pvideo_clip8(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* Limited-range YUV to BGR, using fixed-point BT.601 or BT.709.
 * Brightness step and hue direction have not been verified on hardware. */
static inline void hle_pvideo_yuv(const HlePvideoColor *c, int32_t Y, int32_t U, int32_t V,
                                  uint8_t bgr[3])
{
    const int32_t y = Y - 16, u = U - 128, v = V - 128;
    /* in 1/256 levels */
    const int32_t y8 = (y * c->contrast + c->brightness * 1024) / 16;
    const int32_t u8 = (u * c->sat_cos - v * c->sat_sin) / 16;
    const int32_t v8 = (u * c->sat_sin + v * c->sat_cos) / 16;
    const int32_t l = 298 * y8 + 32768;
    if (c->bt709) {
        bgr[0] = hle_pvideo_clip8((l + 541 * u8) >> 16);
        bgr[1] = hle_pvideo_clip8((l - 55 * u8 - 136 * v8) >> 16);
        bgr[2] = hle_pvideo_clip8((l + 459 * v8) >> 16);
    } else {
        bgr[0] = hle_pvideo_clip8((l + 516 * u8) >> 16);
        bgr[1] = hle_pvideo_clip8((l - 100 * u8 - 208 * v8) >> 16);
        bgr[2] = hle_pvideo_clip8((l + 409 * v8) >> 16);
    }
}

/* Convert YUY2 / UYVY to BGRA. NULL colour uses the original BT.601 path.
 * Odd widths use the last pair's first Y and shared chroma. */
static inline void hle_pvideo_convert(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h,
                                      bool uyvy, const HlePvideoColor *color, uint8_t *dst)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *l = src + y * pitch;
        uint8_t *q = dst + y * w * 4;
        for (uint32_t x = 0; x < w; x += 2, l += 4, q += 8) {
            const int32_t yy[2] = { uyvy ? l[1] : l[0], uyvy ? l[3] : l[2] };
            const int32_t u = uyvy ? l[0] : l[1], v = uyvy ? l[2] : l[3];
            for (uint32_t k = 0; k < 2 && x + k < w; k++) {
                if (color) {
                    hle_pvideo_yuv(color, yy[k], u, v, q + 4 * k);
                } else {
                    const int32_t c = 298 * (yy[k] - 16), uu = u - 128, vv = v - 128;
                    q[4 * k + 0] = hle_pvideo_clip8((c + 516 * uu + 128) >> 8);
                    q[4 * k + 1] = hle_pvideo_clip8((c - 100 * uu - 208 * vv + 128) >> 8);
                    q[4 * k + 2] = hle_pvideo_clip8((c + 409 * vv + 128) >> 8);
                }
                q[4 * k + 3] = 255;
            }
        }
    }
}

#endif
