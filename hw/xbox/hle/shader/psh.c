/*
 * hakuX HLE GPU — Xbox pixel shaders -> GLSL (pure). See psh.h.
 *
 * Register combiners (D3DPIXELSHADERDEF, NV_register_combiners on NV2A):
 * - input byte: bits 0-3 register, bit 4 alpha channel (RGB portion: .aaa;
 *   alpha portion: .a, else .b), bits 5-7 input mapping;
 * - output word: bits 0-3 CD, 4-7 AB, 8-11 SUM/MUX register, bit 12 CD dot,
 *   13 AB dot, 14 MUX, 15-17 output mapping, 18 CD blue-to-alpha, 19 AB;
 * - every input of a stage is read before any output is written; stage
 *   results are clamped to [-1, 1] and kept in 9-bit signed fixed point,
 *   1.0 = 0xFF (rounded half up: q9); the bias mappings' 0.5 is 0x80;
 *   r0.a starts as t0.a;
 * - PSCombinerCount: bits 0-7 stages, bit 8 MUX on r0.a's MSB (else LSB),
 *   bit 12 a C0 per stage (else stage 0's), bit 16 the same for C1;
 * - final combiner: rgb = A*B + (1-A)*C + D, alpha = G, inputs clamped to
 *   [0, 1]; EF_PROD = E*F, V1R0_SUM = v1 + r0, each clamped to [0, 1]
 *   first, the sum up to 2 unless clamped (EFG flags: 0x80 clamp, 0x40
 *   complement v1, 0x20 complement r0). Without one, the output is r0.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/psh.h"

typedef struct Buf {
    char *p;
    size_t cap, len;
    bool overflow;
} Buf;

/* In combiner fixed point, the half-bias constant is 0x80/0xFF. */
#define HALF "(128.0 / 255.0)"

#if defined(__x86_64__) && !defined(__ANDROID__)
static bool g_precise_combiners = true;
#else
static bool g_precise_combiners;
#endif

void hle_psh_set_precision(bool enabled)
{
    g_precise_combiners = enabled;
}

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->len < b->cap ? b->cap - b->len : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || b->len + (size_t)n >= b->cap) {
        b->overflow = true;
        b->len = b->cap;
        return;
    }
    b->len += (size_t)n;
}

void hle_psh_key(const uint32_t *rs, HlePshKey *k)
{
    memset(k, 0, sizeof(*k));
    k->precise = g_precise_combiners;
    unsigned n = rs[HLE_PS_COUNT] & 0xFF;
    n = n > 8 ? 8 : n;
    for (unsigned s = 0; s < n; s++) {      /* stages past the count do not split keys */
        k->rgb_in[s] = rs[HLE_PS_RGB_IN + s];
        k->rgb_out[s] = rs[HLE_PS_RGB_OUT + s];
        k->alpha_in[s] = rs[HLE_PS_ALPHA_IN + s];
        k->alpha_out[s] = rs[HLE_PS_ALPHA_OUT + s];
    }
    k->final_abcd = rs[HLE_PS_FINAL_ABCD];
    k->final_efg = rs[HLE_PS_FINAL_EFG];
    k->count = rs[HLE_PS_COUNT] & 0x111FF;
    k->tex_modes = rs[HLE_PS_TEXMODES] & 0xFFFFF;     /* Clamp axes are added by draw.c. */
    k->dot_map = rs[HLE_PS_DOTMAP] & 0xFFF;
    k->input_tex = rs[HLE_PS_INPUTTEX] & 0x310000;
    k->compare = rs[HLE_PS_COMPARE] & 0xFFFF;
}

/* Register as a vec4 expression; final = the final combiner. */
static const char *reg(unsigned r, bool final)
{
    static const char *const k_reg[16] = {
        "vec4(0.0)", "C0", "C1", "fogr", "v0", "v1", "vec4(0.0)", "vec4(0.0)",
        "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "efp",
    };
    if (!final && r >= 14) {
        return "vec4(0.0)";
    }
    return k_reg[r & 15];
}

/* Input of a combiner: alpha = the alpha portion (float), else vec3. */
static void in_expr(Buf *b, uint8_t in, bool alpha, bool final)
{
    char x[48];
    const char *r = reg(in & 0xF, final);
    bool a = (in & 0x10) != 0;
    if (alpha) {
        snprintf(x, sizeof(x), "%s.%c", r, a ? 'a' : 'b');
    } else {
        snprintf(x, sizeof(x), a ? "%s.aaa" : "%s.rgb", r);
    }
    switch ((in >> 5) & 7) {
    case 0: put(b, "max(%s, 0.0)", x); break;                    /* UNSIGNED_IDENTITY */
    case 1: put(b, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;     /* UNSIGNED_INVERT */
    case 2: put(b, "(2.0 * max(%s, 0.0) - 1.0)", x); break;      /* EXPAND_NORMAL */
    case 3: put(b, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;     /* EXPAND_NEGATE */
    case 4: put(b, "(max(%s, 0.0) - " HALF ")", x); break;       /* HALFBIAS_NORMAL */
    case 5: put(b, "(-max(%s, 0.0) + " HALF ")", x); break;      /* HALFBIAS_NEGATE */
    case 6: put(b, "(%s)", x); break;                            /* SIGNED_IDENTITY */
    default: put(b, "(-(%s))", x); break;                        /* SIGNED_NEGATE */
    }
}

/* A stage's result v through its output mapping. */
static void out_expr(char *e, size_t n, uint32_t out, const char *v)
{
    switch ((out >> 15) & 7) {
    case 1: snprintf(e, n, "(%s - " HALF ")", v); break;            /* BIAS */
    case 2: snprintf(e, n, "(%s * 2.0)", v); break;                 /* SHIFTLEFT_1 */
    case 3: snprintf(e, n, "((%s - " HALF ") * 2.0)", v); break;    /* SHIFTLEFT_1_BIAS */
    case 4: snprintf(e, n, "(%s * 4.0)", v); break;                 /* SHIFTLEFT_2 */
    case 6: snprintf(e, n, "(%s * 0.5)", v); break;                 /* SHIFTRIGHT_1 */
    default: snprintf(e, n, "(%s)", v); break;
    }
}

/* Writable combiner outputs. */
static const char *dst(unsigned r)
{
    switch (r & 15) {
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    default: return NULL;                               /* DISCARD and read-only */
    }
}

static void stage(Buf *b, const HlePshKey *k, unsigned s)
{
    uint32_t ri = k->rgb_in[s], ro = k->rgb_out[s], ai = k->alpha_in[s], ao = k->alpha_out[s];
    unsigned c0 = (k->count & 0x1000) ? s : 0, c1 = 8 + ((k->count & 0x10000) ? s : 0);
    put(b, "  { /* combiner stage %u */\n"
           "    vec4 C0 = col(pc.psc[%u]), C1 = col(pc.psc[%u]);\n", s, c0, c1);
    static const char k_v[4] = { 'A', 'B', 'C', 'D' };
    for (unsigned i = 0; i < 4; i++) {
        put(b, "    vec3 %c = ", k_v[i]);
        in_expr(b, (ri >> (24 - 8 * i)) & 0xFF, false, false);
        put(b, ";\n    float %ca = ", k_v[i]);
        in_expr(b, (ai >> (24 - 8 * i)) & 0xFF, true, false);
        put(b, ";\n");
    }
    /* MSB: bit 7 of the 8-bit value (D3D cnd: r0.a > 0.5, 0x80 and up);
     * LSB: bit 0, of a two's complement value where r0.a is negative */
    put(b, "    bool mx = %s;\n", (k->count & 0x100) ? "r0.a > 0.5"
                                                   : "(int(floor(r0.a * 255.0 + 0.5)) & 1) == 1");
    put(b, "    vec3 ab = %s, cd = %s;\n", (ro & 0x2000) ? "vec3(dot(A, B))" : "A * B",
        (ro & 0x1000) ? "vec3(dot(C, D))" : "C * D");
    put(b, "    vec3 ms = %s;\n", (ro & 0x4000) ? "mx ? cd : ab" : "ab + cd");
    put(b, "    float aab = Aa * Ba, acd = Ca * Da, ams = %s;\n",
        (ao & 0x4000) ? "mx ? acd : aab" : "aab + acd");
    char e[64];
    static const char *const k_rv[3] = { "ab", "cd", "ms" };
    static const char *const k_av[3] = { "aab", "acd", "ams" };
    for (unsigned i = 0; i < 3; i++) {
        out_expr(e, sizeof(e), ro, k_rv[i]);
        put(b, k->precise ? "    %s = q9(clamp(%s, -1.0, 1.0));\n"
                          : "    %s = clamp(%s, -1.0, 1.0);\n", k_rv[i], e);
        out_expr(e, sizeof(e), ao, k_av[i]);
        put(b, k->precise ? "    %s = q9(clamp(%s, -1.0, 1.0));\n"
                          : "    %s = clamp(%s, -1.0, 1.0);\n", k_av[i], e);
    }
    /* outputs: AB bits 4-7, CD bits 0-3, SUM bits 8-11 */
    static const unsigned k_shift[3] = { 4, 0, 8 };
    for (unsigned i = 0; i < 3; i++) {
        const char *d = dst(ro >> k_shift[i]);
        if (d) {
            put(b, "    %s.rgb = %s;\n", d, k_rv[i]);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        const char *d = dst(ao >> k_shift[i]);
        if (d) {
            put(b, "    %s.a = %s;\n", d, k_av[i]);
        }
    }
    if (ro & 0x80000) {                                 /* AB_BLUE_TO_ALPHA */
        const char *d = dst(ro >> 4);
        if (d) {
            put(b, "    %s.a = ab.b;\n", d);
        }
    }
    if (ro & 0x40000) {                                 /* CD_BLUE_TO_ALPHA */
        const char *d = dst(ro);
        if (d) {
            put(b, "    %s.a = cd.b;\n", d);
        }
    }
    put(b, "  }\n");
}

static void final_combiner(Buf *b, const HlePshKey *k)
{
    uint32_t abcd = k->final_abcd, efg = k->final_efg;
    put(b, "  vec4 outc;\n");
    if (!abcd && !efg) {
        /* No final combiner in the shader: the one D3D's fog setters push
         * (Otogi 0xfc362 / 0xfc3ce): A = fog.a, B = r0 or, with
         * SPECULARENABLE, v1 + r0 (clamped), C = fog.rgb, G = r0.a; without
         * fog, D = B (fogf is 1 then) */
        put(b, "  outc = vec4(mix(fogr.rgb, clamp(%s, 0.0, 1.0), fogf), clamp(r0.a, 0.0, 1.0));\n",
            k->specular ? "clamp(v1.rgb, 0.0, 1.0) + clamp(r0.rgb, 0.0, 1.0)" : "r0.rgb");
        return;
    }
    put(b, "  {\n    vec4 C0 = col(pc.psc[16]), C1 = col(pc.psc[17]);\n");
    put(b, "    vec3 E = clamp(");
    in_expr(b, (efg >> 24) & 0xFF, false, false);
    put(b, ", 0.0, 1.0);\n    vec3 F = clamp(");
    in_expr(b, (efg >> 16) & 0xFF, false, false);
    put(b, ", 0.0, 1.0);\n    vec4 efp = vec4(E * F, 0.0);\n");
    /* Clamp v1 and r0 separately before the optional complements. */
    put(b, "    vec4 v1r0 = vec4(%s + %s, 0.0);\n",
        (efg & 0x40) ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "clamp(v1.rgb, 0.0, 1.0)",
        (efg & 0x20) ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "clamp(r0.rgb, 0.0, 1.0)");
    if (efg & 0x80) {
        put(b, "    v1r0 = clamp(v1r0, 0.0, 1.0);\n");
    }
    static const char k_v[4] = { 'A', 'B', 'C', 'D' };
    for (unsigned i = 0; i < 4; i++) {
        /* The unclamped sum can reach 2.0. */
        const uint8_t in = (abcd >> (24 - 8 * i)) & 0xFF;
        const bool sum = (in & 0xEF) == 14;
        put(b, "    vec3 %c = %s", k_v[i], sum ? "" : "clamp(");
        in_expr(b, in, false, true);
        put(b, "%s;\n", sum ? "" : ", 0.0, 1.0)");
    }
    put(b, "    float G = clamp(");
    in_expr(b, (efg >> 8) & 0xFF, true, true);
    put(b, ", 0.0, 1.0);\n"
           "    outc = vec4(A * B + (1.0 - A) * C + D, G);\n  }\n");
}

/* The convolution filters at the texture's own texels (guest ones: a scaled
 * surface's are the resolution scale's wide), level 0: taps through the
 * stage's level-0 sampler (pc.tex bits 24-31, vk/draw.c sampler_lod0), at
 * an explicit LOD, whatever the stage's MAXMIPLEVEL and LOD bias. sampk's
 * when: bit 0 minification, 1 magnification; else the stage's sampler, by
 * the gradients taken before the branch (a quad's fragments may take either
 * side, where implicit derivatives are undefined). Quincunx takes the
 * centre at 1/2 and the four diagonal neighbours at 1/8 each; gaussian the
 * 3 x 3 neighbourhood weighted 1 2 1 by 1 2 1, over 16. */
static const char k_conv_glsl[] =
    "vec4 tap(uint ti, uint ci, vec2 uv) {\n"
    "  return textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(ci)]), uv, "
    "0.0);\n"
    "}\n"
    "vec4 sampk(uint s, vec2 uv, uint kern, uint when) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0xFFu, ci = pc.tex[s] >> 24;\n"
    "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
    "  float sc = (pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0;\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= sz / sc;\n"
    "  vec2 gx = dFdx(uv), gy = dFdy(uv);\n"
    "  float lod = textureQueryLod(sampler2D(uTex2D[nonuniformEXT(ti)], "
    "uSmp[nonuniformEXT(si)]), uv).y;\n"
    "  if ((when & (lod > 0.0 ? 1u : 2u)) == 0u)\n"
    "    return textureGrad(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
    "uv, gx, gy);\n"
    "  vec2 d = sc / sz;\n"
    "  vec4 diag = tap(ti, ci, uv - d) + tap(ti, ci, uv + d) +\n"
    "              tap(ti, ci, uv + vec2(d.x, -d.y)) + tap(ti, ci, uv + vec2(-d.x, d.y));\n"
    "  if (kern == 1u)\n"
    "    return 0.5 * tap(ti, ci, uv) + 0.125 * diag;\n"
    "  vec4 edge = tap(ti, ci, uv + vec2(d.x, 0.0)) + tap(ti, ci, uv - vec2(d.x, 0.0)) +\n"
    "              tap(ti, ci, uv + vec2(0.0, d.y)) + tap(ti, ci, uv - vec2(0.0, d.y));\n"
    "  return (4.0 * tap(ti, ci, uv) + 2.0 * edge + diag) / 16.0;\n"
    "}\n";
/* Apply GL-style clamp to each convolution tap. */
static const char k_conv_glc_glsl[] =
    "vec4 tap(uint ti, uint ci, vec2 uv) {\n"
    "  return textureLod(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(ci)]), uv, "
    "0.0);\n"
    "}\n"
    "vec4 sampk(uint s, vec2 uv, uint kern, uint when) {\n"
    "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0xFFu, ci = pc.tex[s] >> 24;\n"
    "  vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
    "  float sc = (pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0;\n"
    "  if ((pc.flags & (1u << s)) != 0u)\n"
    "    uv /= sz / sc;\n"
    "  vec2 gx = dFdx(uv), gy = dFdy(uv);\n"
    "  float lod = textureQueryLod(sampler2D(uTex2D[nonuniformEXT(ti)], "
    "uSmp[nonuniformEXT(si)]), uv).y;\n"
    "  if ((when & (lod > 0.0 ? 1u : 2u)) == 0u)\n"
    "    return textureGrad(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
    "GLC2(s, uv), gx, gy);\n"
    "  vec2 d = sc / sz;\n"
    "  vec4 diag = tap(ti, ci, GLC2(s, uv - d)) + tap(ti, ci, GLC2(s, uv + d)) +\n"
    "              tap(ti, ci, GLC2(s, uv + vec2(d.x, -d.y))) + tap(ti, ci, GLC2(s, uv + vec2(-d.x, d.y)));\n"
    "  if (kern == 1u)\n"
    "    return 0.5 * tap(ti, ci, GLC2(s, uv)) + 0.125 * diag;\n"
    "  vec4 edge = tap(ti, ci, GLC2(s, uv + vec2(d.x, 0.0))) + tap(ti, ci, GLC2(s, uv - vec2(d.x, 0.0))) +\n"
    "              tap(ti, ci, GLC2(s, uv + vec2(0.0, d.y))) + tap(ti, ci, GLC2(s, uv - vec2(0.0, d.y)));\n"
    "  return (4.0 * tap(ti, ci, GLC2(s, uv)) + 2.0 * edge + diag) / 16.0;\n"
    "}\n";

/* A D3D blend factor (Vulkan's as vk/draw.c blend_factor maps them) as a
 * vec4 of the source s, the destination d and the constant c */
static const char *blend_factor(uint32_t f)
{
    switch (f) {
    case 0:      return "vec4(0.0)";
    case 0x300:  return "s";
    case 0x301:  return "(1.0 - s)";
    case 0x302:  return "vec4(s.a)";
    case 0x303:  return "vec4(1.0 - s.a)";
    case 0x304:  return "vec4(d.a)";
    case 0x305:  return "vec4(1.0 - d.a)";
    case 0x306:  return "d";
    case 0x307:  return "(1.0 - d)";
    case 0x308:  return "vec4(vec3(min(s.a, 1.0 - d.a)), 1.0)";
    case 0x8001: return "c";
    case 0x8002: return "(1.0 - c)";
    case 0x8003: return "vec4(c.a)";
    case 0x8004: return "vec4(1.0 - c.a)";
    default:     return "vec4(1.0)";
    }
}

/* The blend equation (vk/draw.c blend_op's): MIN and MAX ignore the factors */
static const char *blend_eq(uint32_t o)
{
    switch (o) {
    case 0x800A: return "s * sf - d * df";
    case 0x800B: return "d * df - s * sf";
    case 0x8007: return "min(s, d)";
    case 0x8008: return "max(s, d)";
    default:     return "s * sf + d * df";
    }
}

void hle_psh_aa_key(HlePshKey *k, uint32_t aactl, uint32_t xs, uint32_t ys, bool colour, bool depth)
{
    k->aa_grid = k->aa_ctl = 0;
    xs = xs > 2 ? 2 : xs;
    ys = ys > 2 ? 2 : ys;
    if (!(colour || depth) || !(aactl & 1) || xs * ys < 2) {
        return;
    }
    const uint32_t all = (1u << (xs * ys)) - 1, mask = (aactl >> 16) & all;
    const uint32_t a2c = (aactl >> 4) & 1, a2one = (aactl >> 8) & 1;
    if (mask == all && !a2c && !a2one) {
        return;
    }
    k->aa_grid = (uint8_t)(xs | ys << 2);
    k->aa_ctl = (uint8_t)(a2c | a2one << 1 | mask << 4);
}

static const char *k_cmp[8] = {
    "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", "true",
};

/* Shadow buffer at stage s: the stored depth compared with the r coordinate
 * (depth FUNC r, both in the format's range; PROJECT2D compares with 0),
 * the four results bilinearly weighted (percentage-closer filtering). The
 * stored depth is scaled to 0..1 but for an uploaded F24 texture's (format
 * 0), its float as it is: F24 reaches far past F16's 511.9375. */
static void shadow_sample(Buf *b, const HlePshKey *k, unsigned s, bool project3d)
{
    char glcl[40] = "";                 /* Clamp before gathering depth. */
    if (k->tex_modes >> 20) {
        snprintf(glcl, sizeof(glcl), "    uv = GLC2(%uu, uv);\n", s);
    }
    static const char *const k_max[4] = { "3.402823466e38", "16777215.0", "65535.0", "511.9375" };
    static const char *const k_scale[4] = { "1.0", "16777215.0", "65535.0", "511.9375" };
    static const char *const k_op[8] = { "", "lessThan", "equal", "lessThanEqual", "greaterThan",
                                         "notEqual", "greaterThanEqual", "" };
    unsigned func = (k->shadow >> 4) & 7, fmt = (k->shadow_fmt >> (2 * s)) & 3;
    if (func == 0 || func == 7) {
        put(b, "  vec4 t%u = vec4(%s);\n", s, func ? "1.0" : "0.0");
        return;
    }
    /* F24 compares at its precision: r's mantissa cut to 16 bits, as the
     * depth stored (nxdk_pgraph_tests' 0.5e30 matches its texel) */
    const char *r = !project3d ? "0.0" : fmt ? "clamp(pq.z, 0.0, m)" :
                    "uintBitsToFloat(floatBitsToUint(clamp(pq.z, 0.0, m)) & 0xFFFFFF80u)";
    put(b, "  vec4 t%u;\n  { uint ti = pc.tex[%u] & 0xFFFFu, si = (pc.tex[%u] >> 16) & 0xFFu;\n"
           "    vec3 pq = nvproj(vTex%u);\n"
           "    vec2 uv = pq.xy;\n"
           "    vec2 sz = vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0));\n"
           "    if ((pc.flags & (1u << %uu)) != 0u)\n"
           /* a scaled surface's texel coordinates: sz / the resolution scale */
           "      uv /= sz / ((pc.flags & (256u << %uu)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
           "%s"
           "    float m = %s, r = %s;\n"
           "    vec4 d = textureGather(sampler2D(uTex2D[nonuniformEXT(ti)], "
           "uSmp[nonuniformEXT(si)]), uv) * %s;\n"
           "    vec4 p = vec4(%s(d, vec4(r)));\n"
           "    vec2 f = fract(uv * sz - 0.5);\n"
           "    t%u = vec4(mix(mix(p.w, p.z, f.x), mix(p.x, p.y, f.x), f.y));\n",
        s, s, s, s, s, s, glcl, k_max[fmt], r, k_scale[fmt], k_op[func], s);
    if (k->debug == 17) {               /* debug view 17: stored depth, r, result */
        put(b, "    shdbg = vec4(d.x / m, r / m, t%u.x, 1.0);\n", s);
    }
    put(b, "  }\n");
}

/* Mode m fetches a texel from its stage's texture (the others' result is a
 * constant or their coordinates) */
static bool fetches(unsigned m)
{
    return m != HLE_PST_NONE && m != HLE_PST_PASSTHRU && m != HLE_PST_CLIPPLANE &&
           m != HLE_PST_DOTPRODUCT && m != HLE_PST_DOT_ZW;
}

/* Texture colour sign: stage s's signed channels (HlePshKey sgn_ob,
 * sgn_raw) as the NV2A reads them, before anything uses the texel. */
static void sign_decode(Buf *b, const HlePshKey *k, unsigned s, unsigned m)
{
    const unsigned ob = (k->sgn_ob >> (4 * s)) & 15, raw = (k->sgn_raw >> (4 * s)) & 15 & ~ob;
    if (!fetches(m)) {
        return;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (((ob | raw) >> c) & 1) {
            put(b, "  t%u.%c = %s(t%u.%c);\n", s, "rgba"[c], (ob >> c) & 1 ? "sgnob" : "sgnraw",
                s, "rgba"[c]);
        }
    }
}

/* Texture-shader coordinates use signed bytes / 127; combiner inputs
 * use / 127.5. Keep these conversions separate. */
static const char *tex_read(char *e, size_t n, const HlePshKey *k, unsigned t)
{
    const unsigned m = (k->tex_modes >> (5 * t)) & 0x1F;
    const unsigned sg = ((k->sgn_ob | k->sgn_raw) >> (4 * t)) & 15;
    if (!sg || !fetches(m)) {
        snprintf(e, n, "t%u", t);
    } else {
        snprintf(e, n, "tsh(t%u, bvec4(%s, %s, %s, %s))", t, sg & 1 ? "true" : "false",
                 sg & 2 ? "true" : "false", sg & 4 ? "true" : "false", sg & 8 ? "true" : "false");
    }
    return e;
}

/* Colour key (HlePshKey ckey) and colour sign (sign_decode) on stage s's
 * fetched texel. A texel that matches the stage's key colour (its ARGB, or
 * RGB where the format's alpha isn't compared) has its alpha or ARGB
 * zeroed, which later combiner stages may ignore, or kills the fragment.
 * After filtering: a linear-filtered edge between keyed and other texels
 * matches neither. The key compares the texel as the guest stores it (an
 * offset-binary channel's top bit flipped back), and its zero is written
 * after the sign is decoded: a signed channel's 0, not the -1 an
 * offset-binary 0 decodes to. */
static void color_key(Buf *b, const HlePshKey *k, unsigned s, unsigned m)
{
    /* levels a channel (r g b, a); an alpha not compared has 0: ckeq's
     * floor(x * 0 + 0.5) is 0 for both */
    static const char *const k_n[4][2] = {
        { "vec4(255.0, 255.0, 255.0, 0.0)", "vec4(255.0)" },
        { "vec4(31.0, 63.0, 31.0, 0.0)", "vec4(31.0, 63.0, 31.0, 0.0)" },
        { "vec4(31.0, 31.0, 31.0, 0.0)", "vec4(31.0, 31.0, 31.0, 1.0)" },
        { "vec4(15.0, 15.0, 15.0, 0.0)", "vec4(15.0)" } };
    const unsigned op = (k->ckey >> (2 * s)) & 3, bits = (k->ckey_bits >> (2 * s)) & 3;
    const unsigned alpha = (k->ckey_alpha >> s) & 1;
    const unsigned ob = (k->sgn_ob >> (4 * s)) & 15;
    const bool sgn = ((k->sgn_ob | k->sgn_raw) >> (4 * s)) & 15;
    const uint32_t c = k->ckey_col[s];
    if (!op || !fetches(m)) {
        sign_decode(b, k, s, m);
        return;
    }
    char t[160], act[24];
    if (ob) {                           /* offset binary: 8-bit channels (ckey_bits 0) */
        snprintf(t, sizeof(t), "mix(t%u, mod(floor(t%u * 255.0 + 0.5) + 128.0, 256.0) / 255.0, "
                 "vec4(%u.0, %u.0, %u.0, %u.0))", s, s, ob & 1, (ob >> 1) & 1, (ob >> 2) & 1,
                 ob >> 3);
    } else {
        snprintf(t, sizeof(t), "t%u", s);
    }
    switch (op) {
    case 1: snprintf(act, sizeof(act), "t%u.a = 0.0", s); break;
    case 2: snprintf(act, sizeof(act), "t%u = vec4(0.0)", s); break;
    default: snprintf(act, sizeof(act), "discard"); break;
    }
    if (op == 3 || !sgn) {
        put(b, "  if (ckeq(%s, vec4(%u.0, %u.0, %u.0, %u.0) / 255.0, %s)) %s;\n", t,
            (c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24, k_n[bits][alpha], act);
        sign_decode(b, k, s, m);
        return;
    }
    put(b, "  bool ck%u = ckeq(%s, vec4(%u.0, %u.0, %u.0, %u.0) / 255.0, %s);\n", s, t,
        (c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24, k_n[bits][alpha]);
    sign_decode(b, k, s, m);
    put(b, "  if (ck%u) %s;\n", s, act);
}

/* D3DTSS_ALPHAKILL: a fetched texel with alpha 0 discards the fragment. A
 * shadow-buffer stage's result is its alpha too: projected shadow receivers
 * (PGR2's car shadow) draw only where the compare passed. */
static void alpha_kill(Buf *b, const HlePshKey *k, unsigned s, unsigned m)
{
    if (((k->alpha_kill >> s) & 1) && fetches(m)) {
        put(b, "  if (t%u.a == 0.0) discard;\n", s);
    }
}

size_t hle_psh_glsl(const HlePshKey *k, char *buf, size_t cap, unsigned *unsupported)
{
    Buf b = { buf, cap, 0, false };
    const char *interp = k->flat ? "flat " : "";
    /* Register-combiner outputs use signed 9-bit values (1.0 = 0xFF).
     * Round each stage after mapping and clamping, as in Xbox combiner
     * captures (116/116 matching pixels). Inputs remain interpolated. */
    unsigned unsup = 0;
    /* two-sided lighting: the back colours (vsh.h's outputs 7, 8) */
    char back[160] = "";
    if (k->two_side) {
        snprintf(back, sizeof(back), "layout(location = 7) %sin vec4 vBack0;\n"
                 "layout(location = 8) %sin vec4 vBack1;\n", interp, interp);
    }
    /* PSDotMapping HILO_1 (4) in a stage's field: dmap reads the 32-bit texel
     * as two 16-bit halves, hi A:R and lo G:B, over 65535 (as xemu). Read as
     * its R and G bytes mapped to -1..1, the depth buffer Halo 2's fog pass reads as
     * A8R8G8B8 (its top 16 bits in A:R) gave its fog-table lookup depth bits
     * 8-15, which wrap every 256 steps: stripes of fog and none across the
     * scenery, moving with the camera. */
    bool hilo1 = false;
    for (unsigned s = 0; s < 3; s++) {
        hilo1 |= ((k->dot_map >> (4 * s)) & 7) == 4;
    }
    put(&b, "#version 450\n#extension GL_EXT_nonuniform_qualifier : require\n%s%s",
        k->poly_smooth ? HLE_BARY_EXT_GLSL : "",
        hle_push_glsl());
    if (k->precise) {
        put(&b, "float q9(float x) { return floor(x * 255.0 + 0.500244140625) / 255.0; }\n"
                "vec3 q9(vec3 x) { return floor(x * 255.0 + 0.500244140625) / 255.0; }\n");
    }
    /* Preserve unclamped gradients and keep the old GLSL when mode 5 is unused. */
    char glc[720] = "";
    const unsigned gl = k->tex_modes >> 20;
    char gls[16] = "";                  /* Stage argument for direct reads. */
    if (gl) {
        snprintf(glc, sizeof(glc),
                 "const uint GLCLAMP = 0x%03xu;\n"
                 "vec2 glc2(uint s, vec2 v) {\n"
                 "  uint m = GLCLAMP >> (3u * s);\n"
                 "  return vec2((m & 1u) != 0u ? clamp(v.x, 0.0, 0.99999994) : v.x,\n"
                 "              (m & 2u) != 0u ? clamp(v.y, 0.0, 0.99999994) : v.y);\n"
                 "}\n"
                 "vec3 glc3(uint s, vec3 v) {\n"
                 "  return vec3(glc2(s, v.xy), ((GLCLAMP >> (3u * s)) & 4u) != 0u ? clamp(v.z, 0.0, 0.99999994) : v.z);\n"
                 "}\n"
                 "#define GLC2(s, v) glc2(s, v)\n"
                 "#define GLC3(s, v) glc3(s, v)\n"
                 "#define GLT2(t, s, v) (((GLCLAMP >> (3u * (s))) & 3u) != 0u ? "
                 "textureGrad(t, glc2(s, v), dFdx(v), dFdy(v)) : texture(t, v))\n"
                 "#define GLT3(t, s, v) (((GLCLAMP >> (3u * (s))) & 7u) != 0u ? "
                 "textureGrad(t, glc3(s, v), dFdx(v), dFdy(v)) : texture(t, v))\n", k->tex_modes >> 20);
    }
    put(&b, "layout(set = 0, binding = 0) uniform texture2D uTex2D[];\n"
            "layout(set = 0, binding = 1) uniform sampler uSmp[];\n"
            "layout(set = 0, binding = 2) uniform textureCube uTexCube[];\n"
            "layout(set = 0, binding = 3) uniform texture3D uTex3D[];\n"
            "layout(location = 0) %sin vec4 vDiffuse;\n"
            "layout(location = 1) %sin vec4 vSpecular;\n"
            "layout(location = 2) in vec4 vTex0;\n"
            "layout(location = 3) in vec4 vTex1;\n"
            "layout(location = 4) in vec4 vTex2;\n"
            "layout(location = 5) in vec4 vTex3i;\n"
            "layout(location = 6) in float vFog;\n"
            "%s%s%s%s"
            "layout(location = 0) out vec4 oColor;\n"
            "vec4 col(uint c) { return unpackUnorm4x8(c).zyxw; }\n"
            HLE_NVPROJ_GLSL
            "%s%s"
            /* Xbox linear (non-swizzled) textures use texel coordinates */
            "vec4 samp(uint s, vec2 uv) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0xFFu;\n"
            "%s"
            "  if ((pc.flags & (1u << s)) != 0u)\n"
            "    uv /= vec2(textureSize(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[0]), 0)) /\n"
            "          ((pc.flags & (256u << s)) != 0u ? float(pc.flags >> 24) : 1.0);\n"
            "  return %s;\n"
            "}\n"
            "vec4 samp3(uint s, vec3 uvw) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0xFFu;\n"
            "  return %s;\n"
            "}\n"
            "vec4 sampc(uint s, vec3 dir) {\n"
            "  uint ti = pc.tex[s] & 0xFFFFu, si = (pc.tex[s] >> 16) & 0xFFu;\n"
            "  return texture(samplerCube(uTexCube[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), "
            "dir);\n"
            "}\n"
            /* PSDotMapping of a texture color to a vector */
            "vec3 dmap(uint m, vec4 c) {\n"
            "  if (m == 0u) return c.rgb;\n"
            "%s"
            "  if (m == 3u) { vec3 v = floor(c.rgb * 255.0 + 0.5);\n"
            "    return mix(v, v - 256.0, greaterThanEqual(v, vec3(128.0))) / 127.0; }\n"
            "  if (m >= 4u) { vec2 h = c.rg * 2.0 - 1.0;\n"
            "    return vec3(h, m == 4u ? 1.0 : sqrt(max(1.0 - dot(h, h), 0.0))); }\n"
            "  return c.rgb * 2.0 - 1.0;\n"
            "}\n"
            /* bytes as two's complement over 127 (bump offsets) */
            "vec2 sgn8(vec2 c) {\n"
            "  vec2 v = floor(c * 255.0 + 0.5);\n"
            "  return mix(v, v - 256.0, greaterThanEqual(v, vec2(128.0))) / 127.0;\n"
            "}\n"
            "%s%s%s"
            "void main() {\n"
            "%s"
            "  vec4 vTex3 = %s;\n", interp, interp, back, k->wbuf ? HLE_WBUF_IN_GLSL : "",
        (k->fbfetch & ~HLE_FB_ZEQ) || k->sblend || k->fblend ?
            "layout(input_attachment_index = 0, set = 2, binding = 0) "
            "uniform subpassInput uFb;\n" : "",
        k->fbfetch & HLE_FB_ZEQ ? HLE_ZEQ_DECL_GLSL : "",
        /* a linear texture kept to its own size (ff.h HLE_NVRECT_GLSL): the stage fog.w names */
        k->vol & HLE_PSH_RECT ? HLE_NVRECT_GLSL : "", glc,
        k->vol & HLE_PSH_RECT ?
            "  { uint r = floatBitsToUint(pc.fog.w);\n"
            "    if (r != 0u && s == ((r >> 28 & 1u) | (r >> 30 & 2u)))\n"
            "      uv = nvrect(uv, (pc.flags & (256u << s)) != 0u ? 0.5 / float(pc.flags >> 24) : 0.5); }\n"
            : "",
        gl ? "GLT2(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), s, uv)"
           : "texture(sampler2D(uTex2D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uv)",
        gl ? "GLT3(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), s, uvw)"
           : "texture(sampler3D(uTex3D[nonuniformEXT(ti)], uSmp[nonuniformEXT(si)]), uvw)",
        /* HILO_1: the texel's two 16-bit halves, unsigned, and 1 (only where a
         * stage maps so: other shaders' text, and their cached SPIR-V, stay) */
        hilo1 ? "  if (m == 4u) { uvec4 i = uvec4(c * 255.0 + 0.5);\n"
                "    return vec3(float(i.a << 8 | i.r), float(i.g << 8 | i.b), 65535.0) / 65535.0; }\n" : "",
        !k->conv ? "" : gl ? k_conv_glc_glsl : k_conv_glsl,
        /* Signed combiner texels use /127.5; texture-shader reads use /127. */
        k->sgn_ob | k->sgn_raw ?
            "float sgnob(float c) { return max((c * 255.0 - 128.0) / 127.5, -1.0); }\n"
            "float sgnraw(float c) {\n"
            "  float v = floor(c * 255.0 + 0.5);\n"
            "  return max((v >= 128.0 ? v - 256.0 : v) / 127.5, -1.0);\n"
            "}\n"
            "vec4 tsh(vec4 t, bvec4 m) { return mix(t, max(t * (255.0 / 254.0), -1.0), m); }\n" : "",
        /* colour key: the texel and the key the same at n levels a channel */
        k->ckey ? "bool ckeq(vec4 t, vec4 c, vec4 n) {\n"
                  "  return all(equal(floor(t * n + 0.5), floor(c * n + 0.5)));\n"
                  "}\n" : "",
        k->two_side ? "  vec4 v0 = gl_FrontFacing ? vDiffuse : vBack0;\n"
                      "  vec4 v1 = gl_FrontFacing ? vSpecular : vBack1;\n"
                    : "  vec4 v0 = vDiffuse, v1 = vSpecular;\n",
        k->point_sprite ? "vec4(gl_PointCoord, 1.0, 1.0)" : "vTex3i");
    if (k->poly_smooth) {
        put(&b, HLE_POLY_COVER_GLSL);
    }
    /* DOT_ZW's stage: its z and w (zrz, zrw) make the fragment's depth,
     * which replaces W-buffering's */
    const unsigned zr = hle_psh_zrep(k->tex_modes);
    if (k->wbuf && !zr) {
        put(&b, HLE_WBUF_CALC_GLSL);
    }
    /* the table modes' distance: signed, as D3D's fog modes (LTCG 5849
     * 0x5421d0: 0x2601 linear, 0x800 / 0x801 exp, never the _ABS ones);
     * Conker's menu room fogs by a program's oFog that is negative */
    const char *fd = k->fog_abs ? "abs(vFog)" : "vFog";
    switch (k->fog) {
    case 0:  put(&b, "  float fogf = 1.0;\n"); break;
    /* vertex fog: a NaN factor (D3D's linear scale / bias are -inf, NaN when
     * FOGSTART == FOGEND: Otogi's characters) is 1, unfogged, as the NV2A
     * shows them (its min / max keep the number) */
    case 1:  put(&b, "  float fogf = isnan(vFog) ? 1.0 : clamp(vFog, 0.0, 1.0);\n"); break;
    /* the table modes too: the NV2A's linear mode is the same hardware
     * (push-buffer mode decodes D3D's vertex fog as linear, p0 1 / p1 1), so
     * Otogi 2's NaN fog coordinates turned every surface the fog colour */
    case 2:  put(&b, "  float fogf = exp(-%s * pc.fog.z);\n", fd); break;
    case 3:  put(&b, "  float fd = %s * pc.fog.z;\n"
                     "  float fogf = exp(-fd * fd);\n", fd); break;
    default: put(&b, "  float fogf = (pc.fog.y - %s) / (pc.fog.y - pc.fog.x);\n", fd); break;
    }
    if (k->fog >= 2) {
        put(&b, "  fogf = isnan(fogf) ? 1.0 : clamp(fogf, 0.0, 1.0);\n");
    }
    put(&b, "  vec4 fogr = vec4(col(pc.fog_color).rgb, fogf);\n"
            "  float dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n%s%s",
        k->debug == 17 ? "  vec4 shdbg = vec4(0.0, 0.0, 0.0, 1.0);\n" : "",
        zr ? "  float zrz = 0.0, zrw = 1.0;\n" : "");
    for (unsigned s = 0; s < 4; s++) {
        unsigned m = (k->tex_modes >> (5 * s)) & 0x1F;
        /* dot-product stages: the texture coordinates dotted with the
         * stage's input texture (PSInputTexture) through PSDotMapping */
        unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
        unsigned dm = s ? (k->dot_map >> (4 * (s - 1))) & 7 : 0;
        bool dotmode = s && (m == HLE_PST_DOTPRODUCT || (m >= HLE_PST_DOT_ST &&
                                                         m <= HLE_PST_DOT_STR_CUBE) ||
                             m == HLE_PST_DOT_RFLCT_SPEC_CONST);
        char ts[64], ts2[64];
        if (dotmode) {
            put(&b, "  dot%u = dot(vTex%u.xyz, dmap(%uu, %s));\n", s, s, dm,
                tex_read(ts, sizeof(ts), k, in));
        }
        if (s == 2 && m == HLE_PST_DOT_RFLCT_DIFF) {
            /* the diffuse lookup needs stage 3's dot, computed here first */
            unsigned in3 = (k->input_tex >> 20) & 3, dm3 = (k->dot_map >> 8) & 7;
            put(&b, "  dot3 = dot(vTex3.xyz, dmap(%uu, %s));\n", dm3,
                tex_read(ts, sizeof(ts), k, in3));
        }
        if ((k->fbfetch >> s) & 1) {
            /* the target drawn into: this pixel as the earlier draws left it
             * (an 8-bit target holds blue: back to its colour) */
            put(&b, "  vec4 t%u = subpassLoad(uFb)%s;\n", s, k->out_bgra ? ".bgra" : "");
            color_key(&b, k, s, m);     /* and the target's colour sign (sgn_raw) */
            alpha_kill(&b, k, s, m);
            continue;
        }
        if (((k->shadow >> s) & 1) && (m == HLE_PST_PROJECT2D || m == HLE_PST_PROJECT3D)) {
            shadow_sample(&b, k, s, m == HLE_PST_PROJECT3D);
            alpha_kill(&b, k, s, m);
            continue;
        }
        switch (m) {
        case HLE_PST_NONE:
            /* a stage with no texture reads opaque black, as an unbound D3D
             * texture samples: Voodoo Vince draws its cutscene letterbox
             * as t0 * v0 (black diffuse, SRCALPHA blending) with nothing on
             * stage 0; with t0.a 0 the bars blended to nothing */
            put(&b, "  vec4 t%u = vec4(0.0, 0.0, 0.0, 1.0);\n", s);
            break;
        case HLE_PST_PASSTHRU:
            put(&b, "  vec4 t%u = clamp(vTex%u, 0.0, 1.0);\n", s, s);
            break;
        case HLE_PST_CLIPPLANE:
            put(&b, "  vec4 t%u = vec4(0.0);\n", s);
            for (unsigned c = 0; c < 4; c++) {
                put(&b, "  if (vTex%u.%c %s 0.0) discard;\n", s, "xyzw"[c],
                    ((k->compare >> (4 * s + c)) & 1) ? ">=" : "<");
            }
            break;
        case HLE_PST_PROJECT2D:
            if (((k->conv >> (2 * s)) & 3) && !((k->vol >> s) & 1)) {
                put(&b, "  vec4 t%u = sampk(%uu, nvproj(vTex%u).xy, %uu, %uu);\n", s, s, s,
                    (k->conv >> (2 * s)) & 3, (k->conv_mm >> (2 * s)) & 3);
                break;
            }
            put(&b, (k->vol >> s) & 1 ? "  vec4 t%u = samp3(%uu, vec3(nvproj(vTex%u).xy, 0.0));\n"
                                      : "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy);\n", s, s, s);
            break;
        case HLE_PST_PROJECT3D:
            put(&b, "  vec4 t%u = samp3(%uu, nvproj(vTex%u));\n", s, s, s);
            break;
        case HLE_PST_CUBEMAP:
            put(&b, "  vec4 t%u = sampc(%uu, vTex%u.xyz);\n", s, s, s);
            break;
        case HLE_PST_BUMPENVMAP:
        case HLE_PST_BUMPENVMAP_LUM: {
            /* offset texture: the input stage's (b, g) as signed bytes (du,
             * dv) through the bump matrix (column-major, as the NV2A takes
             * BUMPENVMAT 00, 01, 10, 11) move this stage's coordinates; LUM
             * scales the RGB by LSCALE * the input's r + LOFFSET */
            unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
            if (!s) {
                unsup |= 1u << m;
                put(&b, "  vec4 t0 = samp(0u, nvproj(vTex0).xy);\n");
                break;
            }
            static const char *const mat[4][2] = {
                { "", "" }, { "pc.bump.x", "pc.bump.y" }, { "pc.bump.z", "pc.bump.w" },
                { "pc.bump3.x", "pc.bump3.y" } };
            /* (du, dv) the input's signed bytes; a channel colour sign already
             * made signed (sign_decode) as it is, over 127 (tex_read) */
            const unsigned sd = ((k->sgn_ob | k->sgn_raw) >> (4 * in)) & 6;
            char duv[176], du[80], dv[80];
            tex_read(ts, sizeof(ts), k, in);
            if (sd & 4) {
                snprintf(du, sizeof(du), "%s.b", ts);
            } else {
                snprintf(du, sizeof(du), "sgn8(t%u.bg).x", in);
            }
            if (sd & 2) {
                snprintf(dv, sizeof(dv), "%s.g", ts);
            } else {
                snprintf(dv, sizeof(dv), "sgn8(t%u.bg).y", in);
            }
            if (sd == 6) {
                snprintf(duv, sizeof(duv), "%s.bg", ts);
            } else if (!sd) {
                snprintf(duv, sizeof(duv), "sgn8(t%u.bg)", in);
            } else {
                snprintf(duv, sizeof(duv), "vec2(%s, %s)", du, dv);
            }
            put(&b, "  vec4 t%u;\n"
                    "  { vec2 d = mat2(unpackHalf2x16(%s), unpackHalf2x16(%s)) * "
                    "%s;\n", s, mat[s][0], mat[s][1], duv);
            if ((k->vol >> s) & 1) {            /* a volume: r passes through (Conker's
                                                 * electricity, animated along r) */
                put(&b, "    t%u = samp3(%uu, vec3(nvproj(vTex%u).xy + d, vTex%u.z));\n",
                    s, s, s, s);
            } else {
                put(&b, "    t%u = samp(%uu, nvproj(vTex%u).xy + d);\n", s, s, s);
            }
            if (m == HLE_PST_BUMPENVMAP_LUM) {
                put(&b, "    vec2 l = unpackHalf2x16(pc.bumpl[%u]);\n"
                        "    t%u.rgb *= clamp(l.x * %s.r + l.y, 0.0, 1.0);\n", s - 1, s, ts);
            }
            put(&b, "  }\n");
            break;
        }
        case HLE_PST_DPNDNT_AR:
        case HLE_PST_DPNDNT_GB: {
            /* dependent read: the input stage's (a, r) or (g, b), colours in
             * 0..1, are this stage's 2D coordinates, normalised whatever the
             * texture's layout. Mercenaries' sky dome looks its fade up by
             * the scene depth's top bytes (the depth read as A8R8G8B8): at
             * its own coordinates (none written) the fade was 0 and the sky
             * black. */
            unsigned in = s == 3 ? (k->input_tex >> 20) & 3 : s == 2 ? (k->input_tex >> 16) & 1 : 0;
            if (!s) {
                unsup |= 1u << m;
                put(&b, "  vec4 t0 = samp(0u, nvproj(vTex0).xy);\n");
                break;
            }
            put(&b, "  vec4 t%u;\n"
                    "  { uint ti = pc.tex[%u] & 0xFFFFu, si = (pc.tex[%u] >> 16) & 0xFFu;\n"
                    "    t%u = %s(sampler2D(uTex2D[nonuniformEXT(ti)], "
                    "uSmp[nonuniformEXT(si)]), %s%s.%s); }\n",
                s, s, s, s, gl ? "GLT2" : "texture", gl ? (snprintf(gls, sizeof(gls), "%uu, ", s), gls) : "",
                tex_read(ts, sizeof(ts), k, in), m == HLE_PST_DPNDNT_AR ? "ar" : "gb");
            break;
        }
        case HLE_PST_DOT_STR_3D:                    /* stage 3: (dot1, dot2, dot3) */
            put(&b, "  vec4 t%u = samp3(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOTPRODUCT:
            put(&b, "  vec4 t%u = vec4(0.0);\n", s);
            break;
        case HLE_PST_DOT_ST:
            /* the previous stage's dot and this one's: (dot1, dot2) on stage
             * 2, (dot2, dot3) on stage 3 (texm3x2pad / texm3x2tex on stages 2
             * and 3). Read at (dot1, dot2) on stage 3, Splinter Cell Chaos
             * Theory's thermal vision (stage 2 DOTPRODUCT, stage 3 DOT_ST over
             * the heat image, a shimmer by a noise map) took one column of
             * the image at a height set by one dot: vertical stripes */
            put(&b, "  vec4 t%u = samp(%uu, vec2(dot%u, dot%u));\n", s, s, s - 1, s);
            break;
        case HLE_PST_DOT_STR_CUBE:                  /* stage 3: N */
            put(&b, "  vec4 t%u = sampc(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOT_RFLCT_DIFF:                /* stage 2: N */
            put(&b, "  vec4 t%u = sampc(%uu, vec3(dot1, dot2, dot3));\n", s, s);
            break;
        case HLE_PST_DOT_RFLCT_SPEC:                /* stage 3: N reflected about E */
        case HLE_PST_DOT_RFLCT_SPEC_CONST:
            /* E the eye vector: the q coordinates of stages 1-3, or (CONST)
             * SET_EYE_VECTOR's, constant across the draw (this stage's bump
             * words) */
            put(&b, "  vec4 t%u;\n  { vec3 n = vec3(dot1, dot2, dot3), e = %s;\n"
                    "    t%u = sampc(%uu, 2.0 * n * dot(n, e) / max(dot(n, n), 1e-8) - e); }\n",
                s, m == HLE_PST_DOT_RFLCT_SPEC ? "vec3(vTex1.w, vTex2.w, vTex3.w)"
                   : "vec3(uintBitsToFloat(pc.bump3), uintBitsToFloat(pc.bumpl[2]))", s, s);
            break;
        case HLE_PST_DOT_ZW:
            /* depth replace (texm3x2pad / texm3x2depth): the previous stage's
             * dot (z) over this one's (w) is the fragment's depth, after the
             * combiners (zrep). The stage's colour is 0. */
            if (s < 2) {
                unsup |= 1u << m;
                put(&b, "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy);\n", s, s, s);
                break;
            }
            put(&b, "  vec4 t%u = vec4(0.0);\n  zrz = dot%u;\n  zrw = dot%u;\n", s, s - 1, s);
            break;
        case HLE_PST_BRDF:
            /* isotropic BRDF (NVIDIA's US6664963): stages s-2 and s-1 hold the
             * eye's and the light's spherical coordinates, 16-bit unsigned
             * numbers over 65535: elevation the high half (g: V16U16's V),
             * azimuth the low (r: U), as Cxbx-Reloaded reads them. The
             * stage's volume is looked up at (eye elevation, light elevation,
             * light azimuth - eye azimuth), the last on [-1, 1] (a wrapping
             * address mode makes it the angle's). Without a volume bound,
             * the 2D texture at the first two. */
            if (s < 2) {
                unsup |= 1u << m;
                put(&b, "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy);\n", s, s, s);
                break;
            }
            tex_read(ts, sizeof(ts), k, s - 2);
            tex_read(ts2, sizeof(ts2), k, s - 1);
            if ((k->vol >> s) & 1) {
                put(&b, "  vec4 t%u = samp3(%uu, vec3(%s.g, %s.g, %s.r - %s.r));\n",
                    s, s, ts, ts2, ts2, ts);
            } else {
                put(&b, "  vec4 t%u;\n"
                        "  { uint ti = pc.tex[%u] & 0xFFFFu, si = (pc.tex[%u] >> 16) & 0xFFu;\n"
                        "    t%u = %s(sampler2D(uTex2D[nonuniformEXT(ti)], "
                        "uSmp[nonuniformEXT(si)]), %svec2(%s.g, %s.g)); }\n",
                    s, s, s, s, gl ? "GLT2" : "texture", gl ? (snprintf(gls, sizeof(gls), "%uu, ", s), gls) : "",
                    ts, ts2);
            }
            break;
        default:                        /* unknown modes */
            unsup |= 1u << m;
            put(&b, "  vec4 t%u = samp(%uu, nvproj(vTex%u).xy); /* mode %u */\n",
                s, s, s, m);
            break;
        }
        color_key(&b, k, s, m);         /* and the texel's colour sign */
        alpha_kill(&b, k, s, m);
    }
    put(&b, "  vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0.0);\n"
            "  vec4 t0s = t0, t3s = t3;\n");      /* as sampled (debug views 15, 16) */
    unsigned n = k->count & 0xFF;
    for (unsigned s = 0; s < n && s < 8; s++) {
        stage(&b, k, s);
    }
    final_combiner(&b, k);
    put(&b, "  outc = clamp(outc, 0.0, 1.0);\n");
    if (k->poly_smooth) {
        /* Apply polygon coverage before alpha testing. Only interior pixels
         * are shaded. */
        put(&b, "  outc.a *= pcov;\n");
    }
    if (k->alpha_test && k->alpha_func < 8) {
        put(&b, "  { float a = outc.a, r = pc.alpha_ref; if (!(%s)) discard; }\n",
            k_cmp[k->alpha_func]);
    }
    if (zr) {
        /* z / w is in depth-buffer units, as a program's screen z is (0 ..
         * 2^24 - 1 for D24: nxdk_pgraph_tests' DotZW makes 10000 and more),
         * and is normalized the same way (HLE_ZNORM_GLSL, pc.zscale: the
         * integer formats' 1 / (2^n - 1), a float buffer's 1). w 0 is the
         * far plane, as D3D's texm3x2depth makes it. Outside the buffer's
         * range the fragment is rejected (NV_texture_shader), or clamped
         * with ZCLAMP_EN; NaN fails. */
        put(&b, "  float zq = zrw == 0.0 ? 0.0 : zrz / zrw;\n" HLE_ZNORM_GLSL("zq", "zrep")
                "  if (zrw == 0.0) zrep = 1.0;\n%s",
            k->zclamp ? "  zrep = clamp(zrep, 0.0, 1.0);\n"
                      : "  if (!(zrep >= 0.0 && zrep <= 1.0)) discard;\n");
    }
    if (k->fbfetch & HLE_FB_ZEQ) {
        put(&b, "%s", zr ? HLE_ZEQ_TEST_GLSL(k->fbfetch, "zrep")
                         : HLE_ZEQ_TEST_GLSL(k->fbfetch, "gl_FragCoord.z"));
    }
    if (k->aa_grid) {
        /* antialiasing control: this pixel is sample si of its guest pixel
         * (row-major, by guest pixel: the resolution scale divided out). A
         * sample the mask leaves out isn't written; alpha-to-coverage writes
         * the first alpha x N samples; alpha-to-one then makes alpha 1, for
         * the blend too. The NV2A's sample order and coverage pattern are
         * assumed. */
        const unsigned xs = k->aa_grid & 3 ? k->aa_grid & 3 : 1;
        const unsigned ys = (k->aa_grid >> 2) & 3 ? (k->aa_grid >> 2) & 3 : 1;
        put(&b, "  { ivec2 sp = ivec2(gl_FragCoord.xy / float(max((pc.flags >> 16) & 255u, 1u))) %% "
                "ivec2(%u, %u);\n    uint si = uint(sp.y * %u + sp.x);\n", xs, ys, xs);
        const unsigned all = (1u << (xs * ys)) - 1, mask = (k->aa_ctl >> 4) & all;
        if (mask != all) {
            put(&b, "    if (((%uu >> si) & 1u) == 0u) discard;\n", mask);
        }
        if (k->aa_ctl & 1) {
            put(&b, "    if (float(si) + 0.5 >= outc.a * %u.0) discard;\n", xs * ys);
        }
        if (k->aa_ctl & 2) {
            put(&b, "    outc.a = 1.0;\n");
        }
        put(&b, "  }\n");
    }
    static const char *const k_dbg[17] = {
        "outc", "t0", "v0", "r0", "vec4(vec3(fogf), 1.0)", "v1", "t3", "vec4(vec3(r0.a), 1.0)",
        "t2",
        /* 16-pixel bands: stage 1 (a shadow compare), its r / 65535 */
        ("((int(gl_FragCoord.y) & 16) == 0 ? t1 : "
         "vec4(vec3(clamp(vTex1.z / vTex1.w / 65535.0, 0.0, 1.0)), 1.0))"),
        "vec4(vec3(vSpecular.a), 1.0)", "vec4(vec3(t0.a), 1.0)", "vec4(vec3(t3.a), 1.0)",
        "vec4(fract(vTex3.xy / vTex3.w), 0.0, 1.0)", "vec4(clamp(abs(vTex3.xyz) * 0.25, 0.0, 1.0), 1.0)",
        "t3s", "t0s" };
    /* 17: shadow-buffer draws show (stored depth, r, result) / range, others as is */
    const char *dbg = k->debug < 17 ? k_dbg[k->debug] :
                      k->debug == 17 && (k->shadow & 15) ? "shdbg" :
                      k->debug == 18 ? "vec4(1.0, 0.0, 1.0, 1.0)" :
                      k->debug == 19 ? "t1" : k->debug == 20 ? "r1" :
                      /* stage 2's coordinates: |str| / 4, direction, NaN / Inf, and
                       * its cube sampled along a fixed direction */
                      k->debug == 21 ? "vec4(clamp(abs(vTex2.xyz) * 0.25, 0.0, 1.0), 1.0)" :
                      k->debug == 22 ? "vec4(normalize(vTex2.xyz) * 0.5 + 0.5, 1.0)" :
                      k->debug == 23 ? "vec4(any(isnan(vTex2)) ? 1.0 : 0.0, "
                                       "any(isinf(vTex2)) ? 1.0 : 0.0, 0.0, 1.0)" :
                      k->debug == 24 ? "sampc(2u, vec3(0.3, 0.5, 0.8))" : "outc";
    put(&b, "  oColor = %s;\n%s", dbg, k->out_bgra ? "  oColor = oColor.bgra;\n" : "");
    if (k->sblend) {
        put(&b, "  { ivec4 sv = ivec4(floor(clamp(%s, 0.0, 1.0) * 255.0 + 0.5));\n"
        "    ivec4 dv = ivec4(floor(clamp(subpassLoad(uFb), 0.0, 1.0) * 255.0 + 0.5));\n"
        "    sv -= 256 * (sv >> 7); dv -= 256 * (dv >> 7);\n"
        "    ivec4 r = clamp(dv %s sv, ivec4(-128), ivec4(127));\n"
        "    oColor = vec4(r & 255) / 255.0; }\n", "oColor", k->sblend == 1 ? "+" : "-");
    }
    if (k->fblend) {
        /* the blend with the target's pixel, here so that the dither below
         * applies to its result */
        put(&b, "  { vec4 s = clamp(oColor, 0.0, 1.0), d = subpassLoad(uFb), c = col(%uu);\n"
                "    vec4 sf = %s, df = %s;\n"
                "    oColor = clamp(%s, 0.0, 1.0); }\n",
            k->fb_const, blend_factor(k->fb_src), blend_factor(k->fb_dst), blend_eq(k->fb_op));
    }
    if (k->dither) {
        /* ordered dithering (a 4 x 4 Bayer matrix, by guest pixel: the
         * target's resolution scale divided out, so a scaled target holds the
         * pattern a title samples back) into the target's 5 / 6 bits. After
         * the blend where the shader blends (fblend); else before the
         * fixed-function blend, which scales it by the blend factors (an
         * approximation) */
        put(&b, "  { const float bm[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,\n"
                "      3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);\n"
                "    ivec2 p = ivec2(gl_FragCoord.xy / float(max((pc.flags >> 16) & 255u, 1u))) & 3;\n"
                "    float dt = (bm[p.y * 4 + p.x] + 0.5) / 16.0 - 0.5;\n"
                "    oColor.rgb = clamp(oColor.rgb + dt / %s, 0.0, 1.0); }\n",
            k->dither == 1 ? "vec3(31.0, 63.0, 31.0)" : "vec3(31.0)");
    }
    /* the replaced depth replaces W-buffering's too: the fragment's, whatever
     * made it (the polygon offset with it) */
    put(&b, "%s}\n", zr ? "  gl_FragDepth = zrep;\n" : k->wbuf ? HLE_WBUF_STORE_GLSL : "");
    if (unsupported) {
        *unsupported = unsup;
    }
    return b.overflow ? 0 : b.len;
}
