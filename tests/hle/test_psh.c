#include "hw/xbox/hle/shader/psh.h"
#include "hw/xbox/hle/shader/ff.h"
#include "hle_test.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char buf[65536];

static void dump(const char *name)
{
    const char *dir = getenv("HLE_TEST_DUMP");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.frag", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

/* register-combiner encodings (XDK PS_* macros) */
#define IN4(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define R_ZERO 0x0
#define R_C0   0x1
#define R_FOG  0x3
#define R_V0   0x4
#define R_T0   0x8
#define R_T1   0x9
#define R_R0   0xC
#define R_R1   0xD
#define R_V1   0x5
#define R_SUM  0xE
#define CH_A   0x10
#define INVERT 0x20
#define EXPAND 0x40

/* tex t0; mul r0, t0, v0 */
static void test_modulate(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_ALPHA_IN] = IN4(R_T0 | CH_A, R_V0 | CH_A, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_ALPHA_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;                            /* t0 PROJECT2D */
    rs[HLE_PS_RGB_IN + 5] = 0xDEADBEEF;                 /* past the count: ignored */
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_EQ_U32(k.rgb_in[5], 0);
    unsigned un = 99;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_modulate");
    HT_EQ_INT(un, 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp(0u, nvproj(vTex0).xy);") != NULL);
    HT_CHECK(strstr(buf, "vec3 nvproj(vec4 t)") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = max(t0.rgb, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "float Aa = max(t0.a, 0.0);") != NULL);
    HT_CHECK(strstr(buf, "vec3 ab = A * B") != NULL);
    HT_CHECK(strstr(buf, "r0.rgb = ab;") != NULL);
    HT_CHECK(strstr(buf, "r0.a = aab;") != NULL);
    /* no final combiner: D3D's own (fog over r0, alpha r0.a) */
    HT_CHECK(strstr(buf, "outc = vec4(mix(fogr.rgb, clamp(r0.rgb, 0.0, 1.0), fogf), "
                         "clamp(r0.a, 0.0, 1.0));") != NULL);
    HT_CHECK(strstr(buf, "r0 = vec4(0.0, 0.0, 0.0, t0.a)") != NULL);
    k.specular = 1;                                     /* SPECULARENABLE: v1 + r0 */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "clamp(clamp(v1.rgb, 0.0, 1.0) + clamp(r0.rgb, 0.0, 1.0), 0.0, 1.0), "
                         "fogf)") != NULL);
    /* no descriptor indexing: every array sized, nothing nonuniform */
    HT_CHECK(hle_glsl_fixed_slots(buf, sizeof(buf)) == strlen(buf));
    HT_CHECK(strstr(buf, "nonuniform") == NULL && strstr(buf, "[]") == NULL);
    HT_CHECK(strstr(buf, "uniform textureCube uTexCube[4];") != NULL);
    HT_CHECK(strstr(buf, "uniform texture3D uTex3D[4];") != NULL);
    HT_CHECK(strstr(buf, "uTex2D[(ti)]") != NULL);
    dump("psh_fixed");
}

/* fog through the final combiner: xfc fog.a, r0, fog.rgb, zero, zero, zero, r0.a;
 * alpha test GREATEREQUAL; linear fog */
static void test_final_fog(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_ZERO | INVERT, R_ZERO, R_ZERO);   /* t0 * 1 */
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 1;
    rs[HLE_PS_FINAL_ABCD] = IN4(R_FOG | CH_A, R_R0, R_FOG, R_ZERO);
    rs[HLE_PS_FINAL_EFG] = IN4(R_ZERO, R_ZERO, R_R0 | CH_A, 0x80);
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.fog = 4;
    k.alpha_test = 1;
    k.alpha_func = 6;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_fog");
    HT_CHECK(strstr(buf, "vec3 B = max(r0.rgb, 0.0)") == NULL ||
             strstr(buf, "(1.0 - clamp(vec4(0.0).rgb, 0.0, 1.0))") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = clamp(max(fogr.aaa, 0.0), 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "outc = vec4(A * B + (1.0 - A) * C + D, G);") != NULL);
    HT_CHECK(strstr(buf, "float G = clamp(max(r0.a, 0.0), 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "v1r0 = clamp(v1r0, 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "(pc.fog.y - vFog) / (pc.fog.y - pc.fog.x)") != NULL);
    HT_CHECK(strstr(buf, "a >= r") != NULL);
    k.fog_abs = 1;                      /* an _ABS fog mode the title pushed */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "(pc.fog.y - abs(vFog)) / (pc.fog.y - pc.fog.x)") != NULL);
    k.fog_abs = 0;
    k.tex_modes = 2;                    /* PROJECT3D: (s, t, r) / q */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3(0u, nvproj(vTex0));") != NULL);
    k.tex_modes = 1;
    k.vol = 1;                          /* PROJECT2D on a volume: its first slice */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3(0u, vec3(nvproj(vTex0).xy, 0.0));") != NULL);
    k.vol = 0;
}

/* two stages: dot3 with expanded normals and MSB mux, unique C0, clip plane,
 * an unsupported (cube) mode, blue-to-alpha and output scaling */
static void test_dot_mux(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1 | (3u << 5) | (5u << 10);   /* t0 2D, t1 cube, t2 clip */
    rs[HLE_PS_COMPARE] = 0x5u << 8;                      /* t2: s >= 0, r >= 0 */
    rs[HLE_PS_RGB_IN] = IN4(R_T0 | EXPAND, R_V0 | EXPAND, R_C0, R_T1);
    rs[HLE_PS_RGB_OUT] = (R_R1 << 4) | R_R0 | (0x2000) | (0x80000) | (2u << 15);
    rs[HLE_PS_RGB_IN + 1] = IN4(R_R0, R_C0, R_R1, R_C0);
    rs[HLE_PS_RGB_OUT + 1] = (R_R0 << 8) | 0x4000;      /* r0 = mux */
    rs[HLE_PS_COUNT] = 2 | 0x100 | 0x1000;
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.precise = 1;
    unsigned un = 0;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_dot");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "vec4 t1 = sampc(1u, vTex1.xyz);") != NULL);
    HT_CHECK(strstr(buf, "vec3 ab = vec3(dot(A, B))") != NULL);
    HT_CHECK(strstr(buf, "(2.0 * max(t0.rgb, 0.0) - 1.0)") != NULL);
    HT_CHECK(strstr(buf, "r1.a = ab.b;") != NULL);
    HT_CHECK(strstr(buf, "ab = q9(clamp((ab * 2.0), -1.0, 1.0));") != NULL);
    HT_CHECK(strstr(buf, "bool mx = r0.a > 0.5;") != NULL);
    HT_CHECK(strstr(buf, "vec3 ms = mx ? cd : ab;") != NULL);
    HT_CHECK(strstr(buf, "col(pc.psc[1])") != NULL);    /* stage 1's own C0 */
    HT_CHECK(strstr(buf, "if (vTex2.x >= 0.0) discard;") != NULL);
    HT_CHECK(strstr(buf, "if (vTex2.y < 0.0) discard;") != NULL);
}

/* Halo 2's scope mask: r0.a = 0.5 - t0.b (HALFBIAS_NEGATE), then a MUX on
 * its MSB picks black (CD) where t0.b is 0: the bias's 0.5 is 0x80 / 0xFF,
 * in every shader */
static void test_mux_bias(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1;
    rs[HLE_PS_ALPHA_IN + 1] = IN4(R_C0 | CH_A, R_ZERO | INVERT, R_T0 | 0xA0, R_ZERO | INVERT);
    rs[HLE_PS_ALPHA_OUT + 1] = R_R0 << 8;               /* r0.a = C0.a + (0.5 - t0.b) */
    rs[HLE_PS_RGB_IN + 2] = IN4(R_C0, R_ZERO | INVERT, R_R0, R_ZERO | INVERT);
    rs[HLE_PS_RGB_OUT + 2] = (R_T1 << 8) | 0x4000;      /* t1 = mux(C0, r0) */
    rs[HLE_PS_COUNT] = 3 | 0x100;
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.precise = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_mux_bias");
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "bool mx = r0.a > 0.5;") != NULL);
    HT_CHECK(strstr(buf, "vec3 ms = mx ? cd : ab;") != NULL);
    /* a MUX in the alpha portion only; the output BIAS takes the same 0x80 */
    rs[HLE_PS_RGB_OUT + 2] = R_T1 << 8;
    rs[HLE_PS_ALPHA_IN + 2] = IN4(R_C0 | CH_A, R_ZERO | INVERT, R_R0 | CH_A, R_ZERO | INVERT);
    rs[HLE_PS_ALPHA_OUT + 2] = (R_T1 << 8) | 0x4000;
    rs[HLE_PS_RGB_IN + 1] = IN4(R_T0 | 0x80, R_ZERO | INVERT, R_ZERO, R_ZERO);  /* HALFBIAS_NORMAL */
    rs[HLE_PS_RGB_OUT + 1] = (R_R1 << 4) | (3u << 15);  /* SHIFTLEFT_1_BIAS */
    hle_psh_key(rs, &k);
    k.precise = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "ams = mx ? acd : aab;") != NULL);
    HT_CHECK(strstr(buf, "vec3 A = (max(t0.rgb, 0.0) - (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "ab = q9(clamp(((ab - (128.0 / 255.0)) * 2.0), -1.0, 1.0));") != NULL);
    rs[HLE_PS_RGB_IN + 1] = 0;
    rs[HLE_PS_RGB_OUT + 1] = 0;
    rs[HLE_PS_ALPHA_IN + 2] = 0;
    rs[HLE_PS_ALPHA_OUT + 2] = 0;
    rs[HLE_PS_RGB_OUT + 2] = (R_T1 << 8) | 0x4000;
    rs[HLE_PS_COUNT] = 3;                               /* MUX on the LSB */
    hle_psh_key(rs, &k);
    k.precise = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "bool mx = (int(floor(r0.a * 255.0 + 0.5)) & 1) == 1;") != NULL);
    rs[HLE_PS_COUNT] = 3 | 0x100;
    rs[HLE_PS_RGB_OUT + 2] = R_T1 << 8;                 /* no MUX: sum */
    rs[HLE_PS_ALPHA_OUT + 1] = (R_R0 << 8) | (1u << 15);   /* BIAS */
    hle_psh_key(rs, &k);
    k.precise = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float Ca = (-max(t0.b, 0.0) + (128.0 / 255.0));") != NULL);
    HT_CHECK(strstr(buf, "ams = q9(clamp((ams - (128.0 / 255.0)), -1.0, 1.0));") != NULL);
}

/* PGR2's car paint: t0 normal map, t1/t2 DOTPRODUCT, t3 DOT_RFLCT_SPEC */
static void test_reflect(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 0x64621;
    rs[HLE_PS_DOTMAP] = 0x111;                          /* MINUS1_TO_1_D3D */
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_reflect");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "dot1 = dot(vTex1.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot2 = dot(vTex2.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot3 = dot(vTex3.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "e = vec3(vTex1.w, vTex2.w, vTex3.w)") != NULL);
    HT_CHECK(strstr(buf, "t3 = sampc(3u,") != NULL);
    /* DOT_RFLCT_SPEC_CONST: the eye vector constant, in stage 3's bump words */
    rs[HLE_PS_TEXMODES] = 1 | 0x11u << 5 | 0x11u << 10 | (unsigned)HLE_PST_DOT_RFLCT_SPEC_CONST << 15;
    hle_psh_key(rs, &k);
    un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_reflect_const");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "dot3 = dot(vTex3.xyz, dmap(1u, t0));") != NULL);
    HT_CHECK(strstr(buf, "e = vec3(uintBitsToFloat(pc.bump3), uintBitsToFloat(pc.bumpl[2]))")
             != NULL);
    HT_CHECK(strstr(buf, "vTex3.w") == NULL);
    HT_CHECK(strstr(buf, "t3 = sampc(3u, 2.0 * n * dot(n, e)") != NULL);
}

/* The normalization HLE_ZNORM_GLSL emits (z * s + z * s * s for the integer
 * formats' s = 2^-n, z * s otherwise), each product rounded on its own as
 * "precise" keeps them on the GPU */
static float znorm(float z, float s)
{
    const float a = z * s;
    const float ss = s == 0x1p-24f || s == 0x1p-16f ? s * s : 0.0f;
    const float b = z * ss;
    return a + b;
}

/* texm3x2pad / texm3x2depth: stage 2 DOTPRODUCT, stage 3 DOT_ZW; dot2 /
 * dot3 is in depth-buffer units (nxdk_pgraph_tests' DotZW: its first
 * vertex's matrix makes 10000), normalized as a program's screen z,
 * rejected outside the buffer's range (clamped with ZCLAMP_EN) */
static void test_dot_zw(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 1 | 0x11u << 10 | (unsigned)HLE_PST_DOT_ZW << 15;
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_EQ_U32(hle_psh_zrep(k.tex_modes), 3);
    k.alpha_kill = 8;                                   /* no texel: nothing to kill on */
    unsigned un = 99;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_dot_zw");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "dot2 = dot(vTex2.xyz, dmap(0u, t0));") != NULL);
    HT_CHECK(strstr(buf, "dot3 = dot(vTex3.xyz, dmap(0u, t0));") != NULL);
    HT_CHECK(strstr(buf, "vec4 t3 = vec4(0.0);\n  zrz = dot2;\n  zrw = dot3;") != NULL);
    HT_CHECK(strstr(buf, "t3.a == 0.0") == NULL);
    /* units to the buffer's 0..1 by the program's normalization, not as is */
    const char *q = strstr(buf, "float zq = zrw == 0.0 ? 0.0 : zrz / zrw;\n");
    const char *zn = strstr(buf, "precise float zrep = zq * pc.zscale + zq * (pc.zscale == "
                                 "uintBitsToFloat(0x33800000u) ||");
    const char *far = strstr(buf, "if (zrw == 0.0) zrep = 1.0;");
    const char *rej = strstr(buf, "if (!(zrep >= 0.0 && zrep <= 1.0)) discard;");
    const char *out = strstr(buf, "gl_FragDepth = zrep;");
    HT_CHECK(q && zn && far && rej && out && q < zn && zn < far && far < rej && rej < out);
    /* replaces W-buffering's depth (its calculation not made); the EQUAL test
     * reads the replaced one */
    k.wbuf = 1;
    k.zclamp = 1;
    k.fbfetch = HLE_FB_ZEQ24;
    n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_dot_zw_wbuf");
    HT_CHECK(strstr(buf, "zrep = clamp(zrep, 0.0, 1.0);") != NULL);
    HT_CHECK(strstr(buf, "zrep >= 0.0") == NULL);
    HT_CHECK(strstr(buf, "if (zrep < subpassLoad(uZ).r") != NULL);
    HT_CHECK(strstr(buf, "gl_FragCoord.z <") == NULL);
    HT_CHECK(strstr(buf, "gl_FragDepth = zrep;") != NULL);
    HT_CHECK(strstr(buf, "wDepth") == NULL);
    /* stage 2 (texm3x2pad on stage 1) */
    rs[HLE_PS_TEXMODES] = 1 | 0x11u << 5 | (unsigned)HLE_PST_DOT_ZW << 10;
    hle_psh_key(rs, &k);
    HT_EQ_U32(hle_psh_zrep(k.tex_modes), 2);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "zrz = dot1;\n  zrw = dot2;") != NULL);
    /* stage 1: no dot before it */
    rs[HLE_PS_TEXMODES] = 1 | (unsigned)HLE_PST_DOT_ZW << 5;
    hle_psh_key(rs, &k);
    HT_EQ_U32(hle_psh_zrep(k.tex_modes), 0);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 1u << HLE_PST_DOT_ZW);
    HT_CHECK(strstr(buf, "zrep") == NULL && strstr(buf, "gl_FragDepth") == NULL);
}

/* DOT_ZW's depths by format, as the shader normalizes them (pc.zscale as
 * pbdraw.c sets it): DotZW's 10000 (z 1000 x, w 0.1 x the texel's sum) */
static void test_dot_zw_formats(void)
{
    const float z = 1000.0f * 1.9f, w = 0.1f * 1.9f, q = z / w;
    HT_CHECK(q > 9999.0f && q < 10001.0f);
    /* D24: 10000 of its 2^24 - 1 levels, kept (read as 0..1 it was above 1,
     * discarded) */
    const float d24 = znorm(q, 0x1p-24f);
    HT_CHECK(d24 > 0.0f && d24 < 1.0f);
    HT_CHECK((uint32_t)(d24 * 16777215.0f + 0.5f) == (uint32_t)(q + 0.5f));
    HT_EQ_INT(znorm(16777215.0f, 0x1p-24f) == 1.0f, 1);         /* the far plane, kept */
    HT_EQ_INT(znorm(16777220.0f, 0x1p-24f) > 1.0f, 1);          /* past it: rejected */
    /* D16: the same depth a quarter of the way in */
    const float d16 = znorm(q, 0x1p-16f);
    HT_CHECK(d16 > 0.15f && d16 < 0.16f);
    HT_CHECK((uint32_t)(d16 * 65535.0f + 0.5f) == (uint32_t)(q + 0.5f));
    HT_EQ_INT(znorm(65535.0f, 0x1p-16f) == 1.0f, 1);
    HT_EQ_INT(znorm(65536.0f, 0x1p-16f) > 1.0f, 1);
    /* float buffers (zscale 1): the value as it is, as a program's z there */
    HT_CHECK(znorm(0.25f, 1.0f) == 0.25f);
    HT_CHECK(znorm(q, 1.0f) > 1.0f);
}

/* texbrdf: stages 0 and 1 the eye's and light's spherical coordinates, stage
 * 2 the BRDF volume at (eye elevation, light elevation, azimuth difference) */
static void test_brdf(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_TEXMODES] = 3 | 3u << 5 | (unsigned)HLE_PST_BRDF << 10;    /* t0, t1 cubes */
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.vol = 4;
    unsigned un = 99;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_brdf");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "vec4 t2 = samp3(2u, vec3(t0.g, t1.g, t1.r - t0.r));") != NULL);
    /* no volume bound: the 2D texture, normalised (no image of another kind) */
    k.vol = 0;
    n = hle_psh_glsl(&k, buf, sizeof(buf), &un);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_brdf_2d");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "uSmp[nonuniformEXT(si)]), vec2(t0.g, t1.g)); }") != NULL);
    HT_CHECK(strstr(buf, "samp3(2u") == NULL);
    /* stage 3: stages 1 and 2 */
    rs[HLE_PS_TEXMODES] = 1 | 3u << 5 | 3u << 10 | (unsigned)HLE_PST_BRDF << 15;
    hle_psh_key(rs, &k);
    k.vol = 8;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "vec4 t3 = samp3(3u, vec3(t1.g, t2.g, t2.r - t1.r));") != NULL);
    /* stage 1: not two stages before it */
    rs[HLE_PS_TEXMODES] = 1 | (unsigned)HLE_PST_BRDF << 5;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 1u << HLE_PST_BRDF);
}

/* NGB's shadow receivers: t0 a D24 shadow buffer (PROJECT3D, SHADOWFUNC
 * GREATEREQUAL). The coordinates go through NV2A's projection, so a
 * receiver with oT0 = 0 compares r = 0 (lit) instead of NaN weights. */
static void test_shadow_project(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_C0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 2;                            /* t0 PROJECT3D */
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.shadow = 1 | (6u << 4);
    k.shadow_fmt = 1;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_shadow");
    HT_CHECK(strstr(buf, "vec3 pq = nvproj(vTex0);") != NULL);
    HT_CHECK(strstr(buf, "float m = 16777215.0, r = clamp(pq.z, 0.0, m);") != NULL);
    HT_CHECK(strstr(buf, "greaterThanEqual(d, vec4(r))") != NULL);
    HT_CHECK(strstr(buf, "/ vTex0.w") == NULL);
}

/* An uploaded F24 texture (shadow_fmt 0 with the stage's shadow bit): the
 * stored float compared as it is, r cut to F24's 16-bit mantissa */
static void test_shadow_f24(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_C0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 2;                            /* t0 PROJECT3D */
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.shadow = 1 | (2u << 4);                           /* EQUAL */
    k.shadow_fmt = 0;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_shadow_f24");
    HT_CHECK(strstr(buf, "float m = 3.402823466e38, r = uintBitsToFloat(floatBitsToUint("
                         "clamp(pq.z, 0.0, m)) & 0xFFFFFF80u);") != NULL);
    HT_CHECK(strstr(buf, "uv) * 1.0;") != NULL);
    HT_CHECK(strstr(buf, "equal(d, vec4(r))") != NULL);
}

/* Otogi's title ripple (modes 0x004c1): t0 the bump map, t1 BUMPENVMAP of
 * the copied scene, t2 2D; then t2 BUMPENVMAP_LUM reading t1 (PSInputTexture) */
static void test_bumpenv(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T1, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = 0x004c1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    unsigned un = 99;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    dump("psh_bumpenv");
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "uvec4 bump;") != NULL);
    HT_CHECK(strstr(buf, "vec2 d = mat2(unpackHalf2x16(pc.bump.x), unpackHalf2x16(pc.bump.y)) * "
                         "sgn8(t0.bg);") != NULL);
    HT_CHECK(strstr(buf, "t1 = samp(1u, nvproj(vTex1).xy + d);") != NULL);
    HT_CHECK(strstr(buf, "vec4 t2 = samp(2u, nvproj(vTex2).xy);") != NULL);
    k.vol = 2;                          /* t1 a volume: r passes through */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_CHECK(strstr(buf, "t1 = samp3(1u, vec3(nvproj(vTex1).xy + d, vTex1.z));") != NULL);
    rs[HLE_PS_TEXMODES] = 1 | (1u << 5) | (7u << 10);
    rs[HLE_PS_INPUTTEX] = 1u << 16;                     /* t2 reads t1 */
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), &un) > 0);
    HT_EQ_U32(un, 0);
    HT_CHECK(strstr(buf, "sgn8(t1.bg);") != NULL);
    HT_CHECK(strstr(buf, "t2.rgb *= clamp(l.x * t1.r + l.y, 0.0, 1.0);") != NULL);
}

/* two-sided lighting: back faces read v0 / v1 from the back colours */
static void test_two_side(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_V0, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vBack0") == NULL);
    HT_CHECK(strstr(buf, "vec4 v0 = vDiffuse, v1 = vSpecular;") != NULL);
    k.two_side = 1;
    k.flat = 1;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_two_side");
    HT_CHECK(strstr(buf, "layout(location = 7) flat in vec4 vBack0;") != NULL);
    HT_CHECK(strstr(buf, "layout(location = 8) flat in vec4 vBack1;") != NULL);
    HT_CHECK(strstr(buf, "vec4 v0 = gl_FrontFacing ? vDiffuse : vBack0;") != NULL);
    HT_CHECK(strstr(buf, "vec4 v1 = gl_FrontFacing ? vSpecular : vBack1;") != NULL);
}

/* colour key: a matching texel's alpha zeroed (1), ARGB zeroed (2) or the
 * fragment killed (3), compared at the format's precision, alpha included
 * where the format has one (the NV2A tests: an A8R8G8B8 texel 0xFF0000FF
 * survives the key 0xF00000FF; X8R8G8B8 matches whatever the alpha) */
static void test_color_key(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_T1, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_PROJECT2D << 5 | HLE_PST_PROJECT2D << 10 |
                          HLE_PST_PROJECT2D << 15;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "ckeq") == NULL);
    k.ckey = 1 | 2 << 2 | 3 << 4 | 3 << 6;
    k.ckey_bits = 1 << 2 | 2 << 6;                      /* stage 1 R5G6B5, stage 3 1555 */
    k.ckey_alpha = 1 | 1 << 3;                          /* stages 0 (A8R8G8B8) and 3 (A1R5G5B5) */
    k.ckey_col[0] = 0xF00000FFu;
    k.ckey_col[1] = 0x00000000u;
    k.ckey_col[2] = 0xCC00FF00u;                        /* stage 2: X8R8G8B8 */
    k.ckey_col[3] = 0xF0FF00FFu;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_color_key");
    HT_CHECK(strstr(buf, "bool ckeq(vec4 t, vec4 c, vec4 n)") != NULL);
    HT_CHECK(strstr(buf, "if (ckeq(t0, vec4(0.0, 0.0, 255.0, 240.0) / 255.0, vec4(255.0))) "
                         "t0.a = 0.0;") != NULL);
    HT_CHECK(strstr(buf, "if (ckeq(t1, vec4(0.0, 0.0, 0.0, 0.0) / 255.0, "
                         "vec4(31.0, 63.0, 31.0, 0.0))) t1 = vec4(0.0);") != NULL);
    HT_CHECK(strstr(buf, "if (ckeq(t2, vec4(0.0, 255.0, 0.0, 204.0) / 255.0, "
                         "vec4(255.0, 255.0, 255.0, 0.0))) discard;") != NULL);
    HT_CHECK(strstr(buf, "if (ckeq(t3, vec4(255.0, 0.0, 255.0, 240.0) / 255.0, "
                         "vec4(31.0, 31.0, 31.0, 1.0))) discard;") != NULL);
    /* a stage reading the target it renders (fbfetch) is keyed too */
    k.fbfetch = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_color_key_fb");
    const char *fb = strstr(buf, "vec4 t0 = subpassLoad(uFb);");
    const char *ck = strstr(buf, "if (ckeq(t0, ");
    HT_CHECK(fb != NULL && ck != NULL && fb < ck);
}

/* colour sign: offset-binary channels mapped back, others as stored; a
 * bump stage takes its input's signed channels as they are, back over 127
 * (tsh), as dot products and dependent reads do */
static void test_sign(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T1, R_V0, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_BUMPENVMAP << 5;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "sgnob") == NULL);             /* unsigned: no change */
    HT_CHECK(strstr(buf, "* sgn8(t0.bg);") != NULL);
    k.sgn_ob = 0x6;                                     /* stage 0: g, b */
    k.sgn_raw = 0x8 | 0x10;                             /* stage 0 a, stage 1 r */
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_sign");
    HT_CHECK(strstr(buf, "float sgnob(float c)") != NULL);
    HT_CHECK(strstr(buf, "t0.g = sgnob(t0.g);\n  t0.b = sgnob(t0.b);\n"
                         "  t0.a = sgnraw(t0.a);") != NULL);
    HT_CHECK(strstr(buf, "t1.r = sgnraw(t1.r);") != NULL);
    HT_CHECK(strstr(buf, "vec4 tsh(vec4 t, bvec4 m) { return mix(t, max(t * (255.0 / 254.0), "
                         "-1.0), m); }") != NULL);
    HT_CHECK(strstr(buf, "* tsh(t0, bvec4(false, true, true, true)).bg;") != NULL);
    HT_CHECK(strstr(buf, "sgn8(t0.bg)") == NULL);
    HT_CHECK(strstr(buf, "vec3 A = max(t1.rgb, 0.0);") != NULL);     /* combiners: as decoded */
    k.sgn_ob = 0x4;                                     /* b only */
    k.sgn_raw = 0;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "* vec2(tsh(t0, bvec4(false, false, true, false)).b, sgn8(t0.bg).y);")
             != NULL);
    /* a dot product and a dependent read of a signed t0 */
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_DOTPRODUCT << 5 | HLE_PST_DPNDNT_GB << 10;
    hle_psh_key(rs, &k);
    k.sgn_ob = 0x7;                                     /* t0 rgb */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_sign_dot");
    HT_CHECK(strstr(buf, "dot1 = dot(vTex1.xyz, dmap(0u, tsh(t0, bvec4(true, true, true, false))));")
             != NULL);
    HT_CHECK(strstr(buf, "uSmp[nonuniformEXT(si)]), tsh(t0, bvec4(true, true, true, false)).gb); }")
             != NULL);
    k.sgn_ob = 0;                                       /* unsigned: as before */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "dot1 = dot(vTex1.xyz, dmap(0u, t0));") != NULL);
    HT_CHECK(strstr(buf, "tsh") == NULL);
}

/* colour key on signed channels: compared as the guest stores them (an
 * offset-binary channel's top bit flipped back), the key's zero written
 * after the sign is decoded; a target read by fbfetch decoded too */
static void test_sign_color_key(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_T1, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_PROJECT2D << 5 | HLE_PST_PROJECT2D << 10;
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.ckey = 1 | 2 << 2 | 3 << 4;                       /* alpha, ARGB, kill */
    k.ckey_alpha = 1 | 2 | 4;
    k.ckey_col[0] = 0x00000000u;
    k.ckey_col[1] = 0x80FF0000u;
    k.ckey_col[2] = 0x00000000u;
    k.sgn_ob = 0x8 | 0x7 << 4 | 0x1 << 8;               /* t0 a; t1 rgb; t2 r */
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_sign_color_key");
    const char *m0 = strstr(buf, "  bool ck0 = ckeq(mix(t0, mod(floor(t0 * 255.0 + 0.5) + 128.0, "
                                 "256.0) / 255.0, vec4(0.0, 0.0, 0.0, 1.0)), vec4(0.0, 0.0, 0.0, "
                                 "0.0) / 255.0, vec4(255.0));\n");
    const char *d0 = strstr(buf, "  t0.a = sgnob(t0.a);\n");
    const char *z0 = strstr(buf, "  if (ck0) t0.a = 0.0;\n");
    HT_CHECK(m0 != NULL && d0 != NULL && z0 != NULL && m0 < d0 && d0 < z0);
    const char *m1 = strstr(buf, "  bool ck1 = ckeq(mix(t1, mod(floor(t1 * 255.0 + 0.5) + 128.0, "
                                 "256.0) / 255.0, vec4(1.0, 1.0, 1.0, 0.0)), vec4(255.0, 0.0, 0.0, "
                                 "128.0) / 255.0, vec4(255.0));\n");
    const char *z1 = strstr(buf, "  if (ck1) t1 = vec4(0.0);\n");
    HT_CHECK(m1 != NULL && z1 != NULL && m1 < strstr(buf, "  t1.b = sgnob(t1.b);\n") &&
             strstr(buf, "  t1.b = sgnob(t1.b);\n") < z1);
    /* a kill needs no zero: the test, then the decode */
    const char *k2 = strstr(buf, "  if (ckeq(mix(t2, mod(floor(t2 * 255.0 + 0.5) + 128.0, "
                                 "256.0) / 255.0, vec4(1.0, 0.0, 0.0, 0.0)), vec4(0.0, 0.0, 0.0, "
                                 "0.0) / 255.0, vec4(255.0))) discard;\n");
    HT_CHECK(k2 != NULL && k2 < strstr(buf, "  t2.r = sgnob(t2.r);\n"));
    /* a target's channels as stored, through fbfetch */
    k.ckey = 0;
    k.sgn_ob = 0;
    k.sgn_raw = 0x7;
    k.fbfetch = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_sign_fb");
    const char *fb = strstr(buf, "vec4 t0 = subpassLoad(uFb);");
    const char *r0 = strstr(buf, "  t0.r = sgnraw(t0.r);\n");
    HT_CHECK(fb != NULL && r0 != NULL && fb < r0 && strstr(buf, "  t0.b = sgnraw(t0.b);\n"));
}

/* the convolution filters: PROJECT2D stages through sampk, with the kernel
 * and where it applies */
static void test_convolution(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_T1, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_PROJECT2D << 5;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "sampk") == NULL);
    k.conv = 1 | 2 << 2;                                /* quincunx, gaussian */
    k.conv_mm = 3 | 1 << 2;                             /* both; minification */
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_convolution");
    HT_CHECK(strstr(buf, "vec4 sampk(uint s, vec2 uv, uint kern, uint when)") != NULL);
    HT_CHECK(strstr(buf, "vec4 t0 = sampk(0u, nvproj(vTex0).xy, 1u, 3u);") != NULL);
    HT_CHECK(strstr(buf, "vec4 t1 = sampk(1u, nvproj(vTex1).xy, 2u, 1u);") != NULL);
    /* the taps at level 0 through the level-0 sampler (bits 24-31), at an
     * explicit LOD; the other side of the branch by gradients taken before
     * it: no implicit derivatives where a quad's fragments may diverge */
    const char *fn = strstr(buf, "vec4 tap(uint ti, uint ci, vec2 uv) {");
    const char *end = fn ? strstr(strstr(fn, "vec4 sampk("), "\n}\n") : NULL;
    HT_CHECK(fn != NULL && end != NULL);
    char seg[2048] = { 0 };
    if (fn && end && (size_t)(end - fn) < sizeof(seg)) {
        memcpy(seg, fn, (size_t)(end - fn));
    }
    HT_CHECK(strstr(seg, "uSmp[nonuniformEXT(ci)]), uv, 0.0);") != NULL);
    HT_CHECK(strstr(seg, "si = (pc.tex[s] >> 16) & 0xFFu, ci = pc.tex[s] >> 24;") != NULL);
    HT_CHECK(strstr(seg, "texture(") == NULL);
    const char *grad = strstr(seg, "vec2 gx = dFdx(uv), gy = dFdy(uv);");
    const char *br = strstr(seg, "if ((when & (lod > 0.0 ? 1u : 2u)) == 0u)");
    HT_CHECK(grad != NULL && br != NULL && grad < br);
    HT_CHECK(strstr(seg, "uSmp[nonuniformEXT(si)]), uv, gx, gy);") != NULL);
    /* the stages' own samplers: the index's 8 bits */
    HT_CHECK(strstr(buf, "uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0xFFu;\n"
                         "  if ((pc.flags & (1u << s)) != 0u)") != NULL);
    /* no descriptor indexing: the draw set's samplers (vk/draw.c fixed_bind),
     * the taps' at 1 + HLE_FF_STAGES + s, inside the sized array */
    HT_CHECK(hle_glsl_fixed_slots(buf, sizeof(buf)) == strlen(buf));
    HT_CHECK(strstr(buf, "uniform sampler uSmp[9];") != NULL);
    HT_CHECK(1 + 2 * HLE_FF_STAGES == 9);
    HT_CHECK(strstr(buf, "uSmp[(ci)]), uv, 0.0);") != NULL);
    dump("psh_convolution_fixed");
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    k.vol = 1;                                          /* a volume: not filtered so */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 t0 = samp3(0u") != NULL);
}

/* antialiasing control on a 2 x 2 target: the sample by guest pixel, the
 * mask's samples, alpha-to-coverage's first alpha x 4, alpha-to-one */
static void test_aa_control(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_V0, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "uint si") == NULL);
    /* inactive: multisampling off, every sample with nothing else, no
     * samples, no target */
    hle_psh_aa_key(&k, 0x00050010u, 2, 2, true, false);
    HT_EQ_INT(k.aa_grid, 0);
    hle_psh_aa_key(&k, 0xFFFF0001u, 2, 2, true, false);
    HT_EQ_INT(k.aa_grid, 0);
    hle_psh_aa_key(&k, 0x00050001u, 1, 1, true, false);
    HT_EQ_INT(k.aa_grid, 0);
    hle_psh_aa_key(&k, 0x00050001u, 2, 2, false, false);
    HT_EQ_INT(k.aa_grid, 0);
    /* a depth-only draw keeps its mask (its depth and stencil samples) */
    hle_psh_aa_key(&k, 0x00050001u, 2, 2, false, true);
    HT_EQ_INT(k.aa_grid, 2 | 2 << 2);
    HT_EQ_INT(k.aa_ctl, 0x5 << 4);
    /* an empty mask writes no sample: active, though aa_ctl is 0 */
    hle_psh_aa_key(&k, 0x00000001u, 2, 2, true, false);
    HT_EQ_INT(k.aa_grid, 2 | 2 << 2);
    HT_EQ_INT(k.aa_ctl, 0);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "if (((0u >> si) & 1u) == 0u) discard;") != NULL);
    hle_psh_aa_key(&k, 0x00050111u, 2, 2, true, false);
    HT_EQ_INT(k.aa_ctl, 1 | 2 | 0x5 << 4);
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_aa_control");
    HT_CHECK(strstr(buf, "ivec2 sp = ivec2(gl_FragCoord.xy / float(max((pc.flags >> 16) & 255u, "
                         "1u))) % ivec2(2, 2);") != NULL);
    HT_CHECK(strstr(buf, "uint si = uint(sp.y * 2 + sp.x);") != NULL);
    HT_CHECK(strstr(buf, "if (((5u >> si) & 1u) == 0u) discard;") != NULL);
    HT_CHECK(strstr(buf, "if (float(si) + 0.5 >= outc.a * 4.0) discard;") != NULL);
    HT_CHECK(strstr(buf, "outc.a = 1.0;") != NULL);
    /* before the output: the blend takes alpha-to-one's alpha */
    HT_CHECK(strstr(buf, "outc.a = 1.0;") < strstr(buf, "oColor = outc;"));
    /* 2 x 1, every sample in the mask: alpha-to-coverage alone */
    hle_psh_aa_key(&k, 0xFFFF0011u, 2, 1, true, false);
    HT_EQ_INT(k.aa_ctl, 1 | 0x3 << 4);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, ">> si) & 1u") == NULL);
    HT_CHECK(strstr(buf, "if (float(si) + 0.5 >= outc.a * 2.0) discard;") != NULL);
    HT_CHECK(strstr(buf, "outc.a = 1.0;") == NULL);
}

/* dithering into a 16-bit target: the ordered offset by its channel steps */
static void test_dither(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_V0, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "bm[16]") == NULL);
    k.dither = 1;
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_dither");
    HT_CHECK(strstr(buf, "oColor.rgb = clamp(oColor.rgb + dt / vec3(31.0, 63.0, 31.0), 0.0, 1.0);")
             != NULL);
    /* by guest pixel: the target's resolution scale divided out */
    HT_CHECK(strstr(buf, "ivec2 p = ivec2(gl_FragCoord.xy / float(max((pc.flags >> 16) & 255u, "
                         "1u))) & 3;") != NULL);
    HT_CHECK(strstr(buf, "subpassLoad") == NULL);       /* blending: the pipeline's */
    k.dither = 2;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "dt / vec3(31.0)") != NULL);
    /* blended: the shader blends with the target's pixel, then dithers */
    k.dither = 1;
    k.fblend = 1;
    k.fb_src = 0x302;                                   /* SRCALPHA */
    k.fb_dst = 0x303;                                   /* INVSRCALPHA */
    k.fb_op = 0x8006;
    n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_dither_blend");
    HT_CHECK(strstr(buf, "uniform subpassInput uFb;") != NULL);
    const char *bl = strstr(buf, "vec4 sf = vec4(s.a), df = vec4(1.0 - s.a);\n"
                                 "    oColor = clamp(s * sf + d * df, 0.0, 1.0);");
    const char *dt = strstr(buf, "oColor.rgb = clamp(oColor.rgb + dt");
    HT_CHECK(bl != NULL && dt != NULL && bl < dt);
    k.fb_src = 0x8001;                                  /* constant colour, reverse subtract */
    k.fb_dst = 1;
    k.fb_op = 0x800B;
    k.fb_const = 0x80FF0000u;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "c = col(2164195328u);") != NULL);
    HT_CHECK(strstr(buf, "vec4 sf = c, df = vec4(1.0);") != NULL);
    HT_CHECK(strstr(buf, "oColor = clamp(d * df - s * sf, 0.0, 1.0);") != NULL);
}

/* Polygon coverage adjusts alpha before the alpha test. */
static void test_poly_smooth(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_V0, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "gl_BaryCoordNoPerspEXT") == NULL);
    HT_CHECK(strstr(buf, "GL_EXT_fragment_shader_barycentric") == NULL);
    k.poly_smooth = 1;
    k.alpha_test = 1;
    k.alpha_func = 4;                                   /* GREATER */
    k.tex_modes = HLE_PST_PROJECT2D;
    k.alpha_kill = 1;                                   /* a discard before the coverage's use */
    size_t n = hle_psh_glsl(&k, buf, sizeof(buf), NULL);
    HT_CHECK(n > 0 && n == strlen(buf));
    dump("psh_poly_smooth");
    HT_CHECK(strstr(buf, "#extension GL_EXT_fragment_shader_barycentric : require\n") != NULL);
    HT_CHECK(strstr(buf, "float(max((pc.flags >> 16) & 255u, 1u));") != NULL);
    /* Compute derivatives before discards; apply coverage before alpha testing. */
    const char *bc = strstr(buf, "vec3 bc = gl_BaryCoordNoPerspEXT, gx = dFdx(bc), gy = dFdy(bc);");
    const char *kill = strstr(buf, "if (t0.a == 0.0) discard;");
    const char *cv = strstr(buf, "outc.a *= pcov;");
    const char *at = strstr(buf, "if (!(a > r)) discard;");
    HT_CHECK(bc != NULL && kill != NULL && bc < kill);
    HT_CHECK(cv != NULL && at != NULL && cv < at);
}

static void test_overflow(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_COUNT] = 8;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, 1024, NULL) == 0);
}

/* Check combiner rounding against Xbox reference captures in float32. */
static float q9f(float x)
{
    return floorf(x * 255.0f + 0.500244140625f) / 255.0f;
}

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static float st(float v)                /* a stage's output: clamp, q9 */
{
    return q9f(clampf(v, -1.0f, 1.0f));
}

static unsigned unorm8(float x)         /* the target's conversion */
{
    return (unsigned)lrintf(clampf(x, 0.0f, 1.0f) * 255.0f);
}

static float sgn(int b)                 /* sgnob of an offset-binary texel */
{
    return fmaxf((((float)(b + 128) / 255.0f) * 255.0f - 128.0f) / 127.5f, -1.0f);
}

static float tshf(float x)              /* tsh: the texture shader's b / 127 */
{
    return fmaxf(x * (255.0f / 254.0f), -1.0f);
}

#define K(v)   ((float)(v) / 255.0f)
#define HALFF  (128.0f / 255.0f)

static void test_precision(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_C0, R_ZERO | INVERT, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = (R_R0 << 4) | (6u << 15);     /* r0 = C0 / 2 */
    rs[HLE_PS_FINAL_ABCD] = IN4(R_ZERO, R_ZERO, R_ZERO, R_SUM);
    rs[HLE_PS_FINAL_EFG] = IN4(R_ZERO, R_ZERO, R_R0 | CH_A, 0);
    rs[HLE_PS_COUNT] = 1;
    HlePshKey k;
    hle_psh_key(rs, &k);
    k.precise = 0;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "float q9(float x)") == NULL);
    HT_CHECK(strstr(buf, "ab = clamp((ab * 0.5), -1.0, 1.0);") != NULL);
    k.precise = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_precision");
    HT_CHECK(strstr(buf, "float q9(float x) { return floor(x * 255.0 + 0.500244140625) / 255.0; }")
             != NULL);
    HT_CHECK(strstr(buf, "ab = q9(clamp((ab * 0.5), -1.0, 1.0));") != NULL);
    HT_CHECK(strstr(buf, "vec4 v1r0 = vec4(clamp(v1.rgb, 0.0, 1.0) + clamp(r0.rgb, 0.0, 1.0), "
                         "0.0);") != NULL);
    HT_CHECK(strstr(buf, "vec3 D = max(v1r0.rgb, 0.0);") != NULL);      /* up to 2 */
    rs[HLE_PS_FINAL_EFG] = IN4(R_ZERO, R_ZERO, R_R0 | CH_A, 0xE0);
    hle_psh_key(rs, &k);
    k.precise = 1;
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    HT_CHECK(strstr(buf, "vec4 v1r0 = vec4((1.0 - clamp(v1.rgb, 0.0, 1.0)) + "
                         "(1.0 - clamp(r0.rgb, 0.0, 1.0)), 0.0);") != NULL);
    HT_CHECK(strstr(buf, "v1r0 = clamp(v1r0, 0.0, 1.0);") != NULL);

    /* Halving rounds up: 0x19 -> 13, 0xFF -> 128 (MSB set). */
    HT_EQ_U32(unorm8(st(K(25) * 0.5f)), 13);
    HT_EQ_U32(unorm8(st(K(63) * 0.5f)), 32);
    HT_EQ_U32(unorm8(st(K(255) * 0.5f)), 128);
    HT_CHECK(st(K(255) * 0.5f) > 0.5f);
    /* A near-tie from 5-bit inputs must still round down to 60. */
    const float t5 = (12.0f / 31.0f) * (19.0f / 31.0f);
    HT_EQ_U32(unorm8(st(t5)), 60);
    HT_EQ_U32((unsigned)(st(t5) * 255.0f + 0.5f) & 1u, 0);
    /* Float32 error must not round a true tie down. */
    HT_EQ_U32(unorm8(st(nextafterf(K(25) * 0.5f, 0.0f))), 13);
    /* Check exact half-up behavior across 8-bit products and sums. */
    unsigned bad = 0;
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned c = 0; c < 256; c++) {
            bad += (unsigned)(st(K(a) * K(c)) * 255.0f + 0.5f) != (2 * a * c + 255) / 510;
            bad += (unsigned)(st(K(a) * K(c) * 0.5f) * 255.0f + 0.5f) != (a * c + 255) / 510;
            const unsigned d = (a * 37 + c * 101) & 255, e = (a * 211 + c * 29 + 7) & 255;
            const float s2 = st(K(a) * K(c) + K(d) * K(e));
            const unsigned x2 = a * c + d * e;
            bad += x2 <= 65025 && (unsigned)(s2 * 255.0f + 0.5f) != (2 * x2 + 255) / 510;
        }
    }
    HT_EQ_U32(bad, 0);
    /* Finer texture values can fall inside the float32 tie tolerance;
     * Xbox register expansion remains unverified. */
    const float nt = (4.0f / 31.0f) * (19.0f / 31.0f) + (2.0f / 15.0f) * (5.0f / 15.0f);
    HT_EQ_U32(unorm8(st(nt)), 32);
    HT_EQ_U32(unorm8(st(K(33) * K(156) + K(34) * K(85))), 32);
    /* Combiners use signed /127.5; texture shader reads use /127. */
    for (int v = -128; v < 128; v++) {
        bad += fabsf(tshf(sgn(v)) - fmaxf((float)v / 127.0f, -1.0f)) > 1e-6f;
    }
    HT_EQ_U32(bad, 0);
    HT_CHECK(tshf(sgn(127)) == 1.0f && tshf(sgn(-128)) == -1.0f && tshf(sgn(-127)) == -1.0f);
    HT_EQ_U32(unorm8(st(K(255) - HALFF)), 127);
    HT_EQ_U32(unorm8(st((K(255) - HALFF) * 2.0f)), 254);
    /* SignedCombinerOps: 0xBF + (-0x1F) / 2 = 176, 0xBF + (-0x1F - 0.5) = 32 */
    HT_EQ_U32(unorm8(st(K(191) + st(-K(31) * 0.5f))), 176);
    HT_EQ_U32(unorm8(st(K(191) + st(-K(31) - HALFF))), 32);
    /* InputMappings, map(r1) * 0x7F + 0x7F: HALFBIAS of -0x66 63, HALFBIAS_
     * NEGATE 191, UNSIGNED_INVERT of 0xB2 165 (one rounding, not two) */
    const float r1 = st(-K(102));
    HT_EQ_U32(unorm8(st((fmaxf(r1, 0.0f) - HALFF) * K(127) + K(127))), 63);
    HT_EQ_U32(unorm8(st((HALFF - fmaxf(r1, 0.0f)) * K(127) + K(127))), 191);
    HT_EQ_U32(unorm8(st((1.0f - K(178)) * K(127) + K(127))), 165);
    /* ShiftClamping: (0x5F + 0x5F - 0.5) * 2 = 124, (0xBF + 0xBF) / 2 = 191 */
    HT_EQ_U32(unorm8(st((K(95) + K(95) - HALFF) * 2.0f)), 124);
    HT_EQ_U32(unorm8(st((K(191) + K(191)) * 0.5f)), 191);
    /* SpecularR0Sum: v1 + r0 of halves rounded each, 77 128 166; of a
     * negative v1 (BIAS) r0's 63 */
    const float r0h = st(K(127) * 0.5f);
    HT_EQ_U32(unorm8(clampf(st(K(25) * 0.5f), 0, 1) + clampf(r0h, 0, 1)), 77);
    HT_EQ_U32(unorm8(clampf(st(K(127) * 0.5f), 0, 1) + clampf(r0h, 0, 1)), 128);
    HT_EQ_U32(unorm8(clampf(st(K(204) * 0.5f), 0, 1) + clampf(r0h, 0, 1)), 166);
    HT_EQ_U32(unorm8(clampf(st(K(25) - HALFF), 0, 1) + clampf(st(K(191) - HALFF), 0, 1)), 63);
    /* SpecularR0SumFlags: 0x7F * (1 + 1), the sum unclamped: 254 */
    HT_EQ_U32(unorm8(K(127) * (1.0f + 1.0f)), 254);
    /* SignedToUnsignedMapping, five stages: t0 0x66, t2 0x40, t3.b -0x20
     * signed; r1 = t2 * t3.b, r1 = 4 r1 r0, r0 + r1: 102 (signed), 204
     * (t3.b unsigned: 0); and t0 alone reads 0xCC, not 0xCD */
    for (unsigned u = 0; u < 2; u++) {
        float r0 = st(sgn(102) * 1.0f);
        float r1a = st(u ? fmaxf(sgn(-32), 0.0f) : sgn(-32));
        float r1 = st(sgn(64) * r1a);
        r1 = st(r1 * r0 * 4.0f);
        HT_EQ_U32(unorm8(st(r0 + r1)), u ? 204 : 102);
    }
}

static void test_precision_mode(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    HlePshKey k;
    HT_EQ_U32(sizeof(HlePshKey), 216);
    HT_EQ_U32(offsetof(HlePshKey, precise), offsetof(HlePshKey, fb_op) + sizeof(uint16_t));
    HT_EQ_U32(offsetof(HlePshKey, fb_const), 208);
    hle_psh_set_precision(false);
    hle_psh_key(rs, &k);
    HT_EQ_U32(k.precise, 0);
    hle_psh_set_precision(true);
    hle_psh_key(rs, &k);
    HT_EQ_U32(k.precise, 1);
#if defined(__x86_64__) && !defined(__ANDROID__)
    hle_psh_set_precision(true);
#else
    hle_psh_set_precision(false);
#endif
}

/* Check shader output with GL-style clamp disabled and enabled. */
static void test_gl_clamp(void)
{
    uint32_t rs[HLE_PS_REGS] = { 0 };
    rs[HLE_PS_RGB_IN] = IN4(R_T0, R_T1, R_ZERO, R_ZERO);
    rs[HLE_PS_ALPHA_IN] = IN4(R_T0 | CH_A, R_T1 | CH_A, R_ZERO, R_ZERO);
    rs[HLE_PS_RGB_OUT] = R_R0 << 4;
    rs[HLE_PS_ALPHA_OUT] = R_R0 << 4;
    rs[HLE_PS_COUNT] = 1;
    rs[HLE_PS_TEXMODES] = HLE_PST_PROJECT2D | HLE_PST_PROJECT2D << 5;
    HlePshKey k;
    hle_psh_key(rs, &k);
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    /* Keep unclamped shader text stable for the disk cache. */
    HT_CHECK(strstr(buf, "GLC") == NULL && strstr(buf, "GLT") == NULL);
    HT_CHECK(strstr(buf, "  return texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv);\n") != NULL);
    k.tex_modes |= 3u << 20 | 4u << 23;                 /* t0 u and v, t1 w */
    HT_CHECK(hle_psh_glsl(&k, buf, sizeof(buf), NULL) > 0);
    dump("psh_gl_clamp");
    HT_CHECK(strstr(buf, "const uint GLCLAMP = 0x023u;") != NULL);
    HT_CHECK(strstr(buf, "#define GLC2(s, v) glc2(s, v)") != NULL);
    HT_CHECK(strstr(buf, "textureGrad(t, glc2(s, v), dFdx(v), dFdy(v))") != NULL);
    HT_CHECK(strstr(buf, "clamp(v.x, 0.0, 0.99999994)") != NULL);
    HT_CHECK(strstr(buf, "GLT3(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), s, uvw)") != NULL);
}

int main(void)
{
    HT_RUN(test_modulate);
    HT_RUN(test_gl_clamp);
    HT_RUN(test_final_fog);
    HT_RUN(test_dot_mux);
    HT_RUN(test_mux_bias);
    HT_RUN(test_reflect);
    HT_RUN(test_dot_zw);
    HT_RUN(test_dot_zw_formats);
    HT_RUN(test_brdf);
    HT_RUN(test_shadow_project);
    HT_RUN(test_shadow_f24);
    HT_RUN(test_bumpenv);
    HT_RUN(test_two_side);
    HT_RUN(test_color_key);
    HT_RUN(test_convolution);
    HT_RUN(test_sign);
    HT_RUN(test_sign_color_key);
    HT_RUN(test_dither);
    HT_RUN(test_aa_control);
    HT_RUN(test_poly_smooth);
    HT_RUN(test_overflow);
    HT_RUN(test_precision_mode);
    HT_RUN(test_precision);
    HT_DONE();
}
