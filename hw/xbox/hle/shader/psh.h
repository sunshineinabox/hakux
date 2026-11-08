/*
 * hakuX HLE GPU — Xbox pixel shaders (NV2A register combiners and texture
 * modes) -> GLSL 4.50 (spec §5.8; pure). The key is the pixel-shader
 * render-state block (5659 render states 0..56, which SetPixelShader loads
 * from the D3DPIXELSHADERDEF); the combiner constants are push constants.
 *
 * Interface: inputs location 0 vDiffuse, 1 vSpecular, 2..5 vTex0..3, 6 vFog
 * (as vsh.h / ff.h); set 0 bindless images and samplers; HleFfPush.
 */
#ifndef HW_XBOX_HLE_SHADER_PSH_H
#define HW_XBOX_HLE_SHADER_PSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* D3DPIXELSHADERDEF / render-state indices (5659: D3DRS_PSALPHAINPUTS0 = 0) */
enum {
    HLE_PS_ALPHA_IN = 0,                /* [8] */
    HLE_PS_FINAL_ABCD = 8,
    HLE_PS_FINAL_EFG = 9,
    HLE_PS_C0 = 10,                     /* [8] */
    HLE_PS_C1 = 18,                     /* [8] */
    HLE_PS_ALPHA_OUT = 26,              /* [8] */
    HLE_PS_RGB_IN = 34,                 /* [8] */
    HLE_PS_COMPARE = 42,
    HLE_PS_FINAL_C0 = 43,
    HLE_PS_FINAL_C1 = 44,
    HLE_PS_RGB_OUT = 45,                /* [8] */
    HLE_PS_COUNT = 53,
    HLE_PS_TEXMODES = 54,
    HLE_PS_DOTMAP = 55,
    HLE_PS_INPUTTEX = 56,
    HLE_PS_REGS = 57,
};

/* PSTextureModes values */
enum {
    HLE_PST_NONE = 0, HLE_PST_PROJECT2D, HLE_PST_PROJECT3D, HLE_PST_CUBEMAP,
    HLE_PST_PASSTHRU, HLE_PST_CLIPPLANE, HLE_PST_BUMPENVMAP, HLE_PST_BUMPENVMAP_LUM,
    HLE_PST_BRDF, HLE_PST_DOT_ST, HLE_PST_DOT_ZW, HLE_PST_DOT_RFLCT_DIFF,
    HLE_PST_DOT_RFLCT_SPEC, HLE_PST_DOT_STR_3D, HLE_PST_DOT_STR_CUBE, HLE_PST_DPNDNT_AR,
    HLE_PST_DPNDNT_GB, HLE_PST_DOTPRODUCT, HLE_PST_DOT_RFLCT_SPEC_CONST,
};

typedef struct HlePshKey {
    uint32_t rgb_in[8], rgb_out[8], alpha_in[8], alpha_out[8];
    uint32_t final_abcd, final_efg;
    uint32_t count;                     /* PSCombinerCount */
    uint32_t tex_modes;                 /* Texture modes in bits 0–19. Three clamp bits per stage above them. */
    uint32_t dot_map;                   /* PSDotMapping */
    uint32_t input_tex;                 /* PSInputTexture */
    uint32_t compare;                   /* PSCompareMode */
    uint8_t alpha_test;                 /* ALPHATESTENABLE */
    uint8_t alpha_func;                 /* D3DCMP_* - 0x200 (0..7) */
    uint8_t flat;
    uint8_t fog;                        /* 0 off; FOGTABLEMODE + 1 (NONE 1 .. LINEAR 4) */
    uint8_t fog_abs;                    /* the factor of |distance|: an NV2A _ABS fog mode
                                           the title pushed (D3D's are never _ABS) */
    uint8_t debug;                      /* output instead: 1 t0, 2 v0, 3 r0, 4 fog, 5 v1,
                                           6 t3, 7 r0.a, 8 t2, 9 shadow bands, 10 v1.a,
                                           11 t0.a, 12 t3.a, 13 fract(T3 s/q, t/q),
                                           14 |T3 str| / 4, 15 t3 and 16 t0 as sampled,
                                           17 shadow-buffer draws: (stored depth, r,
                                           result) / range, unblended; 18 magenta,
                                           19 t1, 20 r1, 21 |T2 str| / 4, 22 T2 str
                                           direction, 23 T2 NaN / Inf, 24 stage 2's
                                           cube along (0.3, 0.5, 0.8) */
    uint8_t point_sprite;               /* POINTSPRITEENABLE on a point list: texture
                                           unit 3's coordinates are the point's */
    /* Shadow buffers: stages whose texture is a depth surface (bits 0-3),
     * D3DRS_SHADOWFUNC - 0x200 (bits 4-6); per stage (2 bits) the depth
     * range the r coordinate is in: 1 D24 (2^24 - 1), 2 D16, 3 F16's float
     * (511.9375), 0 F24's (an uploaded F24 texture, stored as its float). */
    uint8_t shadow;
    uint8_t shadow_fmt;
    uint8_t out_bgra;                   /* 8-bit (B8) target: it stores blue */
    uint8_t alpha_kill;                 /* stages with D3DTSS_ALPHAKILL (bits 0-3) */
    uint8_t vol;                        /* stages whose texture is a volume (bits 0-3):
                                           2D-coordinate modes fetch it in 3D, as the NV2A
                                           (LLE): PROJECT2D at r = 0, bump-env at the
                                           coordinates' r; HLE_PSH_RECT: the stage the
                                           push block's fog.w names has its linear
                                           texture kept to its own size, part of a
                                           larger surface (ff.h HLE_NVRECT_GLSL); bits
                                           5-7 free */
    uint8_t wbuf;                       /* W-buffering: depth = vW (HLE_WBUF_*_GLSL) */
    uint8_t fbfetch;                    /* stages (bits 0-3) whose texture is the colour
                                           target drawn into: its pixel, read as the
                                           input attachment (set 2) */
    uint8_t specular;                   /* SPECULARENABLE: D3D's own final combiner adds
                                           v1 (a shader without one) */
    uint8_t sblend;                     /* the NV2A's signed blend equations (ff.h
                                           HleFfPsKey.sblend) */
    uint8_t two_side;                   /* SET_TWO_SIDE_LIGHT_EN (a vertex program's
                                           shader: it writes the back colours): back
                                           faces read v0 / v1 from oB0 / oB1 */
    /* Colour key (TEXTURE_CONTROL0 1:0, D3DTSS_COLORKEYOP), 2 bits a stage:
     * a texel that is the stage's key colour gets 1 its alpha zeroed, 2 its
     * ARGB zeroed, 3 the fragment killed. Compared at the format's channel
     * precision (ckey_bits, 2 bits a stage: 0 8 bits, 1 R5G6B5, 2 1555,
     * 3 4444), alpha included where the format has one (ckey_alpha, a bit a
     * stage; an X format's is not compared). */
    uint8_t ckey, ckey_bits, ckey_alpha;
    uint32_t ckey_col[4];               /* SET_COLOR_KEY_COLOR per stage (D3DCOLOR) */
    /* The NV2A's convolution filters (CONVOLUTION_2D_LOD0), 2 bits a stage:
     * conv the kernel (1 quincunx, 2 gaussian 3 x 3), conv_mm where it
     * applies (bit 0 minification, 1 magnification). PROJECT2D only. */
    uint8_t conv, conv_mm;
    /* Colour sign, 4 bits a stage (bit 0 r .. 3 a) as two's complement:
     * sgn_ob the channels uploaded offset binary (tex/xdk_res.h), mapped
     * back exactly; sgn_raw those read as stored (targets, formats not
     * converted), mapped after filtering, inexact where texels of both
     * signs blend. */
    uint16_t sgn_ob, sgn_raw;
    uint8_t dither;                     /* SET_DITHER_ENABLE into a 16-bit target: 1
                                           R5G6B5, 2 1555 (ordered, 4 x 4 by guest
                                           pixel) */
    /* Blended, dithered draws (fbfetch): the shader blends with the target's
     * pixel by fb_src / fb_dst (D3D's blend factors) and fb_op (the blend
     * equation; fb_const the BLENDCOLOR where a factor takes it), then
     * dithers the result, as the NV2A dithers what the blend leaves. */
    uint8_t fblend;
    uint16_t fb_src, fb_dst, fb_op;
    uint8_t precise;                    /* Reuse padding; keep the shader key size stable. */
    uint32_t fb_const;
    /* SET_ANTI_ALIASING_CONTROL on an antialiased target, which is emulated
     * as an image aa_grid's times as large (bits 0-1 xs, 2-3 ys; 0: no
     * coverage control): each of its pixels is a sample of a guest pixel.
     * aa_ctl: bit 0 alpha-to-coverage, 1 alpha-to-one, 4-7 the sample mask
     * (all clear: no sample is written). hle_psh_aa_key sets both. */
    uint8_t aa_grid, aa_ctl;
    uint8_t zclamp;                     /* a DOT_ZW draw's ZCLAMP_EN: its depth clamped to
                                           the range, else a fragment outside it rejected
                                           (0 for every other draw: no split key) */
    uint8_t poly_smooth;                /* Smooth triangle edges using barycentric coverage. */
} HlePshKey;
#define HLE_PSH_RECT 0x10u              /* HlePshKey.vol: a linear texture kept to its size */

/* The stage (2 or 3) whose DOT_ZW replaces the fragment's depth, 0 none. It
 * needs the previous stage's dot product: not on stage 0 or 1. */
static inline unsigned hle_psh_zrep(uint32_t tex_modes)
{
    for (unsigned s = 3; s >= 2; s--) {
        if (((tex_modes >> (5 * s)) & 0x1F) == HLE_PST_DOT_ZW) {
            return s;
        }
    }
    return 0;
}

/* Antialiasing control into k (aa_grid, aa_ctl): SET_ANTI_ALIASING_CONTROL's
 * aactl for a draw into a target with xs x ys samples (1 x 1: none), colour
 * or depth (a depth-only draw's mask applies to its depth and stencil).
 * Inactive without multisampling (bit 0), without a target or samples, or
 * with every sample in the mask and neither alpha-to-coverage nor
 * alpha-to-one. */
void hle_psh_aa_key(HlePshKey *k, uint32_t aactl, uint32_t xs, uint32_t ys, bool colour, bool depth);

/* Configure precision before starting a game. Shader keys record the mode. */
void hle_psh_set_precision(bool enabled);

/* Key from the render-state array (rs[0..56] = the pixel-shader block). */
void hle_psh_key(const uint32_t *rs, HlePshKey *k);

/* GLSL into buf; returns its length, 0 when it does not fit. Unsupported
 * texture modes sample as PROJECT2D and set *unsupported (may be NULL): a
 * mode on a stage it can't be on (bump-env and dependent reads on stage 0,
 * BRDF and DOT_ZW below stage 2). DOT_RFLCT_SPEC_CONST's eye vector is in
 * stage 3's bump words (pc.bump3, pc.bumpl[2]: float bits), which a stage in
 * that mode doesn't use. */
size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported);

#endif
