/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the runtime library's strings, numbers and formatted
 * output (the pure parts are in rtl_text.c).
 *
 * STRING / UNICODE_STRING: { USHORT Length, USHORT MaximumLength, Buffer },
 * lengths in bytes. LARGE_INTEGER arguments are two words, low first.
 */
#include "native/kernel/kernel.h"
#include "native/kernel/rtl_text.h"

#include <string.h>

/* A guest counted string, its buffer at its host address. */
static NcRtlStr str_get(NcKernel *k, uint32_t s)
{
    NcRtlStr r;
    r.len = *(uint16_t *)nc_vm_host(k->vm, s);
    r.max = *(uint16_t *)nc_vm_host(k->vm, s + 2);
    r.buf = nc_vm_host(k->vm, nc_vm_rd32(k->vm, s + 4));
    return r;
}

static void str_set_len(NcKernel *k, uint32_t s, uint16_t len)
{
    *(uint16_t *)nc_vm_host(k->vm, s) = len;
}

static void str_set(NcKernel *k, uint32_t s, uint16_t len, uint16_t max, uint32_t buf)
{
    *(uint16_t *)nc_vm_host(k->vm, s) = len;
    *(uint16_t *)nc_vm_host(k->vm, s + 2) = max;
    nc_vm_wr32(k->vm, s + 4, buf);
}

static uint64_t qword(const uint32_t *a)
{
    return (uint64_t)a[0] | (uint64_t)a[1] << 32;
}

/* ------------------------------------------------------------ strings */

/* RtlAppendStringToString(Destination, Source) -> NTSTATUS */
static uint64_t RtlAppendStringToString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcRtlStr d = str_get(k, a[0]);
    const NcRtlStr s = str_get(k, a[1]);
    const uint32_t st = nc_rtl_append(&d, s.buf, s.len, false);
    str_set_len(k, a[0], d.len);
    return st;
}

/* RtlAppendUnicodeStringToString(Destination, Source) -> NTSTATUS */
static uint64_t RtlAppendUnicodeStringToString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcRtlStr d = str_get(k, a[0]);
    const NcRtlStr s = str_get(k, a[1]);
    const uint32_t st = nc_rtl_append(&d, s.buf, s.len, true);
    str_set_len(k, a[0], d.len);
    return st;
}

/* RtlAppendUnicodeToString(Destination, Source): Source a terminated UTF-16 string or NULL */
static uint64_t RtlAppendUnicodeToString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (!a[1]) {
        return NC_STATUS_SUCCESS;
    }
    const uint16_t *src = nc_vm_host(k->vm, a[1]);
    const size_t n = nc_rtl_wcslen(src) * 2;
    NcRtlStr d = str_get(k, a[0]);
    const uint32_t st = nc_rtl_append(&d, (const uint8_t *)src, n < 0xFFFE ? (uint16_t)n : 0xFFFE, true);
    str_set_len(k, a[0], d.len);
    return st;
}

/* RtlCompareUnicodeString(String1, String2, CaseInSensitive) -> LONG */
static uint64_t RtlCompareUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NcRtlStr s1 = str_get(k, a[0]), s2 = str_get(k, a[1]);
    return (uint32_t)nc_rtl_compare_utf16((const uint16_t *)s1.buf, s1.len, (const uint16_t *)s2.buf, s2.len,
                                          a[2] & 0xff);
}

/* RtlEqualUnicodeString(String1, String2, CaseInSensitive) -> BOOLEAN */
static uint64_t RtlEqualUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NcRtlStr s1 = str_get(k, a[0]), s2 = str_get(k, a[1]);
    return s1.len == s2.len &&
           nc_rtl_compare_utf16((const uint16_t *)s1.buf, s1.len, (const uint16_t *)s2.buf, s2.len, a[2] & 0xff) == 0;
}

/* As much of the string a[1] as fits into a[0]; none: empty. */
static uint64_t copy_string(NcKernel *k, const uint32_t *a, bool wide)
{
    NcRtlStr d = str_get(k, a[0]);
    if (a[1]) {
        const NcRtlStr s = str_get(k, a[1]);
        nc_rtl_copy(&d, &s, wide);
    } else {
        nc_rtl_copy(&d, NULL, wide);
    }
    str_set_len(k, a[0], d.len);
    return 0;
}

/* RtlCopyString(DestinationString, SourceString) */
static uint64_t RtlCopyString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return copy_string(k, a, false);
}

/* RtlCopyUnicodeString(DestinationString, SourceString) */
static uint64_t RtlCopyUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return copy_string(k, a, true);
}

/* RtlCreateUnicodeString(DestinationString, SourceString) -> BOOLEAN: a pool copy
 * of the terminated UTF-16 string, freed by RtlFreeUnicodeString */
static uint64_t RtlCreateUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint16_t *src = nc_vm_host(k->vm, a[1]);
    const size_t size = (nc_rtl_wcslen(src) + 1) * 2;
    if (size > 0xFFFF) {
        return 0;
    }
    const uint32_t buf = nc_rtl_str_alloc(k, (uint32_t)size);
    if (!buf) {
        return 0;
    }
    memcpy(nc_vm_host(k->vm, buf), src, size);
    str_set(k, a[0], (uint16_t)(size - 2), (uint16_t)size, buf);
    return 1;
}

/* RtlDowncaseUnicodeChar(SourceCharacter) -> WCHAR */
static uint64_t RtlDowncaseUnicodeChar(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_rtl_downcase((uint16_t)a[0]);
}

/* RtlDowncaseUnicodeString(DestinationString, SourceString, AllocateDestinationString) -> NTSTATUS */
static uint64_t RtlDowncaseUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NcRtlStr s = str_get(k, a[1]);
    const uint16_t n = s.len & ~1u;
    uint32_t buf = nc_vm_rd32(k->vm, a[0] + 4);
    uint16_t max = *(uint16_t *)nc_vm_host(k->vm, a[0] + 2);
    if (a[2] & 0xff) {
        max = n;
        buf = nc_rtl_str_alloc(k, max);
        if (!buf) {
            return NC_STATUS_NO_MEMORY;
        }
    } else if (max < n) {
        return NC_RTL_BUFFER_OVERFLOW;
    }
    const uint16_t *src = (const uint16_t *)s.buf;
    uint16_t *dst = nc_vm_host(k->vm, buf);
    for (uint16_t i = 0; i < n / 2; i++) {
        dst[i] = nc_rtl_downcase(src[i]);
    }
    str_set(k, a[0], n, max, buf);
    return NC_STATUS_SUCCESS;
}

/* RtlMultiByteToUnicodeN(UnicodeString, MaxBytesInUnicodeString, BytesInUnicodeString,
 * MultiByteString, BytesInMultiByteString) -> NTSTATUS: Latin-1 */
static uint64_t RtlMultiByteToUnicodeN(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const size_t n = nc_rtl_latin1_to_utf16(nc_vm_host(k->vm, a[0]), a[1] / 2, nc_vm_host(k->vm, a[3]), a[4]);
    if (a[2]) {
        nc_vm_wr32(k->vm, a[2], (uint32_t)n * 2);
    }
    return NC_STATUS_SUCCESS;
}

/* RtlMultiByteToUnicodeSize(BytesInUnicodeString, MultiByteString, BytesInMultiByteString) -> NTSTATUS */
static uint64_t RtlMultiByteToUnicodeSize(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_vm_wr32(k->vm, a[0], a[2] * 2);
    return NC_STATUS_SUCCESS;
}

/* RtlUnicodeToMultiByteN(MultiByteString, MaxBytesInMultiByteString, BytesInMultiByteString,
 * UnicodeString, BytesInUnicodeString) -> NTSTATUS: Latin-1, '?' for what isn't in it */
static uint64_t RtlUnicodeToMultiByteN(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const size_t n = nc_rtl_utf16_to_latin1(nc_vm_host(k->vm, a[0]), a[1], nc_vm_host(k->vm, a[3]), a[4] / 2);
    if (a[2]) {
        nc_vm_wr32(k->vm, a[2], (uint32_t)n);
    }
    return NC_STATUS_SUCCESS;
}

/* RtlUnicodeToMultiByteSize(BytesInMultiByteString, UnicodeString, BytesInUnicodeString) -> NTSTATUS */
static uint64_t RtlUnicodeToMultiByteSize(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_vm_wr32(k->vm, a[0], a[2] / 2);
    return NC_STATUS_SUCCESS;
}

/* ------------------------------------------------------------ numbers */

/* RtlCharToInteger(String, Base, Value) -> NTSTATUS */
static uint64_t RtlCharToInteger(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t v;
    const uint32_t st = nc_rtl_text_to_ulong(nc_vm_host(k->vm, a[0]), SIZE_MAX, false, a[1], &v);
    if (st != NC_RTL_SUCCESS) {
        return st;
    }
    if (!a[2]) {
        return NC_RTL_ACCESS_VIOLATION;
    }
    nc_vm_wr32(k->vm, a[2], v);
    return NC_STATUS_SUCCESS;
}

/* RtlUnicodeStringToInteger(String, Base, Value) -> NTSTATUS */
static uint64_t RtlUnicodeStringToInteger(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const NcRtlStr s = str_get(k, a[0]);
    uint32_t v;
    const uint32_t st = nc_rtl_text_to_ulong(s.buf, s.len / 2u, true, a[1], &v);
    if (st != NC_RTL_SUCCESS) {
        return st;
    }
    if (!a[2]) {
        return NC_RTL_ACCESS_VIOLATION;
    }
    nc_vm_wr32(k->vm, a[2], v);
    return NC_STATUS_SUCCESS;
}

/* RtlIntegerToChar(Value, Base, OutputLength, String) -> NTSTATUS */
static uint64_t RtlIntegerToChar(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return nc_rtl_integer_to_char(a[0], a[1], a[2], a[3] ? (char *)nc_vm_host(k->vm, a[3]) : NULL);
}

/* RtlIntegerToUnicodeString(Value, Base, String) -> NTSTATUS */
static uint64_t RtlIntegerToUnicodeString(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    NcRtlStr s = str_get(k, a[2]);
    const uint32_t st = nc_rtl_integer_to_unicode(a[0], a[1], &s);
    str_set_len(k, a[2], s.len);
    return st;
}

/* RtlExtendedIntegerMultiply(Multiplicand, Multiplier) -> LARGE_INTEGER */
static uint64_t RtlExtendedIntegerMultiply(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return (uint64_t)nc_rtl_ext_multiply((int64_t)qword(a), (int32_t)a[2]);
}

/* RtlExtendedLargeIntegerDivide(Dividend, Divisor, Remainder) -> LARGE_INTEGER */
static uint64_t RtlExtendedLargeIntegerDivide(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    /* TODO: a divisor of 0 gives 0, not the divide error */
    if (!a[2]) {
        static bool logged;
        if (!logged) {
            logged = true;
            nc_log("hakuX-native", "rtl: RtlExtendedLargeIntegerDivide by 0 from %08x gives 0",
                   nc_vm_rd32(k->vm, nc_frame_gpr(f, NC_ESP)));
        }
    }
    uint32_t rem;
    const uint64_t q = nc_rtl_ext_divide(qword(a), a[2], &rem);
    if (a[3]) {
        nc_vm_wr32(k->vm, a[3], rem);
    }
    return q;
}

/* RtlExtendedMagicDivide(Dividend, MagicDivisor, ShiftCount) -> LARGE_INTEGER */
static uint64_t RtlExtendedMagicDivide(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return (uint64_t)nc_rtl_magic_divide((int64_t)qword(a), qword(a + 2), (int8_t)a[4]);
}

/* RtlMapGenericMask(AccessMask, GenericMapping) */
static uint64_t RtlMapGenericMask(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t map[4];
    for (unsigned i = 0; i < 4; i++) {
        map[i] = nc_vm_rd32(k->vm, a[1] + 4 * i);
    }
    nc_vm_wr32(k->vm, a[0], nc_rtl_map_generic(nc_vm_rd32(k->vm, a[0]), map));
    return 0;
}

/* RtlUlongByteSwap(Source), fastcall */
static uint64_t RtlUlongByteSwap(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return __builtin_bswap32(a[0]);
}

/* RtlUshortByteSwap(Source), fastcall */
static uint64_t RtlUshortByteSwap(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return __builtin_bswap16((uint16_t)a[0]);
}

/* ------------------------------------------------------------ printf */

/* The formatter's arguments: words from the guest at ap, guest pointers. */
typedef struct GuestArgs {
    NcVm *vm;
    uint32_t ap;
} GuestArgs;

static uint32_t guest_arg(void *ctx)
{
    GuestArgs *g = ctx;
    const uint32_t v = nc_vm_rd32(g->vm, g->ap);
    g->ap += 4;
    return v;
}

static void *guest_mem(void *ctx, uint32_t va)
{
    const GuestArgs *g = ctx;
    return va ? nc_vm_host(g->vm, va) : NULL;
}

/* Format into the guest's buffer (at most count bytes when bounded). */
static uint64_t format(NcKernel *k, uint32_t buf, uint32_t count, bool bounded, uint32_t fmt, uint32_t ap)
{
    if (!fmt) {
        return (uint32_t)-1;
    }
    GuestArgs g = {k->vm, ap};
    const NcRtlFmtIo io = {&g, guest_arg, guest_mem};
    const size_t cap = !buf ? 0 : bounded ? count : SIZE_MAX;
    return (uint32_t)nc_rtl_vsnprintf(nc_vm_host(k->vm, buf), cap, nc_vm_host(k->vm, fmt), &io);
}

/* RtlSprintf(Buffer, Format, ...), cdecl: the arguments after Format on the stack */
static uint64_t RtlSprintf(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return format(k, a[0], 0, false, a[1], nc_frame_gpr(f, NC_ESP) + 12);
}

/* RtlSnprintf(Buffer, Count, Format, ...), cdecl */
static uint64_t RtlSnprintf(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return format(k, a[0], a[1], true, a[2], nc_frame_gpr(f, NC_ESP) + 16);
}

/* RtlVsprintf(Buffer, Format, ArgList), cdecl */
static uint64_t RtlVsprintf(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return format(k, a[0], 0, false, a[1], a[2]);
}

/* RtlVsnprintf(Buffer, Count, Format, ArgList), cdecl */
static uint64_t RtlVsnprintf(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return format(k, a[0], a[1], true, a[2], a[3]);
}

void nc_rtlstr_register(NcKernel *k)
{
    nc_kernel_impl(k, "RtlAppendStringToString", RtlAppendStringToString);
    nc_kernel_impl(k, "RtlAppendUnicodeStringToString", RtlAppendUnicodeStringToString);
    nc_kernel_impl(k, "RtlAppendUnicodeToString", RtlAppendUnicodeToString);
    nc_kernel_impl(k, "RtlCompareUnicodeString", RtlCompareUnicodeString);
    nc_kernel_impl(k, "RtlEqualUnicodeString", RtlEqualUnicodeString);
    nc_kernel_impl(k, "RtlCopyString", RtlCopyString);
    nc_kernel_impl(k, "RtlCopyUnicodeString", RtlCopyUnicodeString);
    nc_kernel_impl(k, "RtlCreateUnicodeString", RtlCreateUnicodeString);
    nc_kernel_impl(k, "RtlDowncaseUnicodeChar", RtlDowncaseUnicodeChar);
    nc_kernel_impl(k, "RtlDowncaseUnicodeString", RtlDowncaseUnicodeString);
    nc_kernel_impl(k, "RtlMultiByteToUnicodeN", RtlMultiByteToUnicodeN);
    nc_kernel_impl(k, "RtlMultiByteToUnicodeSize", RtlMultiByteToUnicodeSize);
    nc_kernel_impl(k, "RtlUnicodeToMultiByteN", RtlUnicodeToMultiByteN);
    nc_kernel_impl(k, "RtlUnicodeToMultiByteSize", RtlUnicodeToMultiByteSize);
    nc_kernel_impl(k, "RtlCharToInteger", RtlCharToInteger);
    nc_kernel_impl(k, "RtlUnicodeStringToInteger", RtlUnicodeStringToInteger);
    nc_kernel_impl(k, "RtlIntegerToChar", RtlIntegerToChar);
    nc_kernel_impl(k, "RtlIntegerToUnicodeString", RtlIntegerToUnicodeString);
    nc_kernel_impl(k, "RtlExtendedIntegerMultiply", RtlExtendedIntegerMultiply);
    nc_kernel_impl(k, "RtlExtendedLargeIntegerDivide", RtlExtendedLargeIntegerDivide);
    nc_kernel_impl(k, "RtlExtendedMagicDivide", RtlExtendedMagicDivide);
    nc_kernel_impl(k, "RtlMapGenericMask", RtlMapGenericMask);
    nc_kernel_impl(k, "RtlUlongByteSwap", RtlUlongByteSwap);
    nc_kernel_impl(k, "RtlUshortByteSwap", RtlUshortByteSwap);
    nc_kernel_impl(k, "RtlSprintf", RtlSprintf);
    nc_kernel_impl(k, "RtlSnprintf", RtlSnprintf);
    nc_kernel_impl(k, "RtlVsprintf", RtlVsprintf);
    nc_kernel_impl(k, "RtlVsnprintf", RtlVsnprintf);
}
