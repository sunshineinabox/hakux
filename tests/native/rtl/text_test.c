/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of native/kernel/rtl_text.c, the runtime library's string,
 * number and formatted-output routines (RtlCharToInteger, RtlIntegerToChar,
 * the Latin-1 conversions, RtlSprintf and friends).
 *
 *   gcc -O1 -I<repo> tests/native/rtl/text_test.c native/kernel/rtl_text.c -o text_test && ./text_test
 */
#include "native/kernel/rtl_case.h"
#include "native/kernel/rtl_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_fail += !ok;
    g_pass += !!ok;
}

/* ------------------------------------------------------------ case */

static void test_case(void)
{
    static const struct {
        uint16_t c, want;
    } k[] = {
        {'A', 'a'},     {'Z', 'z'},     {'a', 'a'},     {'@', '@'},     {'[', '['},     {0xC0, 0xE0},
        {0xC9, 0xE9},   {0xD7, 0xD7},   {0xDE, 0xFE},   {0xDF, 0xDF},   {0x178, 0xFF},  {0x100, 0x101},
        {0x101, 0x101}, {0x130, 'i'},   {0x139, 0x13A}, {0x141, 0x142}, {0x152, 0x153}, {0x17D, 0x17E},
        {0x391, 0x3B1}, {0x3A3, 0x3C3}, {0x3A2, 0x3A2}, {0x410, 0x430}, {0x42F, 0x44F}, {0x401, 0x451},
        {0xFF21, 0xFF41}, {0x3042, 0x3042},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        const uint16_t got = nc_rtl_downcase(k[i].c);
        check(got == k[i].want, "downcase U+%04X -> U+%04X (got U+%04X)", k[i].c, k[i].want, got);
    }
    /* the counterpart of the upcase table, both ways */
    unsigned bad_down = 0, bad_up = 0;
    for (uint32_t c = 0; c < 0x10000; c++) {
        const uint16_t d = nc_rtl_downcase((uint16_t)c), u = nc_rtl_upcase((uint16_t)c);
        if (d != c && c != 0x130 && nc_rtl_upcase(d) != c) {
            bad_down++;
        }
        if (u != c && c != 0xB5 && c != 0x131 && c != 0x17F && c != 0x3C2 && nc_rtl_downcase(u) != c) {
            bad_up++;
        }
    }
    check(!bad_down && !bad_up, "downcase undoes upcase (%u, %u off)", bad_down, bad_up);
}

/* ------------------------------------------------------------ strings */

static void test_latin1(void)
{
    const uint8_t mb[] = {'A', 0xE9, 0xFF, 'z'};
    uint16_t w[8] = {0};
    size_t n = nc_rtl_latin1_to_utf16(w, 8, mb, 4);
    check(n == 4 && w[0] == 'A' && w[1] == 0xE9 && w[2] == 0xFF && w[3] == 'z' && !w[4], "Latin-1 to UTF-16");
    w[2] = 0x7777;
    n = nc_rtl_latin1_to_utf16(w, 2, mb, 4);
    check(n == 2 && w[2] == 0x7777, "Latin-1 to UTF-16, as much as fits");

    const uint16_t u[] = {'a', 0xE9, 0x100, 0x20AC, 0xFF};
    uint8_t out[8];
    memset(out, 0x55, sizeof(out));
    n = nc_rtl_utf16_to_latin1(out, 8, u, 5);
    check(n == 5 && !memcmp(out, "a\xE9??\xFF", 5) && out[5] == 0x55, "UTF-16 to Latin-1, '?' above 0xFF");
    n = nc_rtl_utf16_to_latin1(out, 3, u, 5);
    check(n == 3, "UTF-16 to Latin-1, as much as fits");
    check(nc_rtl_latin1(0x20AC) == '?' && nc_rtl_latin1(0xA9) == 0xA9, "Latin-1 of one character");
    const uint16_t z[] = {'a', 'b', 'c', 0};
    check(nc_rtl_wcslen(z) == 3 && nc_rtl_wcslen(z + 3) == 0, "wcslen");
}

static void test_compare(void)
{
    const uint16_t abc[] = {'a', 'b', 'c'}, ABC[] = {'A', 'B', 'C'}, abd[] = {'a', 'b', 'd'};
    const uint16_t ee[] = {0xE9}, EE[] = {0xC9};
    check(nc_rtl_compare_utf16(abc, 6, abc, 6, false) == 0, "compare equal");
    check(nc_rtl_compare_utf16(abc, 6, ABC, 6, false) > 0, "compare case-sensitive");
    check(nc_rtl_compare_utf16(abc, 6, ABC, 6, true) == 0, "compare case-insensitive");
    check(nc_rtl_compare_utf16(ee, 2, EE, 2, true) == 0, "compare case-insensitive, e acute");
    check(nc_rtl_compare_utf16(abc, 6, abd, 6, false) == 'c' - 'd', "compare: the first difference");
    check(nc_rtl_compare_utf16(abc, 4, abc, 6, false) == -2, "compare: length difference in bytes");
    /* upcased: '_' (0x5F) sorts after 'A' but before 'a' */
    const uint16_t a_[] = {'a'}, u_[] = {'_'};
    check(nc_rtl_compare_utf16(a_, 2, u_, 2, true) < 0, "compare case-insensitive upcases");
}

static void test_append_copy(void)
{
    uint8_t buf[16];
    memset(buf, 0x55, sizeof(buf));
    memcpy(buf, "ab", 2);
    NcRtlStr d = {2, 8, buf};
    check(nc_rtl_append(&d, (const uint8_t *)"cde", 3, false) == NC_RTL_SUCCESS && d.len == 5 &&
              !memcmp(buf, "abcde", 5) && buf[5] == 0x55,
          "append ANSI, no terminator");
    check(nc_rtl_append(&d, (const uint8_t *)"fghi", 4, false) == NC_RTL_BUFFER_TOO_SMALL && d.len == 5 &&
              buf[5] == 0x55,
          "append ANSI too long: BUFFER_TOO_SMALL, unchanged");
    check(nc_rtl_append(&d, (const uint8_t *)"fgh", 3, false) == NC_RTL_SUCCESS && d.len == 8, "append to full");
    check(nc_rtl_append(&d, (const uint8_t *)"", 0, false) == NC_RTL_SUCCESS && d.len == 8, "append nothing");

    const uint16_t xy[] = {'x', 'y'};
    memset(buf, 0x55, sizeof(buf));
    NcRtlStr w = {0, 8, buf};
    check(nc_rtl_append(&w, (const uint8_t *)xy, 4, true) == NC_RTL_SUCCESS && w.len == 4 && buf[4] == 0 &&
              buf[5] == 0,
          "append UTF-16 with a terminator");
    buf[8] = 0x55;
    check(nc_rtl_append(&w, (const uint8_t *)xy, 4, true) == NC_RTL_SUCCESS && w.len == 8 && buf[8] == 0x55,
          "append UTF-16, no room for the terminator");

    uint8_t src[] = "hello";
    NcRtlStr s = {5, 6, src};
    memset(buf, 0x55, sizeof(buf));
    NcRtlStr c = {0, 3, buf};
    nc_rtl_copy(&c, &s, false);
    check(c.len == 3 && !memcmp(buf, "hel", 3) && buf[3] == 0x55, "copy ANSI, as much as fits");
    c.max = 16;
    nc_rtl_copy(&c, &s, false);
    check(c.len == 5 && buf[5] == 0x55, "copy ANSI, no terminator");
    nc_rtl_copy(&c, NULL, false);
    check(c.len == 0, "copy NULL: empty");

    const uint16_t ws[] = {'a', 'b', 'c'};
    NcRtlStr wsrc = {6, 6, (uint8_t *)ws};
    memset(buf, 0x55, sizeof(buf));
    NcRtlStr wd = {0, 10, buf};
    nc_rtl_copy(&wd, &wsrc, true);
    check(wd.len == 6 && buf[6] == 0 && buf[7] == 0, "copy UTF-16 with a terminator");
    memset(buf, 0x55, sizeof(buf));
    wd.max = 6;
    nc_rtl_copy(&wd, &wsrc, true);
    check(wd.len == 6 && buf[6] == 0x55, "copy UTF-16 to a full buffer: no terminator");
}

/* ------------------------------------------------------------ numbers */

static void to_ulong(const char *s, uint32_t base, uint32_t want_st, uint32_t want)
{
    uint32_t v = 0xDEADBEEF;
    const uint32_t st = nc_rtl_text_to_ulong(s, SIZE_MAX, false, base, &v);
    check(st == want_st && (st || v == want), "RtlCharToInteger(\"%s\", %u) -> %08x %u (got %08x %u)", s, base,
          want_st, want, st, v);
}

static void wto_ulong(const char *s, size_t n, uint32_t base, uint32_t want)
{
    uint16_t w[32];
    for (size_t i = 0; i < n; i++) {
        w[i] = (uint8_t)s[i];
    }
    uint32_t v = 0xDEADBEEF;
    const uint32_t st = nc_rtl_text_to_ulong(w, n, true, base, &v);
    check(!st && v == want, "RtlUnicodeStringToInteger(\"%.*s\", %u) -> %u (got %u)", (int)n, s, base, want, v);
}

static void test_text_to_ulong(void)
{
    to_ulong("123", 10, 0, 123);
    to_ulong("123", 0, 0, 123);
    to_ulong("  \t-42", 0, 0, (uint32_t)-42);
    to_ulong("+7", 0, 0, 7);
    to_ulong("-1", 10, 0, 0xFFFFFFFF);
    to_ulong("0x1F", 0, 0, 0x1F);
    to_ulong("0X1F", 0, 0, 0);            /* only a lowercase prefix */
    to_ulong("0b101", 0, 0, 5);
    to_ulong("0o17", 0, 0, 15);
    to_ulong("-0x10", 0, 0, (uint32_t)-16);
    to_ulong("0x", 0, 0, 0);
    to_ulong("0", 0, 0, 0);
    to_ulong("0x10", 16, 0, 0);           /* a prefix only for base 0 */
    to_ulong("ff", 16, 0, 255);
    to_ulong("FFFFFFFF", 16, 0, 0xFFFFFFFF);
    to_ulong("12abc", 10, 0, 12);
    to_ulong("1012", 2, 0, 5);
    to_ulong("789", 8, 0, 7);
    to_ulong("4294967296", 10, 0, 0);     /* wraps */
    to_ulong("", 0, 0, 0);
    to_ulong("x", 10, 0, 0);
    to_ulong("12", 3, NC_RTL_INVALID_PARAMETER, 0);
    to_ulong("12", 36, NC_RTL_INVALID_PARAMETER, 0);
    wto_ulong("1234", 2, 10, 12);         /* counted: the length ends it */
    wto_ulong(" -0x1a", 6, 0, (uint32_t)-26);
    wto_ulong("0b", 2, 0, 0);
    wto_ulong("777", 3, 8, 511);
}

static void to_char(uint32_t value, uint32_t base, uint32_t length, uint32_t want_st, const char *want,
                    bool terminated)
{
    char buf[40];
    memset(buf, 0x55, sizeof(buf));
    const uint32_t st = nc_rtl_integer_to_char(value, base, length, buf);
    bool ok = st == want_st;
    if (ok && !st) {
        const size_t n = strlen(want);
        ok = !memcmp(buf, want, n) && (terminated ? buf[n] == 0 : buf[n] == 0x55);
    }
    check(ok, "RtlIntegerToChar(%u, %u, %u) -> %08x \"%s\"%s (got %08x)", value, base, length, want_st, want,
          terminated ? "" : " unterminated", st);
}

static void test_integer_to_char(void)
{
    to_char(1234, 10, 10, 0, "1234", true);
    to_char(1234, 0, 10, 0, "1234", true);
    to_char(0, 10, 10, 0, "0", true);
    to_char(255, 16, 10, 0, "FF", true);
    to_char(5, 2, 10, 0, "101", true);
    to_char(8, 8, 10, 0, "10", true);
    to_char(0xFFFFFFFF, 2, 33, 0, "11111111111111111111111111111111", true);
    to_char(0xFFFFFFFF, 10, 10, 0, "4294967295", false);   /* exactly the length: no terminator */
    to_char(1234, 10, 4, 0, "1234", false);
    to_char(1234, 10, 3, NC_RTL_BUFFER_OVERFLOW, "", false);
    to_char(1234, 7, 10, NC_RTL_INVALID_PARAMETER, "", false);
    check(nc_rtl_integer_to_char(1, 10, 4, NULL) == NC_RTL_ACCESS_VIOLATION, "RtlIntegerToChar to NULL");

    uint8_t buf[16];
    memset(buf, 0x55, sizeof(buf));
    NcRtlStr s = {0, 8, buf};
    check(nc_rtl_integer_to_unicode(123, 10, &s) == 0 && s.len == 6 && buf[0] == '1' && buf[1] == 0 &&
              buf[4] == '3' && buf[6] == 0 && buf[7] == 0,
          "RtlIntegerToUnicodeString with a terminator");
    s.max = 6;
    s.len = 0;
    check(nc_rtl_integer_to_unicode(123, 10, &s) == NC_RTL_BUFFER_OVERFLOW && s.len == 6,
          "RtlIntegerToUnicodeString without room for the terminator: BUFFER_OVERFLOW");
    s.max = 16;
    s.len = 2;
    check(nc_rtl_integer_to_unicode(123, 5, &s) == NC_RTL_INVALID_PARAMETER && s.len == 2,
          "RtlIntegerToUnicodeString bad base: INVALID_PARAMETER");
    check(nc_rtl_integer_to_unicode(0xBEEF, 16, &s) == 0 && s.len == 8 && buf[0] == 'B' && buf[6] == 'F',
          "RtlIntegerToUnicodeString base 16");
}

static void test_math(void)
{
    const uint32_t map[4] = {0x1, 0x2, 0x4, 0x1F};
    check(nc_rtl_map_generic(0x80000100u, map) == 0x101, "RtlMapGenericMask read");
    check(nc_rtl_map_generic(0x60000000u, map) == 0x6, "RtlMapGenericMask write, execute");
    check(nc_rtl_map_generic(0x10000000u, map) == 0x1F, "RtlMapGenericMask all");
    check(nc_rtl_map_generic(0x00100000u, map) == 0x00100000u, "RtlMapGenericMask none");

    check(nc_rtl_ext_multiply(-5, 3) == -15, "RtlExtendedIntegerMultiply sign");
    check(nc_rtl_ext_multiply(0x100000000ll, -1) == -0x100000000ll, "RtlExtendedIntegerMultiply 64-bit");
    check(nc_rtl_ext_multiply(10000000ll * 3600, 24) == 864000000000ll, "RtlExtendedIntegerMultiply a day");

    uint32_t rem = 99;
    check(nc_rtl_ext_divide(100, 7, &rem) == 14 && rem == 2, "RtlExtendedLargeIntegerDivide");
    check(nc_rtl_ext_divide(0xFFFFFFFFFFFFFFFFull, 2, &rem) == 0x7FFFFFFFFFFFFFFFull && rem == 1,
          "RtlExtendedLargeIntegerDivide unsigned");
    check(nc_rtl_ext_divide(0x123456789ull, 0x10000, &rem) == 0x12345 && rem == 0x6789,
          "RtlExtendedLargeIntegerDivide 64-bit");
    check(nc_rtl_ext_divide(5, 0, &rem) == 0 && rem == 0, "RtlExtendedLargeIntegerDivide by 0");

    /* 1 / 10000 as the kernel's time routines use it */
    const uint64_t magic = 0xD1B71758E219652Cull;
    const int64_t xs[] = {0, 1, 9999, 10000, 123456789, 864000000000ll, 0x7FFFFFFFFFFFFFFFll};
    unsigned bad = 0;
    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        bad += nc_rtl_magic_divide(xs[i], magic, 13) != xs[i] / 10000;
        bad += nc_rtl_magic_divide(-xs[i], magic, 13) != -xs[i] / 10000;
    }
    check(!bad, "RtlExtendedMagicDivide by 10000, both signs (%u off)", bad);
    check(nc_rtl_magic_divide(1ll << 40, 1ull << 63, 0) == 1ll << 39, "RtlExtendedMagicDivide no shift");
}

/* ------------------------------------------------------------ printf */

#define MEM_BASE 0x10000u
static uint8_t g_mem[4096];
static size_t g_mem_used;

typedef struct FakeArgs {
    const uint32_t *w;
    size_t n, i;
} FakeArgs;

static uint32_t fake_arg(void *ctx)
{
    FakeArgs *a = ctx;
    return a->i < a->n ? a->w[a->i++] : 0;
}

static void *fake_mem(void *ctx, uint32_t va)
{
    (void)ctx;
    return va ? &g_mem[va - MEM_BASE] : NULL;
}

static uint32_t mem_put(const void *p, size_t n)
{
    g_mem_used = (g_mem_used + 3) & ~(size_t)3;
    memcpy(&g_mem[g_mem_used], p, n);
    const uint32_t va = MEM_BASE + (uint32_t)g_mem_used;
    g_mem_used += n;
    return va;
}

static uint32_t str(const char *s)
{
    return mem_put(s, strlen(s) + 1);
}

/* A terminated UTF-16 string. */
static uint32_t wstr(const uint16_t *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return mem_put(s, (n + 1) * 2);
}

/* A STRING / UNICODE_STRING. */
static uint32_t counted(uint16_t len, uint16_t max, uint32_t buf)
{
    uint8_t h[8];
    memcpy(h, &len, 2);
    memcpy(h + 2, &max, 2);
    memcpy(h + 4, &buf, 4);
    return mem_put(h, 8);
}

static uint32_t dlo(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return (uint32_t)b;
}

static uint32_t dhi(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return (uint32_t)(b >> 32);
}

static double bits(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* want: the text (want_len bytes); terminated if the call should write one. */
static void fmt_check(const char *want, size_t want_len, int want_ret, size_t cap, const char *fmt,
                      const uint32_t *w, size_t n)
{
    char out[1024];
    memset(out, 0x7E, sizeof(out));
    FakeArgs a = {w, n, 0};
    const NcRtlFmtIo io = {&a, fake_arg, fake_mem};
    const int r = nc_rtl_vsnprintf(out, cap, fmt, &io);
    const bool terminated = want_ret >= 0 && (size_t)want_ret < cap;
    const bool ok = r == want_ret && !memcmp(out, want, want_len) &&
                    (terminated ? out[want_len] == 0 : out[want_len] == 0x7E) && a.i == n;
    check(ok, "printf(\"%s\") cap %zu -> \"%.*s\" %d%s (got \"%.*s\" %d, %zu of %zu args)", fmt,
          cap == SIZE_MAX ? 0 : cap, (int)want_len, want, want_ret, terminated ? "" : " unterminated",
          r < 0 ? (int)(cap < 64 ? cap : 64) : r, out, r, a.i, n);
}

#define PF(want, fmt, ...)                                                                                     \
    do {                                                                                                       \
        const uint32_t w_[] = {0, __VA_ARGS__};                                                                \
        fmt_check(want, strlen(want), (int)strlen(want), 1024, fmt, w_ + 1, sizeof(w_) / 4 - 1);               \
    } while (0)

#define PFN(want, ret, cap, fmt, ...)                                                                          \
    do {                                                                                                       \
        const uint32_t w_[] = {0, __VA_ARGS__};                                                                \
        fmt_check(want, strlen(want), ret, cap, fmt, w_ + 1, sizeof(w_) / 4 - 1);                              \
    } while (0)

static void test_printf_int(void)
{
    PF("plain text", "plain text");
    PF("42", "%d", 42);
    PF("-42", "%d", (uint32_t)-42);
    PF("-2147483648", "%i", 0x80000000u);
    PF("4294967295", "%u", 0xFFFFFFFFu);
    PF("abcdef", "%x", 0xABCDEF);
    PF("ABCDEF", "%X", 0xABCDEF);
    PF("10", "%o", 8);
    PF("010", "%#o", 8);
    PF("0", "%#o", 0);
    PF("0xff", "%#x", 255);
    PF("0XFF", "%#X", 255);
    PF("0", "%#x", 0);
    PF("   42", "%5d", 42);
    PF("42   |", "%-5d|", 42);
    PF("00042", "%05d", 42);
    PF("-0042", "%05d", (uint32_t)-42);
    PF("42   |", "%-05d|", 42);
    PF("+42", "%+d", 42);
    PF(" 42", "% d", 42);
    PF("+42", "%+ d", 42);
    PF("42", "%+u", 42);
    PF("0x002a", "%#06x", 42);
    PF("007", "%.3d", 7);
    PF("-007", "%.3d", (uint32_t)-7);
    PF("     007", "%08.3d", 7);
    PF("", "%.0d", 0);
    PF("     |", "%5.0d|", 0);
    PF("   42", "%*d", 5, 42);
    PF("42   |", "%*d|", (uint32_t)-5, 42);
    PF("007", "%.*d", 3, 7);
    PF("7", "%.*d", (uint32_t)-1, 7);
    PF("22136", "%hd", 0x12345678);
    PF("-1", "%hd", 0xFFFF);
    PF("65535", "%hu", 0xFFFFFFFFu);
    PF("2345", "%hx", 0x12345);
    PF("-1", "%ld", 0xFFFFFFFFu);
    PF("-1234567890123", "%I64d", (uint32_t)-1234567890123ll, (uint32_t)((uint64_t)-1234567890123ll >> 32));
    PF("18446744073709551615", "%I64u", 0xFFFFFFFFu, 0xFFFFFFFFu);
    PF("123456789abcdef0", "%I64x", 0x9ABCDEF0u, 0x12345678u);
    PF("-1234567890123", "%lld", (uint32_t)-1234567890123ll, (uint32_t)((uint64_t)-1234567890123ll >> 32));
    PF("  0x100000000", "%#13I64x", 0, 1);
    PF("-1", "%I32d", 0xFFFFFFFFu);
    PF("5", "%Id", 5);
    PF("aIqb", "a%Iqb");
    PF("00001234", "%p", 0x1234);
    PF("DEADBEEF", "%p", 0xDEADBEEFu);
    PF("1 2 3", "%d %d %d", 1, 2, 3);
    PF("x=-5, y=0x10", "x=%d, y=%#x", (uint32_t)-5, 16);
}

static void test_printf_text(void)
{
    const uint32_t hello = str("hello");
    const uint32_t ab = str("ab");
    static const uint16_t wide[] = {'w', 0xE9, 0x263A, 0};
    const uint32_t w = wstr(wide);
    const uint32_t ansi = counted(3, 8, str("abcdef"));
    static const uint16_t wz[] = {'U', 'n', 'i', 0x416, 0};
    const uint32_t uni = counted(8, 10, wstr(wz));
    const uint32_t nobuf = counted(3, 3, 0);

    PF("A", "%c", 'A');
    PF("  A", "%3c", 'A');
    PF("A  |", "%-3c|", 'A');
    PF("\xE9", "%C", 0xE9);
    PF("?", "%C", 0x263A);
    PF("A", "%lc", 'A');
    PF("?", "%wc", 0x3A9);
    PF("A", "%hC", 0x141);
    PF("A", "%c", 0x141);
    fmt_check("a\0b", 3, 3, 1024, "a%cb", (const uint32_t[]){0}, 1);
    PF("hello", "%s", hello);
    PF("hel", "%.3s", hello);
    PF("   hello", "%8s", hello);
    PF("hello   |", "%-8s|", hello);
    PF("hello", "%2s", hello);
    PF("000ab", "%05s", ab);
    PF("(null)", "%s", 0);
    PF("(n", "%.2s", 0);
    PF("w\xE9?", "%S", w);
    PF("w\xE9?", "%ws", w);
    PF("w\xE9?", "%ls", w);
    PF("w\xE9", "%.2S", w);
    PF("  w\xE9?", "%5S", w);
    PF("hello", "%hS", hello);
    PF("hello", "%hs", hello);
    PF("(null)", "%S", 0);
    PF("abc", "%Z", ansi);
    PF("   abc", "%6Z", ansi);
    PF("abc", "%.1Z", ansi);
    PF("Uni?", "%wZ", uni);
    PF("(null)", "%Z", 0);
    PF("(null)", "%Z", nobuf);
    PF("(null)", "%wZ", 0);
    PF("hello, w\xE9?: abc", "%s, %S: %Z", hello, w, ansi);

    /* %n */
    const uint32_t cnt = mem_put("\x55\x55\x55\x55\x55\x55", 6);
    PF("abc", "abc%n", cnt);
    int32_t v;
    memcpy(&v, &g_mem[cnt - MEM_BASE], 4);
    check(v == 3 && g_mem[cnt - MEM_BASE + 4] == 0x55, "%%n stores the count (%d)", v);
    memset(&g_mem[cnt - MEM_BASE], 0x55, 6);
    PF("hello!", "%s!%hn", hello, cnt);
    int16_t h;
    memcpy(&h, &g_mem[cnt - MEM_BASE], 2);
    check(h == 6 && g_mem[cnt - MEM_BASE + 2] == 0x55, "%%hn stores 16 bits (%d)", h);

    /* not conversions */
    PF("%", "%%");
    PF("100%", "100%%");
    PF("%", "%5%");
    PF("y", "%y");
    PF("y", "%-y");
    PF("abc", "abc%");
    PF("abc", "abc%-5");
}

static void test_printf_float(void)
{
    PF("3.140000", "%f", dlo(3.14), dhi(3.14));
    PF("2.50", "%.2f", dlo(2.5), dhi(2.5));
    PF("3", "%.0f", dlo(3.0), dhi(3.0));
    PF("3.", "%#.0f", dlo(3.0), dhi(3.0));
    PF("-0.000000", "%f", dlo(-0.0), dhi(-0.0));
    PF("1.234568e+004", "%e", dlo(12345.678), dhi(12345.678));
    PF("1.234568E+004", "%E", dlo(12345.678), dhi(12345.678));
    PF("1.000000e+100", "%e", dlo(1e100), dhi(1e100));
    PF("1.0e-300", "%.1e", dlo(1e-300), dhi(1e-300));
    PF("0.0001", "%g", dlo(0.0001), dhi(0.0001));
    PF("1e-005", "%g", dlo(1e-5), dhi(1e-5));
    PF("1E+020", "%G", dlo(1e20), dhi(1e20));
    PF("100000", "%g", dlo(100000.0), dhi(100000.0));
    PF("1e+006", "%g", dlo(1e6), dhi(1e6));
    PF("1.50000", "%#g", dlo(1.5), dhi(1.5));
    PF("    -1.500", "%10.3f", dlo(-1.5), dhi(-1.5));
    PF("-00001.500", "%010.3f", dlo(-1.5), dhi(-1.5));
    PF("-1.500    |", "%-10.3f|", dlo(-1.5), dhi(-1.5));
    PF("+2.0", "%+.1f", dlo(2.0), dhi(2.0));
    PF(" 2.0", "% .1f", dlo(2.0), dhi(2.0));
    PF("1.000000", "%lf", dlo(1.0), dhi(1.0));
    PF("1.000000", "%Lf", dlo(1.0), dhi(1.0));
    PF("1.500000 7", "%f %d", dlo(1.5), dhi(1.5), 7);

    const double inf = bits(0x7FF0000000000000ull), ninf = bits(0xFFF0000000000000ull);
    const double qnan = bits(0x7FF8000000000000ull), ind = bits(0xFFF8000000000000ull);
    const double snan = bits(0x7FF0000000000001ull);
    PF("1.#INF00", "%f", dlo(inf), dhi(inf));
    PF("-1.#INF00", "%f", dlo(ninf), dhi(ninf));
    PF("1", "%.0f", dlo(inf), dhi(inf));
    PF("1.$", "%.1f", dlo(inf), dhi(inf));
    PF("1.#J", "%.2f", dlo(inf), dhi(inf));
    PF("1.#IO", "%.3f", dlo(inf), dhi(inf));
    PF("1.#INF", "%.4f", dlo(inf), dhi(inf));
    PF("1.#INF00e+000", "%e", dlo(inf), dhi(inf));
    PF("1.#INF", "%g", dlo(inf), dhi(inf));
    PF("  1.#INF00", "%10f", dlo(inf), dhi(inf));
    PF("1.#QNAN0", "%f", dlo(qnan), dhi(qnan));
    PF("-1.#IND00", "%f", dlo(ind), dhi(ind));
    PF("1.#SNAN0", "%f", dlo(snan), dhi(snan));
}

static void test_printf_bounds(void)
{
    const uint32_t hw = str("hello world");
    PFN("hello", -1, 5, "%s", hw);              /* longer: -1, no terminator */
    PFN("hello", 5, 5, "hello");                /* exactly: the length, no terminator */
    PFN("hello", 5, 6, "hello");                /* room for the terminator */
    PFN("", -1, 0, "x");
    PFN("", 0, 0, "");
    PFN("  ", -1, 2, "%5d", 1);
    PFN("12", -1, 2, "%d", 123);
    PFN("hello world", 11, SIZE_MAX, "%s", hw); /* RtlSprintf */

    char big[1024];
    memset(big, '0', 511);
    big[511] = '1';
    big[512] = 0;
    PF(big, "%.600d", 1);                       /* precision at most 512 */
}

int main(void)
{
    test_case();
    test_latin1();
    test_compare();
    test_append_copy();
    test_text_to_ulong();
    test_integer_to_char();
    test_math();
    test_printf_int();
    test_printf_text();
    test_printf_float();
    test_printf_bounds();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
