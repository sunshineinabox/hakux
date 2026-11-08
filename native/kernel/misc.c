/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — interlocked lists, port I/O buffers, the debugger's
 * exports and bug checks.
 *
 * The guest CPU stands still while a kernel call runs, so the interlocked
 * exports are plain reads and writes of guest memory, and their spin locks
 * are never taken. No debugger is attached: the debugger's exports log what
 * they are given and return, as the Xbox kernel does without one.
 *
 * LIST_ENTRY: Flink 0x00, Blink 0x04. SLIST_HEADER: Next 0x00, Depth 0x04
 * (USHORT), Sequence 0x06 (USHORT); an entry's Next is its first word.
 */
#include "native/kernel/kernel.h"
#include "native/dev/dev.h"

#define FLINK 0x00
#define BLINK 0x04
#define SL_NEXT  0x00
#define SL_DEPTH 0x04 /* Depth in the low half, Sequence in the high half */

static uint32_t rd(NcKernel *k, uint32_t va)
{
    return nc_vm_rd32(k->vm, va);
}

static void wr(NcKernel *k, uint32_t va, uint32_t v)
{
    nc_vm_wr32(k->vm, va, v);
}

static uint64_t rd64(NcKernel *k, uint32_t va)
{
    return rd(k, va) | (uint64_t)rd(k, va + 4) << 32;
}

static void wr64(NcKernel *k, uint32_t va, uint64_t v)
{
    wr(k, va, (uint32_t)v);
    wr(k, va + 4, (uint32_t)(v >> 32));
}

static uint32_t caller(NcKernel *k, NcFrame *f)
{
    return rd(k, nc_frame_gpr(f, NC_ESP));
}

/* ------------------------------------------------------- interlocked */

/* ExInterlockedAddLargeInteger(Addend, Increment, Lock) -> Addend's old value */
static uint64_t ExInterlockedAddLargeInteger(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint64_t old = rd64(k, a[0]);
    wr64(k, a[0], old + (a[1] | (uint64_t)a[2] << 32));
    return old;
}

/* ExInterlockedAddLargeStatistic(Addend, Increment): adds the ULONG to the LARGE_INTEGER */
static uint64_t ExInterlockedAddLargeStatistic(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    wr64(k, a[0], rd64(k, a[0]) + a[1]);
    return 0;
}

/* ExInterlockedCompareExchange64(Destination, Exchange, Comparand) -> Destination's old
 * value; *Exchange goes there when it was *Comparand */
static uint64_t ExInterlockedCompareExchange64(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint64_t old = rd64(k, a[0]);
    if (old == rd64(k, a[2])) {
        wr64(k, a[0], rd64(k, a[1]));
    }
    return old;
}

/* ExfInterlockedInsertHeadList(ListHead, ListEntry) -> the old first entry or NULL */
static uint64_t ExfInterlockedInsertHeadList(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t head = a[0], e = a[1], first = rd(k, head + FLINK);
    wr(k, e + FLINK, first);
    wr(k, e + BLINK, head);
    wr(k, first + BLINK, e);
    wr(k, head + FLINK, e);
    return first == head ? 0 : first;
}

/* ExfInterlockedInsertTailList(ListHead, ListEntry) -> the old last entry or NULL */
static uint64_t ExfInterlockedInsertTailList(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t head = a[0], e = a[1], last = rd(k, head + BLINK);
    wr(k, e + FLINK, head);
    wr(k, e + BLINK, last);
    wr(k, last + FLINK, e);
    wr(k, head + BLINK, e);
    return last == head ? 0 : last;
}

/* ExfInterlockedRemoveHeadList(ListHead) -> the first entry, unlinked, or NULL */
static uint64_t ExfInterlockedRemoveHeadList(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t head = a[0], first = rd(k, head + FLINK);
    if (first == head || !first) {
        return 0;
    }
    const uint32_t next = rd(k, first + FLINK);
    wr(k, head + FLINK, next);
    wr(k, next + BLINK, head);
    return first;
}

/* InterlockedPushEntrySList(ListHead, ListEntry) -> the old first entry or NULL;
 * Depth and Sequence go up by one */
static uint64_t InterlockedPushEntrySList(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t first = rd(k, a[0] + SL_NEXT);
    wr(k, a[1], first);
    wr(k, a[0] + SL_NEXT, a[1]);
    wr(k, a[0] + SL_DEPTH, rd(k, a[0] + SL_DEPTH) + 0x10001u);
    return first;
}

/* InterlockedPopEntrySList(ListHead) -> the first entry, unlinked, or NULL; Depth
 * goes down by one */
static uint64_t InterlockedPopEntrySList(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t first = rd(k, a[0] + SL_NEXT);
    if (first) {
        wr(k, a[0] + SL_NEXT, rd(k, first));
        wr(k, a[0] + SL_DEPTH, rd(k, a[0] + SL_DEPTH) - 1);
    }
    return first;
}

/* InterlockedFlushSList(ListHead) -> the first entry or NULL; the list is left
 * empty (Depth 0, Sequence kept) */
static uint64_t InterlockedFlushSList(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t first = rd(k, a[0] + SL_NEXT);
    if (first) {
        wr(k, a[0] + SL_NEXT, 0);
        wr(k, a[0] + SL_DEPTH, rd(k, a[0] + SL_DEPTH) & 0xFFFF0000u);
    }
    return first;
}

/* ------------------------------------------------------------- ports */

/* Count values of size bytes between Port and Buffer, through the ports
 * guest IN and OUT instructions reach (dev/ports.c), as REP INS and REP OUTS. */
static void port_buffer(NcKernel *k, const uint32_t *a, unsigned size, bool out)
{
    const uint16_t port = (uint16_t)a[0];
    for (uint32_t i = 0; i < a[2]; i++) {
        uint8_t *p = nc_vm_host(k->vm, a[1] + i * size);
        if (out) {
            nc_port_out(port, size, size == 1 ? *p : size == 2 ? nc_rd16(p) : nc_rd32(p));
            continue;
        }
        const uint32_t v = nc_port_in(port, size);
        if (size == 1) {
            *p = (uint8_t)v;
        } else if (size == 2) {
            nc_wr16(p, (uint16_t)v);
        } else {
            nc_wr32(p, v);
        }
    }
}

/* READ_PORT_BUFFER_UCHAR(Port, Buffer, Count) */
static uint64_t READ_PORT_BUFFER_UCHAR(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    port_buffer(k, a, 1, false);
    return 0;
}

/* READ_PORT_BUFFER_USHORT(Port, Buffer, Count) */
static uint64_t READ_PORT_BUFFER_USHORT(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    port_buffer(k, a, 2, false);
    return 0;
}

/* READ_PORT_BUFFER_ULONG(Port, Buffer, Count) */
static uint64_t READ_PORT_BUFFER_ULONG(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    port_buffer(k, a, 4, false);
    return 0;
}

/* WRITE_PORT_BUFFER_UCHAR(Port, Buffer, Count) */
static uint64_t WRITE_PORT_BUFFER_UCHAR(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    port_buffer(k, a, 1, true);
    return 0;
}

/* WRITE_PORT_BUFFER_USHORT(Port, Buffer, Count) */
static uint64_t WRITE_PORT_BUFFER_USHORT(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    port_buffer(k, a, 2, true);
    return 0;
}

/* WRITE_PORT_BUFFER_ULONG(Port, Buffer, Count) */
static uint64_t WRITE_PORT_BUFFER_ULONG(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    port_buffer(k, a, 4, true);
    return 0;
}

/* ---------------------------------------------------------- debugger */

/* DbgBreakPointWithStatus(Status): logged, as DbgBreakPoint */
static uint64_t DbgBreakPointWithStatus(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_log("hakuX-native-dbg", "DbgBreakPointWithStatus(%08x) from %08x", a[0], caller(k, f));
    return 0;
}

/* DbgLoadImageSymbols / DbgUnLoadImageSymbols(FileName, ImageBase, ProcessId): no debugger to tell */
static uint64_t DbgImageSymbols(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

/* DbgPrompt(Prompt, Response, MaximumResponseLength) -> 0: the prompt is logged, the
 * response is empty */
static uint64_t DbgPrompt(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char prompt[256];
    nc_log("hakuX-native-dbg", "DbgPrompt from %08x: %s", caller(k, f),
           nc_kernel_guest_str(k, a[0], prompt, sizeof(prompt)));
    if (a[1] && a[2]) {
        *(uint8_t *)nc_vm_host(k->vm, a[1]) = 0;
    }
    return 0;
}

/* RtlAssert(FailedAssertion, FileName, LineNumber, Message): logged */
static uint64_t RtlAssert(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char what[256], file[128], msg[256];
    nc_log("hakuX-native-dbg", "assertion failed from %08x: %s, %s line %u%s%s", caller(k, f),
           nc_kernel_guest_str(k, a[0], what, sizeof(what)), nc_kernel_guest_str(k, a[1], file, sizeof(file)), a[2],
           a[3] ? ": " : "", a[3] ? nc_kernel_guest_str(k, a[3], msg, sizeof(msg)) : "");
    return 0;
}

/* RtlRip(ApiName, Expression, Message): logged */
static uint64_t RtlRip(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    char api[128], expr[256], msg[256];
    nc_log("hakuX-native-dbg", "RIP from %08x: %s: %s %s", caller(k, f), nc_kernel_guest_str(k, a[0], api, sizeof(api)),
           nc_kernel_guest_str(k, a[1], expr, sizeof(expr)), nc_kernel_guest_str(k, a[2], msg, sizeof(msg)));
    return 0;
}

/* KeBugCheckEx(BugCheckCode, Parameter1, Parameter2, Parameter3, Parameter4): the guest stops */
static uint64_t KeBugCheckEx(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_kernel_request_stop(k, f, "KeBugCheckEx(%08x, %08x, %08x, %08x, %08x) from %08x", a[0], a[1], a[2], a[3],
                           a[4], caller(k, f));
    return 0;
}

/* MmDbgAllocateMemory(NumberOfBytes, Protect) -> NULL */
static uint64_t MmDbgAllocateMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    /* TODO: no debugger memory, as on a retail console */
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        nc_log("hakuX-native", "mm: MmDbgAllocateMemory(%u) from %08x: no debugger memory", a[0], caller(k, f));
    }
    return 0;
}

/* MmDbgFreeMemory(BaseAddress, NumberOfBytes) -> 0 pages freed */
static uint64_t MmDbgFreeMemory(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    /* TODO: no debugger memory, as on a retail console */
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        nc_log("hakuX-native", "mm: MmDbgFreeMemory(%08x, %u) from %08x: no debugger memory", a[0], a[1],
               caller(k, f));
    }
    return 0;
}

/* MmDbgQueryAvailablePages() -> 0 */
static uint64_t MmDbgQueryAvailablePages(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    /* TODO: no debugger memory, as on a retail console */
    static bool s_logged;
    if (!s_logged) {
        s_logged = true;
        nc_log("hakuX-native", "mm: MmDbgQueryAvailablePages from %08x: no debugger memory", caller(k, f));
    }
    return 0;
}

/* MmDbgWriteCheck(VirtualAddress, Opaque) -> VirtualAddress if it is mapped, else NULL.
 * The kernel writes any mapped page, so no PTE changes: *Opaque = 0. */
static uint64_t MmDbgWriteCheck(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (a[1]) {
        wr(k, a[1], 0);
    }
    return nc_vm_phys(k->vm, a[0]) != UINT32_MAX ? a[0] : 0;
}

/* MmDbgReleaseAddress(VirtualAddress, Opaque): MmDbgWriteCheck changed no PTE */
static uint64_t MmDbgReleaseAddress(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

void nc_misc_register(NcKernel *k)
{
    nc_kernel_impl(k, "ExInterlockedAddLargeInteger", ExInterlockedAddLargeInteger);
    nc_kernel_impl(k, "ExInterlockedAddLargeStatistic", ExInterlockedAddLargeStatistic);
    nc_kernel_impl(k, "ExInterlockedCompareExchange64", ExInterlockedCompareExchange64);
    nc_kernel_impl(k, "ExfInterlockedInsertHeadList", ExfInterlockedInsertHeadList);
    nc_kernel_impl(k, "ExfInterlockedInsertTailList", ExfInterlockedInsertTailList);
    nc_kernel_impl(k, "ExfInterlockedRemoveHeadList", ExfInterlockedRemoveHeadList);
    nc_kernel_impl(k, "InterlockedFlushSList", InterlockedFlushSList);
    nc_kernel_impl(k, "InterlockedPopEntrySList", InterlockedPopEntrySList);
    nc_kernel_impl(k, "InterlockedPushEntrySList", InterlockedPushEntrySList);
    nc_kernel_impl(k, "READ_PORT_BUFFER_UCHAR", READ_PORT_BUFFER_UCHAR);
    nc_kernel_impl(k, "READ_PORT_BUFFER_USHORT", READ_PORT_BUFFER_USHORT);
    nc_kernel_impl(k, "READ_PORT_BUFFER_ULONG", READ_PORT_BUFFER_ULONG);
    nc_kernel_impl(k, "WRITE_PORT_BUFFER_UCHAR", WRITE_PORT_BUFFER_UCHAR);
    nc_kernel_impl(k, "WRITE_PORT_BUFFER_USHORT", WRITE_PORT_BUFFER_USHORT);
    nc_kernel_impl(k, "WRITE_PORT_BUFFER_ULONG", WRITE_PORT_BUFFER_ULONG);
    nc_kernel_impl(k, "DbgBreakPointWithStatus", DbgBreakPointWithStatus);
    nc_kernel_impl(k, "DbgLoadImageSymbols", DbgImageSymbols);
    nc_kernel_impl(k, "DbgUnLoadImageSymbols", DbgImageSymbols);
    nc_kernel_impl(k, "DbgPrompt", DbgPrompt);
    nc_kernel_impl(k, "RtlAssert", RtlAssert);
    nc_kernel_impl(k, "RtlRip", RtlRip);
    nc_kernel_impl(k, "KeBugCheckEx", KeBugCheckEx);
    nc_kernel_impl(k, "MmDbgAllocateMemory", MmDbgAllocateMemory);
    nc_kernel_impl(k, "MmDbgFreeMemory", MmDbgFreeMemory);
    nc_kernel_impl(k, "MmDbgQueryAvailablePages", MmDbgQueryAvailablePages);
    nc_kernel_impl(k, "MmDbgWriteCheck", MmDbgWriteCheck);
    nc_kernel_impl(k, "MmDbgReleaseAddress", MmDbgReleaseAddress);
}
