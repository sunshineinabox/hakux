/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the runtime library's pure string, number and
 * formatting routines, on host buffers (tests/native/rtl tests them).
 *
 * The multibyte code page is Latin-1: a UTF-16 character above 0xFF becomes
 * '?'. UTF-16 is little-endian, as on the guest.
 */
#ifndef NATIVE_KERNEL_RTL_TEXT_H
#define NATIVE_KERNEL_RTL_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The NTSTATUS values these return. */
#define NC_RTL_SUCCESS           0x00000000u
#define NC_RTL_BUFFER_OVERFLOW   0x80000005u
#define NC_RTL_ACCESS_VIOLATION  0xC0000005u
#define NC_RTL_INVALID_PARAMETER 0xC000000Du
#define NC_RTL_BUFFER_TOO_SMALL  0xC0000023u

/* A counted string (STRING or UNICODE_STRING) with its buffer on the host:
 * lengths in bytes. */
typedef struct NcRtlStr {
    uint16_t len, max;
    uint8_t *buf;
} NcRtlStr;

/* RtlDowncaseUnicodeChar: the lowercase of what nc_rtl_upcase (rtl_case.h)
 * uppercases. */
uint16_t nc_rtl_downcase(uint16_t c);

/* A UTF-16 character in Latin-1. */
static inline uint8_t nc_rtl_latin1(uint16_t c)
{
    return c < 0x100 ? (uint8_t)c : '?';
}

/* Latin-1 to UTF-16 and back: as many characters as fit; returns them. */
size_t nc_rtl_latin1_to_utf16(uint16_t *dst, size_t dst_chars, const uint8_t *src, size_t src_bytes);
size_t nc_rtl_utf16_to_latin1(uint8_t *dst, size_t dst_bytes, const uint16_t *src, size_t src_chars);

/* The characters before a UTF-16 string's terminator. */
size_t nc_rtl_wcslen(const uint16_t *s);

/* RtlCompareUnicodeString: the first difference (upcased when nocase), else
 * len1 - len2; lengths in bytes. */
int32_t nc_rtl_compare_utf16(const uint16_t *s1, uint16_t len1, const uint16_t *s2, uint16_t len2, bool nocase);

/* RtlAppend*: n bytes of src after dst's; BUFFER_TOO_SMALL when they don't
 * fit. wide: a UTF-16 terminator follows when there's room. */
uint32_t nc_rtl_append(NcRtlStr *dst, const uint8_t *src, uint16_t n, bool wide);

/* RtlCopyString / RtlCopyUnicodeString: as much of src as fits (src NULL:
 * empty). wide: a UTF-16 terminator follows when there's room. */
void nc_rtl_copy(NcRtlStr *dst, const NcRtlStr *src, bool wide);

/* RtlCharToInteger / RtlUnicodeStringToInteger on n characters (wide: UTF-16,
 * else bytes, ending at a 0 too): blanks, a sign, then digits of base (0:
 * 10, or 2, 8, 16 for a 0b, 0o, 0x prefix). INVALID_PARAMETER for another
 * base. */
uint32_t nc_rtl_text_to_ulong(const void *s, size_t n, bool wide, uint32_t base, uint32_t *value);

/* The digits of value in base (0: 10; else 2, 8, 10 or 16), uppercase, into
 * out[33] with a terminator; 0 for another base, else how many. */
size_t nc_rtl_ulong_digits(uint32_t value, uint32_t base, char out[33]);

/* RtlIntegerToChar: the digits into str (NULL: ACCESS_VIOLATION), with a
 * terminator if length leaves room; BUFFER_OVERFLOW if they're longer. */
uint32_t nc_rtl_integer_to_char(uint32_t value, uint32_t base, uint32_t length, char *str);

/* RtlIntegerToUnicodeString: the digits into dst with a terminator;
 * dst->len is set even when BUFFER_OVERFLOW (no room for both). */
uint32_t nc_rtl_integer_to_unicode(uint32_t value, uint32_t base, NcRtlStr *dst);

/* RtlMapGenericMask: GENERIC_READ, _WRITE, _EXECUTE and _ALL replaced by
 * map's four masks. */
uint32_t nc_rtl_map_generic(uint32_t mask, const uint32_t map[4]);

/* RtlExtendedIntegerMultiply, RtlExtendedLargeIntegerDivide (unsigned; a
 * divisor of 0 gives 0 and remainder 0), RtlExtendedMagicDivide. */
int64_t nc_rtl_ext_multiply(int64_t a, int32_t b);
uint64_t nc_rtl_ext_divide(uint64_t dividend, uint32_t divisor, uint32_t *remainder);
int64_t nc_rtl_magic_divide(int64_t dividend, uint64_t magic, int shift);

/* Where the formatter gets its arguments: the next 32-bit argument word, and
 * the host address of a 32-bit pointer (NULL for 0). */
typedef struct NcRtlFmtIo {
    void *ctx;
    uint32_t (*arg)(void *ctx);
    void *(*mem)(void *ctx, uint32_t va);
} NcRtlFmtIo;

/* The C runtime's _vsnprintf (RtlVsnprintf; RtlVsprintf with cap SIZE_MAX):
 * fmt formatted into out. Returns the length, with a terminator if there's
 * room; when the text is longer than cap, cap bytes of it and -1. */
int nc_rtl_vsnprintf(char *out, size_t cap, const char *fmt, const NcRtlFmtIo *io);

#ifdef __cplusplus
}
#endif

#endif
