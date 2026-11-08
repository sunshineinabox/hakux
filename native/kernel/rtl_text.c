/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the runtime library's pure string, number and
 * formatting routines (rtl_text.h).
 */
#include "native/kernel/rtl_text.h"
#include "native/kernel/rtl_case.h"

#include <stdio.h>
#include <string.h>

static uint16_t rd16(const void *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ------------------------------------------------------------ strings */

uint16_t nc_rtl_downcase(uint16_t c)
{
    if (c < 'A') {
        return c;
    }
    if (c <= 'Z') {
        return (uint16_t)(c + 32);
    }
    if (c < 0xC0) {
        return c;
    }
    if (c <= 0xDE && c != 0xD7) {
        return (uint16_t)(c + 32);          /* Latin-1 letters (not the multiplication sign) */
    }
    if (c >= 0x100 && c <= 0x17F) {        /* Latin Extended-A: case pairs */
        if (c == 0x130) {
            return 'i';                     /* I with dot above */
        }
        if (c <= 0x137 || (c >= 0x14A && c <= 0x177)) {
            return (uint16_t)(c & 1 ? c : c + 1);     /* upper even, lower odd */
        }
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
            return (uint16_t)(c & 1 ? c + 1 : c);     /* upper odd, lower even */
        }
        return c == 0x178 ? 0xFF : c;
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) {   /* Greek */
        return (uint16_t)(c + 32);
    }
    if (c >= 0x410 && c <= 0x42F) {         /* Cyrillic */
        return (uint16_t)(c + 32);
    }
    if (c >= 0x400 && c <= 0x40F) {
        return (uint16_t)(c + 80);
    }
    if (c >= 0xFF21 && c <= 0xFF3A) {       /* fullwidth A-Z */
        return (uint16_t)(c + 32);
    }
    return c;
}

size_t nc_rtl_latin1_to_utf16(uint16_t *dst, size_t dst_chars, const uint8_t *src, size_t src_bytes)
{
    const size_t n = src_bytes < dst_chars ? src_bytes : dst_chars;
    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    return n;
}

size_t nc_rtl_utf16_to_latin1(uint8_t *dst, size_t dst_bytes, const uint16_t *src, size_t src_chars)
{
    const size_t n = src_chars < dst_bytes ? src_chars : dst_bytes;
    for (size_t i = 0; i < n; i++) {
        dst[i] = nc_rtl_latin1(src[i]);
    }
    return n;
}

size_t nc_rtl_wcslen(const uint16_t *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int32_t nc_rtl_compare_utf16(const uint16_t *s1, uint16_t len1, const uint16_t *s2, uint16_t len2, bool nocase)
{
    const size_t n = (len1 < len2 ? len1 : len2) / 2u;
    for (size_t i = 0; i < n; i++) {
        const int32_t c1 = nocase ? nc_rtl_upcase(s1[i]) : s1[i];
        const int32_t c2 = nocase ? nc_rtl_upcase(s2[i]) : s2[i];
        if (c1 != c2) {
            return c1 - c2;
        }
    }
    return (int32_t)len1 - (int32_t)len2;
}

static void terminate_wide(NcRtlStr *s)
{
    if (s->len + 2u <= s->max) {
        s->buf[s->len] = 0;
        s->buf[s->len + 1] = 0;
    }
}

uint32_t nc_rtl_append(NcRtlStr *dst, const uint8_t *src, uint16_t n, bool wide)
{
    if (!n) {
        return NC_RTL_SUCCESS;
    }
    if ((uint32_t)dst->len + n > dst->max) {
        return NC_RTL_BUFFER_TOO_SMALL;
    }
    memmove(dst->buf + dst->len, src, n);
    dst->len = (uint16_t)(dst->len + n);
    if (wide) {
        terminate_wide(dst);
    }
    return NC_RTL_SUCCESS;
}

void nc_rtl_copy(NcRtlStr *dst, const NcRtlStr *src, bool wide)
{
    if (!src) {
        dst->len = 0;
        return;
    }
    const uint16_t n = src->len < dst->max ? src->len : dst->max;
    memmove(dst->buf, src->buf, n);
    dst->len = n;
    if (wide) {
        terminate_wide(dst);
    }
}

/* ------------------------------------------------------------ numbers */

static uint16_t char_at(const void *s, bool wide, size_t i)
{
    return wide ? rd16((const uint8_t *)s + 2 * i) : ((const uint8_t *)s)[i];
}

uint32_t nc_rtl_text_to_ulong(const void *s, size_t n, bool wide, uint32_t base, uint32_t *value)
{
    if (!wide) {
        size_t len = 0;
        while (len < n && ((const uint8_t *)s)[len]) {
            len++;
        }
        n = len;
    }
    size_t i = 0;
    while (i < n && char_at(s, wide, i) <= ' ') {
        i++;
    }
    bool minus = false;
    if (i < n && (char_at(s, wide, i) == '+' || char_at(s, wide, i) == '-')) {
        minus = char_at(s, wide, i) == '-';
        i++;
    }
    if (!base) {
        base = 10;
        if (i + 1 < n && char_at(s, wide, i) == '0') {
            const uint16_t p = char_at(s, wide, i + 1);
            const uint32_t b = p == 'b' ? 2 : p == 'o' ? 8 : p == 'x' ? 16 : 0;
            if (b) {
                base = b;
                i += 2;
            }
        }
    } else if (base != 2 && base != 8 && base != 10 && base != 16) {
        return NC_RTL_INVALID_PARAMETER;
    }
    uint32_t v = 0;
    for (; i < n; i++) {
        const uint16_t c = char_at(s, wide, i);
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'A' && c <= 'Z') {
            d = c - 'A' + 10u;
        } else if (c >= 'a' && c <= 'z') {
            d = c - 'a' + 10u;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
    }
    *value = minus ? 0u - v : v;
    return NC_RTL_SUCCESS;
}

size_t nc_rtl_ulong_digits(uint32_t value, uint32_t base, char out[33])
{
    if (!base) {
        base = 10;
    }
    out[0] = 0;
    if (base != 2 && base != 8 && base != 10 && base != 16) {
        return 0;
    }
    char rev[32];
    size_t n = 0;
    do {
        const uint32_t d = value % base;
        value /= base;
        rev[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    } while (value);
    for (size_t i = 0; i < n; i++) {
        out[i] = rev[n - 1 - i];
    }
    out[n] = 0;
    return n;
}

uint32_t nc_rtl_integer_to_char(uint32_t value, uint32_t base, uint32_t length, char *str)
{
    char d[33];
    const size_t n = nc_rtl_ulong_digits(value, base, d);
    if (!n) {
        return NC_RTL_INVALID_PARAMETER;
    }
    if (n > length) {
        return NC_RTL_BUFFER_OVERFLOW;
    }
    if (!str) {
        return NC_RTL_ACCESS_VIOLATION;
    }
    memcpy(str, d, n < length ? n + 1 : n);
    return NC_RTL_SUCCESS;
}

uint32_t nc_rtl_integer_to_unicode(uint32_t value, uint32_t base, NcRtlStr *dst)
{
    char d[33];
    const size_t n = nc_rtl_ulong_digits(value, base, d);
    if (!n) {
        return NC_RTL_INVALID_PARAMETER;
    }
    dst->len = (uint16_t)(n * 2);
    if (dst->len >= dst->max) {
        return NC_RTL_BUFFER_OVERFLOW;
    }
    for (size_t i = 0; i < n; i++) {
        dst->buf[2 * i] = (uint8_t)d[i];
        dst->buf[2 * i + 1] = 0;
    }
    terminate_wide(dst);
    return NC_RTL_SUCCESS;
}

static char ascii_upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

static bool name_match(const char *m, const char *s)
{
    if (!*m) {
        return !*s;
    }
    if (*m == '*') {
        const char *end = s + strlen(s);
        const char *dot = m[1] == '.' ? strrchr(s, '.') : NULL;
        if (dot) {
            end = dot;                  /* not past the name's last dot */
        }
        for (const char *p = s;; p++) {
            if (name_match(m + 1, p)) {
                return true;
            }
            if (p == end) {
                return false;
            }
        }
    }
    if (*m == '.' && !*s && (m[1] == '*' || m[1] == '?' || !m[1])) {
        return name_match(m + 1, s);    /* the end of a name without a dot */
    }
    return *s && (*m == '?' || ascii_upper(*m) == ascii_upper(*s)) && name_match(m + 1, s + 1);
}

bool nc_rtl_name_match(const char *mask, const char *name)
{
    return name_match(strcmp(mask, "*.*") ? mask : "*", name);
}

uint32_t nc_rtl_map_generic(uint32_t mask, const uint32_t map[4])
{
    for (unsigned i = 0; i < 4; i++) {
        if (mask & (0x80000000u >> i)) {   /* GENERIC_READ, _WRITE, _EXECUTE, _ALL */
            mask |= map[i];
        }
    }
    return mask & 0x0FFFFFFFu;
}

int64_t nc_rtl_ext_multiply(int64_t a, int32_t b)
{
    return (int64_t)((uint64_t)a * (uint64_t)(int64_t)b);
}

uint64_t nc_rtl_ext_divide(uint64_t dividend, uint32_t divisor, uint32_t *remainder)
{
    if (!divisor) {
        *remainder = 0;
        return 0;
    }
    *remainder = (uint32_t)(dividend % divisor);
    return dividend / divisor;
}

/* The high 64 bits of a * b. */
static uint64_t mul_high(uint64_t a, uint64_t b)
{
    const uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    const uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

int64_t nc_rtl_magic_divide(int64_t dividend, uint64_t magic, int shift)
{
    const bool neg = dividend < 0;
    const uint64_t q = mul_high(neg ? 0 - (uint64_t)dividend : (uint64_t)dividend, magic) >> (shift & 63);
    return (int64_t)(neg ? 0 - q : q);
}

/* ------------------------------------------------------------ printf */

enum {
    F_LEFT = 1 << 0,    /* - */
    F_PLUS = 1 << 1,    /* + */
    F_SPACE = 1 << 2,   /* ' ' */
    F_ALT = 1 << 3,     /* # */
    F_ZERO = 1 << 4,    /* 0 */
    F_SHORT = 1 << 5,   /* h */
    F_LONG = 1 << 6,    /* l */
    F_I64 = 1 << 7,     /* I64, ll */
    F_WIDE = 1 << 8,    /* w */
    F_SIGNED = 1 << 9,
    F_NEG = 1 << 10,
};

#define FMT_MAX_PRECISION 512

typedef struct Out {
    char *buf;
    size_t cap, pos;
    bool over;          /* the text didn't fit: the rest is dropped */
} Out;

static void put(Out *o, char c)
{
    if (o->over) {
        return;
    }
    if (o->pos < o->cap) {
        o->buf[o->pos++] = c;
    } else {
        o->over = true;
    }
}

static void put_n(Out *o, char c, int64_t n)
{
    for (; n > 0 && !o->over; n--) {
        put(o, c);
    }
}

/* A converted field: padding, prefix (a sign or 0x), zeros for the 0 flag,
 * then the text (wide: UTF-16, in Latin-1). */
static void field(Out *o, unsigned flags, int64_t width, const char *prefix, size_t plen, const void *text,
                  size_t tlen, bool wide)
{
    const int64_t pad = width - (int64_t)tlen - (int64_t)plen;
    if (!(flags & (F_LEFT | F_ZERO))) {
        put_n(o, ' ', pad);
    }
    for (size_t i = 0; i < plen; i++) {
        put(o, prefix[i]);
    }
    if ((flags & F_ZERO) && !(flags & F_LEFT)) {
        put_n(o, '0', pad);
    }
    for (size_t i = 0; i < tlen && !o->over; i++) {
        put(o, wide ? (char)nc_rtl_latin1(rd16((const uint8_t *)text + 2 * i)) : ((const char *)text)[i]);
    }
    if (flags & F_LEFT) {
        put_n(o, ' ', pad);
    }
}

static uint64_t arg64(const NcRtlFmtIo *io)
{
    const uint64_t lo = io->arg(io->ctx);
    return lo | (uint64_t)io->arg(io->ctx) << 32;
}

static void fmt_int(Out *o, char t, unsigned flags, int64_t width, int64_t prec, const NcRtlFmtIo *io)
{
    unsigned radix = 10;
    bool upper = false;
    if (t == 'd' || t == 'i') {
        flags |= F_SIGNED;
    } else if (t == 'o') {
        radix = 8;
    } else if (t == 'x' || t == 'X' || t == 'p') {
        radix = 16;
        upper = t != 'x';
        if (t == 'p') {
            prec = 8;                       /* a pointer: eight uppercase digits */
        }
    }
    uint64_t v;
    if (flags & F_I64) {
        v = arg64(io);
    } else {
        const uint32_t w = io->arg(io->ctx);
        if (flags & F_SHORT) {
            v = flags & F_SIGNED ? (uint64_t)(int64_t)(int16_t)w : (uint16_t)w;
        } else {
            v = flags & F_SIGNED ? (uint64_t)(int64_t)(int32_t)w : w;
        }
    }
    if ((flags & F_SIGNED) && (int64_t)v < 0) {
        v = 0 - v;
        flags |= F_NEG;
    }
    if (prec < 0) {
        prec = 1;
    } else {
        flags &= ~(unsigned)F_ZERO;         /* a precision turns the 0 flag off */
        prec = prec > FMT_MAX_PRECISION ? FMT_MAX_PRECISION : prec;
    }
    char prefix[2];
    size_t plen = 0;
    if (flags & F_NEG) {
        prefix[plen++] = '-';
    } else if ((flags & F_SIGNED) && (flags & F_PLUS)) {
        prefix[plen++] = '+';
    } else if ((flags & F_SIGNED) && (flags & F_SPACE)) {
        prefix[plen++] = ' ';
    } else if (radix == 16 && (flags & F_ALT) && v) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[FMT_MAX_PRECISION + 32];
    char *end = buf + sizeof(buf), *q = end;
    while (prec-- > 0 || v) {               /* precision 0 and value 0: no digits */
        *--q = digits[v % radix];
        v /= radix;
    }
    if (radix == 8 && (flags & F_ALT) && (q == end || *q != '0')) {
        *--q = '0';
    }
    field(o, flags, width, prefix, plen, q, (size_t)(end - q), false);
}

/* The C runtime's digits for an infinity or NaN (after a leading 1), shown
 * as 1.#INF and the like and rounded as digits are. */
static size_t fmt_special(char *text, char t, unsigned flags, int64_t prec, const char *tail)
{
    const size_t tl = strlen(tail);
    const bool g = t == 'g' || t == 'G';
    const int64_t fp = g ? (prec ? prec : 1) - 1 : prec;   /* digits after the point */
    size_t n = 0;
    text[n++] = '1';
    if (fp > 0 || (flags & F_ALT)) {
        text[n++] = '.';
    }
    for (int64_t i = 0; i < fp; i++) {
        text[n++] = (size_t)i < tl ? tail[i] : '0';
    }
    if (fp > 0 && (size_t)fp < tl && tail[fp] >= '5') {
        text[n - 1]++;
    }
    if (g && !(flags & F_ALT)) {
        while (text[n - 1] == '0') {
            n--;
        }
        if (text[n - 1] == '.') {
            n--;
        }
    }
    if (t == 'e' || t == 'E') {
        memcpy(text + n, t == 'e' ? "e+000" : "E+000", 5);
        n += 5;
    }
    return n;
}

/* %e %E %f %g %G: the host formats the digits (its rounding); the exponent
 * has three digits, as the C runtime's. */
static void fmt_float(Out *o, char t, unsigned flags, int64_t width, int64_t prec, uint64_t bits)
{
    prec = prec < 0 ? 6 : prec > FMT_MAX_PRECISION ? FMT_MAX_PRECISION : prec;
    const bool neg = bits >> 63;
    bits &= ~(1ull << 63);
    char text[FMT_MAX_PRECISION + 400];
    size_t n;
    if ((bits >> 52) == 0x7FF) {
        const uint64_t mant = bits & ((1ull << 52) - 1);
        const char *tail = !mant ? "#INF" : neg && mant == 1ull << 51 ? "#IND" : mant & 1ull << 51 ? "#QNAN" : "#SNAN";
        n = fmt_special(text, t, flags, prec, tail);
    } else {
        double d;
        memcpy(&d, &bits, sizeof(d));
        char spec[8];
        size_t s = 0;
        spec[s++] = '%';
        if (flags & F_ALT) {
            spec[s++] = '#';
        }
        spec[s++] = '.';
        spec[s++] = '*';
        spec[s++] = t;
        spec[s] = 0;
        const int r = snprintf(text, sizeof(text) - 2, spec, (int)prec, d);
        n = r < 0 ? 0 : (size_t)r < sizeof(text) - 2 ? (size_t)r : sizeof(text) - 3;
        char *e = memchr(text, t == 'E' || t == 'G' ? 'E' : 'e', n);
        if (e && e + 2 <= text + n) {
            const size_t nd = (size_t)(text + n - (e + 2));
            if (nd < 3) {
                memmove(e + 2 + (3 - nd), e + 2, nd);
                memset(e + 2, '0', 3 - nd);
                n += 3 - nd;
            }
        }
    }
    char prefix = neg ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;
    field(o, flags, width, &prefix, prefix ? 1 : 0, text, n, false);
}

static const uint16_t k_wnull[] = {'(', 'n', 'u', 'l', 'l', ')', 0};

int nc_rtl_vsnprintf(char *out, size_t cap, const char *fmt, const NcRtlFmtIo *io)
{
    Out o = {out, cap, 0, false};
    const char *p = fmt;
    while (*p && !o.over) {
        if (*p != '%') {
            put(&o, *p++);
            continue;
        }
        p++;
        unsigned flags = 0;
        for (;; p++) {
            const char c = *p;
            if (c == '-') {
                flags |= F_LEFT;
            } else if (c == '+') {
                flags |= F_PLUS;
            } else if (c == ' ') {
                flags |= F_SPACE;
            } else if (c == '#') {
                flags |= F_ALT;
            } else if (c == '0') {
                flags |= F_ZERO;
            } else {
                break;
            }
        }
        int64_t width = 0, prec = -1;
        if (*p == '*') {                    /* from an argument; negative: left-justified */
            const int32_t w = (int32_t)io->arg(io->ctx);
            if (w < 0) {
                flags |= F_LEFT;
            }
            width = w < 0 ? -(int64_t)w : w;
            p++;
        }
        for (; *p >= '0' && *p <= '9'; p++) {
            width = width * 10 + (*p - '0');
            width = width > 0x7FFFFFFF ? 0x7FFFFFFF : width;
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {                /* from an argument; negative: as if none */
                const int32_t v = (int32_t)io->arg(io->ctx);
                prec = v < 0 ? -1 : v;
                p++;
            }
            for (; *p >= '0' && *p <= '9'; p++) {
                prec = prec * 10 + (*p - '0');
                prec = prec > 0x7FFFFFFF ? 0x7FFFFFFF : prec;
            }
        }
        for (;; p++) {
            const char c = *p;
            if (c == 'h') {
                flags |= F_SHORT;
            } else if (c == 'l') {
                if (p[1] == 'l') {
                    flags |= F_I64;
                    p++;
                } else {
                    flags |= F_LONG;
                }
            } else if (c == 'w') {
                flags |= F_WIDE;
            } else if (c == 'L' || c == 'N' || c == 'F') {
                /* long double (a double), near and far pointers: no change */
            } else if (c == 'I') {
                if (p[1] == '6' && p[2] == '4') {
                    flags |= F_I64;
                    p += 2;
                } else if (p[1] == '3' && p[2] == '2') {
                    flags &= ~(unsigned)F_I64;
                    p += 2;
                } else if (!p[1] || !strchr("diouxX", p[1])) {
                    break;                  /* not a size: the I itself is printed */
                }
            } else {
                break;
            }
        }
        const char t = *p;
        if (!t) {
            break;                          /* the format ends inside a conversion */
        }
        p++;
        switch (t) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
            fmt_int(&o, t, flags, width, prec, io);
            break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            fmt_float(&o, t, flags, width, prec, arg64(io));
            break;
        case 'C':
            if (!(flags & (F_SHORT | F_LONG | F_WIDE))) {
                flags |= F_WIDE;
            }
            /* fall through */
        case 'c': {
            const uint32_t w = io->arg(io->ctx);
            const char c = (flags & (F_LONG | F_WIDE)) ? (char)nc_rtl_latin1((uint16_t)w) : (char)w;
            field(&o, flags, width, NULL, 0, &c, 1, false);
            break;
        }
        case 'S':
            if (!(flags & (F_SHORT | F_LONG | F_WIDE))) {
                flags |= F_WIDE;
            }
            /* fall through */
        case 's': {
            const bool wide = flags & (F_LONG | F_WIDE);
            const void *s = io->mem(io->ctx, io->arg(io->ctx));
            if (!s) {
                s = wide ? (const void *)k_wnull : (const void *)"(null)";
            }
            const size_t max = prec < 0 ? SIZE_MAX : (size_t)prec;
            size_t n = 0;
            while (n < max && (wide ? rd16((const uint8_t *)s + 2 * n) : ((const char *)s)[n])) {
                n++;
            }
            field(&o, flags, width, NULL, 0, s, n, wide);
            break;
        }
        case 'Z': {                         /* a STRING, or with w a UNICODE_STRING */
            const uint8_t *str = io->mem(io->ctx, io->arg(io->ctx));
            const void *buf = str ? io->mem(io->ctx, rd32(str + 4)) : NULL;
            if (!buf) {
                field(&o, flags, width, NULL, 0, "(null)", 6, false);
            } else if (flags & F_WIDE) {
                field(&o, flags, width, NULL, 0, buf, rd16(str) / 2u, true);
            } else {
                field(&o, flags, width, NULL, 0, buf, rd16(str), false);
            }
            break;
        }
        case 'n': {                         /* the count so far, stored */
            uint8_t *dst = io->mem(io->ctx, io->arg(io->ctx));
            const int32_t v = o.over ? -1 : (int32_t)o.pos;
            if (dst) {
                if (flags & F_SHORT) {
                    const int16_t h = (int16_t)v;
                    memcpy(dst, &h, 2);
                } else {
                    memcpy(dst, &v, 4);
                }
            }
            break;
        }
        default:                            /* not a conversion: the character itself */
            put(&o, t);
            break;
        }
    }
    if (o.over) {
        return -1;
    }
    if (o.pos < o.cap) {
        o.buf[o.pos] = 0;
    }
    return (int)o.pos;
}
