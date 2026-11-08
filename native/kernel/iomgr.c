/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — IRPs, drivers, device queues and I/O completion ports.
 *
 * Xbox layouts (nxdk xboxkrnl.h; the stack location's parameters as title
 * drivers read them):
 *   IRP (0x68, its stack locations after it): Type 6, Size 0x02, Flags 0x04,
 *     ThreadListEntry 0x08, IoStatus 0x10, StackCount 0x18, CurrentLocation
 *     0x19, PendingReturned 0x1A, Cancel 0x1B, UserIosb 0x1C, UserEvent 0x20,
 *     UserApcRoutine 0x28, UserApcContext 0x2C, UserBuffer 0x30, Tail at 0x3C:
 *     DeviceQueueEntry / CompletionKey 0x3C, Thread 0x50, ListEntry 0x54,
 *     CurrentStackLocation / PacketType 0x5C, OriginalFileObject 0x60.
 *   IO_STACK_LOCATION (0x24): MajorFunction, MinorFunction, Flags, Control,
 *     Parameters 0x04 (Read/Write { Length, BufferOffset, ByteOffset 0x0C };
 *     DeviceIoControl { OutputBufferLength, InputBuffer, InputBufferLength,
 *     IoControlCode 0x10 }), DeviceObject 0x14, FileObject 0x18,
 *     CompletionRoutine 0x1C, Context 0x20.
 *   DEVICE_OBJECT: DriverObject 0x08, CurrentIrp 0x10, StartIoFlags 0x1D,
 *     StackSize 0x1E, DeviceQueue 0x28, StartIoKey 0x44.
 *   DRIVER_OBJECT: DriverStartIo 0x00, MajorFunction[14] 0x0C.
 *   KDEVICE_QUEUE (0x0C): Type, Size, Busy 0x03, DeviceListHead 0x04;
 *     KDEVICE_QUEUE_ENTRY: DeviceListEntry, SortKey 0x08, Inserted 0x0C.
 *
 * A driver's dispatch routines, StartIo and completion routines are guest
 * code: they run as callouts, the kernel call going on after them in a
 * continuation (kernel.h). The kernel's own devices (the disc, the hard
 * disk's partitions: no driver) do an IRP in the call (nc_io_device_irp).
 * What IopCompleteRequest does in a kernel APC to the IRP's thread (the
 * IoStatusBlock, the event, the caller's APC or completion port, the IRP
 * freed) is done in the call that completes the IRP, as this kernel's own
 * I/O completes.
 */
#include "native/kernel/kernel.h"

#define IRP_BASE         0x68u
#define SL_SIZE          0x24u

#define IRP_TYPE         0x00
#define IRP_SIZE         0x02
#define IRP_FLAGS        0x04
#define IRP_THREAD_LIST  0x08
#define IRP_STATUS       0x10
#define IRP_INFORMATION  0x14
#define IRP_STACK_COUNT  0x18
#define IRP_LOCATION     0x19
#define IRP_PENDING      0x1A
#define IRP_CANCEL       0x1B
#define IRP_USER_IOSB    0x1C
#define IRP_USER_EVENT   0x20
#define IRP_APC_ROUTINE  0x28
#define IRP_APC_CONTEXT  0x2C
#define IRP_USER_BUFFER  0x30
#define IRP_QUEUE_ENTRY  0x3C
#define IRP_KEY          0x3C
#define IRP_THREAD       0x50
#define IRP_LIST_ENTRY   0x54
#define IRP_STACK        0x5C
#define IRP_PACKET_TYPE  0x5C
#define IRP_FILE         0x60

#define IRP_PAGING_IO    0x02u
#define IRP_CLOSE        0x40u
#define IRP_DEFER        0x80u

#define SL_MAJOR         0x00
#define SL_MINOR         0x01
#define SL_FLAGS         0x02
#define SL_CONTROL       0x03
#define SL_LENGTH        0x04
#define SL_OFFSET        0x0C
#define SL_OUT_LENGTH    0x04
#define SL_IN_BUFFER     0x08
#define SL_IN_LENGTH     0x0C
#define SL_CODE          0x10
#define SL_DEVICE        0x14
#define SL_ROUTINE       0x1C
#define SL_CONTEXT       0x20
#define SL_PENDING       0x01u
#define SL_ON_CANCEL     0x20u
#define SL_ON_SUCCESS    0x40u
#define SL_ON_ERROR      0x80u

#define DEV_DRIVER       0x08
#define DEV_CURRENT_IRP  0x10
#define DEV_STARTIO      0x1D
#define DEV_STACK_SIZE   0x1E
#define DEV_QUEUE        0x28
#define DEV_STARTIO_KEY  0x44
#define STARTIO_INSIDE   0x01u
#define STARTIO_DEFERRED 0x02u
#define STARTIO_BY_KEY   0x04u

#define DRV_STARTIO      0x00
#define DRV_MAJOR        0x0C

#define DQ_BUSY          0x03
#define DQ_HEAD          0x04
#define DQE_KEY          0x08
#define DQE_INSERTED     0x0C

#define ET_IRP_LIST      0x134  /* ETHREAD.IrpList */
#define FO_FINAL_STATUS  0x10
#define FO_COMPLETION    0x20
#define FO_EVENT         0x38

/* A completion port's packet from NtSetIoCompletion / an I/O without an IRP:
 * ListEntry, PacketType (1) 0x08, Key 0x0C, ApcContext 0x10, Status 0x14,
 * Information 0x18. A queued IRP has PacketType 0 at the same place. */
#define PACKET_SIZE      0x1Cu
#define PK_TYPE          0x08
#define PK_KEY           0x0C
#define PK_APC_CONTEXT   0x10
#define PK_STATUS        0x14
#define PK_INFORMATION   0x18

#define STATUS_INVALID_DEVICE_REQUEST   0xC0000010u
#define STATUS_MORE_PROCESSING_REQUIRED 0xC0000016u
#define STATUS_USER_APC                 0x000000C0u
#define IO_DISK_INCREMENT    1  /* the kernel's own devices' completion boost (the disc's: as much) */
#define IRP_MJ_FLUSH_BUFFERS 0x06
#define IRP_MJ_SHUTDOWN      0x0C

static uint8_t rd8(NcKernel *k, uint32_t va)
{
    return *(const uint8_t *)nc_vm_host(k->vm, va);
}

static void wr8(NcKernel *k, uint32_t va, uint8_t v)
{
    *(uint8_t *)nc_vm_host(k->vm, va) = v;
}

static uint32_t rd32(NcKernel *k, uint32_t va)
{
    return nc_vm_rd32(k->vm, va);
}

static void wr32(NcKernel *k, uint32_t va, uint32_t v)
{
    nc_vm_wr32(k->vm, va, v);
}

static bool nt_success(uint32_t status)
{
    return (int32_t)status >= 0;
}

/* ------------------------------------------------------------- lists */

static void list_init(NcKernel *k, uint32_t head)
{
    wr32(k, head, head);
    wr32(k, head + 4, head);
}

static bool list_empty(NcKernel *k, uint32_t head)
{
    return rd32(k, head) == head;
}

static void list_insert_before(NcKernel *k, uint32_t next, uint32_t entry)
{
    const uint32_t prev = rd32(k, next + 4);
    wr32(k, entry, next);
    wr32(k, entry + 4, prev);
    wr32(k, prev, entry);
    wr32(k, next + 4, entry);
}

static void list_remove(NcKernel *k, uint32_t entry)
{
    const uint32_t next = rd32(k, entry), prev = rd32(k, entry + 4);
    wr32(k, prev, next);
    wr32(k, next + 4, prev);
}

/* ------------------------------------------------------- device queues */

/* KeInitializeDeviceQueue(DeviceQueue) */
static uint64_t KeInitializeDeviceQueue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    *(uint16_t *)nc_vm_host(k->vm, a[0]) = 0x14; /* DeviceQueueObject */
    wr8(k, a[0] + 2, 0x0C);
    wr8(k, a[0] + DQ_BUSY, 0);
    list_init(k, a[0] + DQ_HEAD);
    return 0;
}

/* An entry for a busy queue goes in (by key: after those of its key or
 * lower); an idle queue becomes busy instead. Returns whether it went in. */
static bool devq_insert(NcKernel *k, uint32_t q, uint32_t e, bool by_key, uint32_t key)
{
    if (!rd8(k, q + DQ_BUSY)) {
        wr8(k, q + DQ_BUSY, 1);
        wr8(k, e + DQE_INSERTED, 0);
        return false;
    }
    uint32_t next = q + DQ_HEAD;
    if (by_key) {
        wr32(k, e + DQE_KEY, key);
        for (next = rd32(k, q + DQ_HEAD); next != q + DQ_HEAD && rd32(k, next + DQE_KEY) <= key;) {
            next = rd32(k, next);
        }
    }
    list_insert_before(k, next, e);
    wr8(k, e + DQE_INSERTED, 1);
    return true;
}

/* The first entry (by key: the first of the key or above, else the first),
 * out of the queue; an empty queue goes idle and gives 0. */
static uint32_t devq_remove(NcKernel *k, uint32_t q, bool by_key, uint32_t key)
{
    const uint32_t head = q + DQ_HEAD;
    if (list_empty(k, head)) {
        wr8(k, q + DQ_BUSY, 0);
        return 0;
    }
    uint32_t e = rd32(k, head);
    if (by_key) {
        while (e != head && rd32(k, e + DQE_KEY) < key) {
            e = rd32(k, e);
        }
        if (e == head) {
            e = rd32(k, head);
        }
    }
    list_remove(k, e);
    wr8(k, e + DQE_INSERTED, 0);
    return e;
}

/* KeInsertDeviceQueue(DeviceQueue, DeviceQueueEntry) -> inserted */
static uint64_t KeInsertDeviceQueue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return devq_insert(k, a[0], a[1], false, 0);
}

/* KeInsertByKeyDeviceQueue(DeviceQueue, DeviceQueueEntry, SortKey) -> inserted */
static uint64_t KeInsertByKeyDeviceQueue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return devq_insert(k, a[0], a[1], true, a[2]);
}

/* KeRemoveDeviceQueue(DeviceQueue) -> entry or NULL */
static uint64_t KeRemoveDeviceQueue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return devq_remove(k, a[0], false, 0);
}

/* KeRemoveByKeyDeviceQueue(DeviceQueue, SortKey) -> entry or NULL */
static uint64_t KeRemoveByKeyDeviceQueue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return devq_remove(k, a[0], true, a[1]);
}

/* KeRemoveEntryDeviceQueue(DeviceQueue, DeviceQueueEntry) -> it was queued */
static uint64_t KeRemoveEntryDeviceQueue(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    if (!rd8(k, a[1] + DQE_INSERTED)) {
        return 0;
    }
    list_remove(k, a[1]);
    wr8(k, a[1] + DQE_INSERTED, 0);
    return 1;
}

/* ---------------------------------------------------------------- IRPs */

static void irp_init(NcKernel *k, uint32_t irp, uint16_t size, uint8_t stack)
{
    memset(nc_vm_host(k->vm, irp), 0, size);
    *(uint16_t *)nc_vm_host(k->vm, irp + IRP_TYPE) = 6; /* IO_TYPE_IRP */
    *(uint16_t *)nc_vm_host(k->vm, irp + IRP_SIZE) = size;
    wr8(k, irp + IRP_STACK_COUNT, stack);
    wr8(k, irp + IRP_LOCATION, (uint8_t)(stack + 1));
    list_init(k, irp + IRP_THREAD_LIST);
    wr32(k, irp + IRP_STACK, irp + IRP_BASE + stack * SL_SIZE);
}

static uint32_t irp_alloc(NcKernel *k, uint8_t stack)
{
    const uint32_t size = IRP_BASE + stack * SL_SIZE;
    const uint32_t irp = nc_ex_alloc(k, size);
    if (irp) {
        irp_init(k, irp, (uint16_t)size, stack);
    }
    return irp;
}

/* Into the thread's IrpList (ETHREAD +0x134, set up on first use). */
static void irp_thread_link(NcKernel *k, uint32_t irp, uint32_t ethread)
{
    if (!ethread) {
        return;
    }
    const uint32_t head = ethread + ET_IRP_LIST;
    if (!rd32(k, head)) {
        list_init(k, head);
    }
    list_insert_before(k, head, irp + IRP_THREAD_LIST);
}

static void irp_thread_unlink(NcKernel *k, uint32_t irp)
{
    const uint32_t e = irp + IRP_THREAD_LIST;
    if (rd32(k, e) && rd32(k, e) != e) {
        list_remove(k, e);
    }
    list_init(k, e);
}

static uint32_t current_ethread(const NcKernel *k)
{
    return k->current ? k->current->ethread : 0;
}

/* IoAllocateIrp(StackSize) -> IRP or NULL, from the pool */
static uint64_t IoAllocateIrp(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return irp_alloc(k, (uint8_t)a[0]);
}

/* IoInitializeIrp(Irp, PacketSize, StackSize) */
static uint64_t IoInitializeIrp(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    irp_init(k, a[0], (uint16_t)a[1], (uint8_t)a[2]);
    return 0;
}

/* IoFreeIrp(Irp) */
static uint64_t IoFreeIrp(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    nc_ex_free(k, a[0]);
    return 0;
}

/* IoQueueThreadIrp(Irp): into the running thread's IRP list */
static uint64_t IoQueueThreadIrp(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    irp_thread_link(k, a[0], current_ethread(k));
    return 0;
}

/* IoMarkIrpMustComplete(Irp): the Xbox doesn't cancel a thread's IRPs */
static uint64_t IoMarkIrpMustComplete(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return 0;
}

/* ---------------------------------------------------------- completion */

static uint32_t port_of(NcKernel *k, uint32_t fo, uint32_t *key);

/* What IopCompleteRequest does once the completion routines are done. */
static void irp_finish(NcKernel *k, uint32_t irp, int boost)
{
    const uint32_t flags = rd32(k, irp + IRP_FLAGS);
    const uint32_t iosb = rd32(k, irp + IRP_USER_IOSB), event = rd32(k, irp + IRP_USER_EVENT);
    const uint32_t status = rd32(k, irp + IRP_STATUS);
    if (flags & (IRP_CLOSE | IRP_PAGING_IO)) {
        if (iosb) {
            wr32(k, iosb, status);
            wr32(k, iosb + 4, rd32(k, irp + IRP_INFORMATION));
        }
        if (event) {
            nc_ke_set_event(k, event, boost);
        }
        if (flags & IRP_PAGING_IO) {
            nc_ex_free(k, irp);
        }
        return;
    }
    const bool pending = rd8(k, irp + IRP_PENDING);
    if ((flags & IRP_DEFER) && !pending) {
        return; /* the service that sent it finishes it */
    }
    irp_thread_unlink(k, irp);
    if (status >= 0xC0000000u && !pending) {
        nc_ex_free(k, irp); /* the caller has the error from the call itself */
        return;
    }
    const uint32_t fo = rd32(k, irp + IRP_FILE);
    if (iosb) {
        wr32(k, iosb, status);
        wr32(k, iosb + 4, rd32(k, irp + IRP_INFORMATION));
    }
    if (event) {
        nc_ke_set_event(k, event, boost);
    } else if (fo) {
        wr32(k, fo + FO_FINAL_STATUS, status);
        nc_ke_set_event(k, fo + FO_EVENT, boost);
    }
    const uint32_t apc = rd32(k, irp + IRP_APC_ROUTINE), apc_ctx = rd32(k, irp + IRP_APC_CONTEXT);
    uint32_t key = 0, port = 0;
    if (apc) {
        NcThread *t = nc_ps_thread_of(k, rd32(k, irp + IRP_THREAD));
        if (t && t->state != NC_T_TERMINATED) {
            const NcApc a = {.normal = apc, .ctx = apc_ctx, .a1 = iosb, .a2 = 0};
            nc_apc_queue(k, t, &a, boost);
        }
    } else if (fo && apc_ctx && (port = port_of(k, fo, &key)) != 0) {
        wr32(k, irp + IRP_KEY, key);
        wr32(k, irp + IRP_PACKET_TYPE, 0);
        nc_ke_queue_insert(k, port, irp + IRP_LIST_ENTRY, false);
        return; /* NtRemoveIoCompletion frees it */
    }
    nc_ex_free(k, irp);
}

/* The location under the current one is done: the current goes up one. */
static void irp_up(NcKernel *k, uint32_t irp)
{
    wr8(k, irp + IRP_LOCATION, (uint8_t)(rd8(k, irp + IRP_LOCATION) + 1));
    wr32(k, irp + IRP_STACK, rd32(k, irp + IRP_STACK) + SL_SIZE);
}

/* IofCompleteRequest's walk up the stack locations (the current one already
 * moved past the one to do): each one's completion routine, as its Control
 * asks, runs as a callout; one that returns STATUS_MORE_PROCESSING_REQUIRED
 * stops the walk. Then irp_finish, its waiter boosted by boost. The kernel
 * call returns result (now, or from the continuation after the routines). */
static uint64_t irp_walk(NcKernel *k, NcFrame *f, uint32_t irp, uint32_t result, int boost)
{
    for (;;) {
        const int loc = (int8_t)rd8(k, irp + IRP_LOCATION), count = (int8_t)rd8(k, irp + IRP_STACK_COUNT);
        if (loc > count + 1) {
            break;
        }
        const uint32_t up = rd32(k, irp + IRP_STACK), sp = up - SL_SIZE;
        const uint8_t control = rd8(k, sp + SL_CONTROL);
        const uint32_t status = rd32(k, irp + IRP_STATUS);
        const uint32_t routine = rd32(k, sp + SL_ROUTINE), context = rd32(k, sp + SL_CONTEXT);
        wr8(k, irp + IRP_PENDING, control & SL_PENDING);
        const bool run = routine && ((nt_success(status) && (control & SL_ON_SUCCESS)) ||
                                     (!nt_success(status) && (control & SL_ON_ERROR)) ||
                                     (rd8(k, irp + IRP_CANCEL) && (control & SL_ON_CANCEL)));
        wr8(k, sp + SL_MINOR, 0);
        wr8(k, sp + SL_FLAGS, 0);
        wr8(k, sp + SL_CONTROL, 0);
        wr32(k, sp + SL_ROUTINE, 0);
        wr32(k, sp + SL_CONTEXT, 0);
        if (run) {
            const uint32_t args[3] = {loc == count + 1 ? 0 : rd32(k, up + SL_DEVICE), irp, context};
            const uint32_t ctx[NC_KCONT_WORDS] = {irp, result, (uint32_t)boost};
            nc_kernel_callout_cont(k, f, routine, args, 3, 0xFF, NC_KCONT_IRP_COMPLETE, ctx);
            return 0;
        }
        if ((control & SL_PENDING) && loc <= count) {
            wr8(k, up + SL_CONTROL, rd8(k, up + SL_CONTROL) | SL_PENDING);
        }
        irp_up(k, irp);
    }
    irp_finish(k, irp, boost);
    return result;
}

static uint64_t irp_complete_cont(NcKernel *k, NcFrame *f, uint32_t eax, const uint32_t *ctx)
{
    if (eax == STATUS_MORE_PROCESSING_REQUIRED) {
        return ctx[1];
    }
    irp_up(k, ctx[0]);
    return irp_walk(k, f, ctx[0], ctx[1], (int32_t)ctx[2]);
}

/* Completes irp from its current stack location (IofCompleteRequest's
 * PriorityBoost: boost); the call returns result. */
static uint64_t irp_complete(NcKernel *k, NcFrame *f, uint32_t irp, uint32_t result, int boost)
{
    const int loc = (int8_t)rd8(k, irp + IRP_LOCATION), count = (int8_t)rd8(k, irp + IRP_STACK_COUNT);
    if (loc > count + 1) {
        nc_kernel_request_stop(k, f, "KeBugCheckEx(MULTIPLE_IRP_COMPLETE_REQUESTS 0x44, %08x)", irp);
        return 0;
    }
    irp_up(k, irp);
    return irp_walk(k, f, irp, result, boost);
}

/* IofCompleteRequest(Irp, PriorityBoost) */
static uint64_t IofCompleteRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return irp_complete(k, f, a[0], 0, (int8_t)a[1]);
}

/* IoInvalidDeviceRequest(DeviceObject, Irp) -> STATUS_INVALID_DEVICE_REQUEST, the IRP completed */
static uint64_t IoInvalidDeviceRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    wr32(k, a[1] + IRP_STATUS, STATUS_INVALID_DEVICE_REQUEST);
    wr32(k, a[1] + IRP_INFORMATION, 0);
    return irp_complete(k, f, a[1], STATUS_INVALID_DEVICE_REQUEST, 0);
}

/* ------------------------------------------------------------ drivers */

/* Down a stack location to dev; its dispatch routine, or 0 for one of the
 * kernel's own devices. *sp: the new location. False: no location left. */
static bool irp_down(NcKernel *k, NcFrame *f, uint32_t dev, uint32_t irp, uint32_t *routine, uint32_t *sp)
{
    const int loc = (int8_t)rd8(k, irp + IRP_LOCATION) - 1;
    wr8(k, irp + IRP_LOCATION, (uint8_t)loc);
    if (loc <= 0) {
        nc_kernel_request_stop(k, f, "KeBugCheckEx(NO_MORE_IRP_STACK_LOCATIONS 0x35, %08x)", irp);
        return false;
    }
    *sp = rd32(k, irp + IRP_STACK) - SL_SIZE;
    wr32(k, irp + IRP_STACK, *sp);
    wr32(k, *sp + SL_DEVICE, dev);
    const uint32_t drv = rd32(k, dev + DEV_DRIVER);
    *routine = drv ? rd32(k, drv + DRV_MAJOR + 4 * (rd8(k, *sp + SL_MAJOR) & 0x0F)) : 0;
    return true;
}

/* An IRP for one of the kernel's own devices, done and completed now. */
static uint32_t kernel_device(NcKernel *k, uint32_t dev, uint32_t irp, uint32_t sp)
{
    uint32_t info;
    const uint32_t st = nc_io_device_irp(k, dev, irp, sp, &info);
    wr32(k, irp + IRP_STATUS, st);
    wr32(k, irp + IRP_INFORMATION, info);
    return st;
}

/* IofCallDriver(DeviceObject, Irp) -> the dispatch routine's status */
static uint64_t IofCallDriver(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t routine, sp;
    if (!irp_down(k, f, a[0], a[1], &routine, &sp)) {
        return 0;
    }
    if (!rd32(k, a[0] + DEV_DRIVER)) {
        const uint32_t st = kernel_device(k, a[0], a[1], sp);
        return irp_complete(k, f, a[1], st, IO_DISK_INCREMENT);
    }
    if (!routine) {
        wr32(k, a[1] + IRP_STATUS, STATUS_INVALID_DEVICE_REQUEST);
        wr32(k, a[1] + IRP_INFORMATION, 0);
        return irp_complete(k, f, a[1], STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    const uint32_t args[2] = {a[0], a[1]};
    nc_kernel_callout(k, f, routine, args, 2);
    return 0;
}

/* The next stack location for an IRP the kernel builds for dev. */
static uint32_t build(NcKernel *k, uint32_t major, uint32_t dev, uint32_t iosb, uint32_t *sp)
{
    const uint32_t irp = irp_alloc(k, rd8(k, dev + DEV_STACK_SIZE));
    if (!irp) {
        return 0;
    }
    *sp = rd32(k, irp + IRP_STACK) - SL_SIZE;
    wr8(k, *sp + SL_MAJOR, (uint8_t)major);
    wr32(k, irp + IRP_USER_IOSB, iosb);
    wr32(k, irp + IRP_THREAD, current_ethread(k));
    return irp;
}

static uint32_t build_fsd(NcKernel *k, uint32_t major, uint32_t dev, uint32_t buf, uint32_t len, uint32_t offset,
                          uint32_t iosb)
{
    uint32_t sp;
    const uint32_t irp = build(k, major, dev, iosb, &sp);
    if (irp && major != IRP_MJ_FLUSH_BUFFERS && major != IRP_MJ_SHUTDOWN) {
        wr32(k, irp + IRP_USER_BUFFER, buf);
        wr32(k, sp + SL_LENGTH, len);
        if (offset) {
            wr32(k, sp + SL_OFFSET, rd32(k, offset));
            wr32(k, sp + SL_OFFSET + 4, rd32(k, offset + 4));
        }
    }
    return irp;
}

/* IoBuildAsynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset,
 *   IoStatusBlock) -> IRP or NULL */
static uint64_t IoBuildAsynchronousFsdRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return build_fsd(k, a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* IoBuildSynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset,
 *   Event, IoStatusBlock) -> IRP or NULL, in the thread's IRP list */
static uint64_t IoBuildSynchronousFsdRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t irp = build_fsd(k, a[0], a[1], a[2], a[3], a[4], a[6]);
    if (irp) {
        wr32(k, irp + IRP_USER_EVENT, a[5]);
        irp_thread_link(k, irp, current_ethread(k));
    }
    return irp;
}

/* IoBuildDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
 *   OutputBuffer, OutputBufferLength, InternalDeviceIoControl, Event, IoStatusBlock) -> IRP or NULL */
static uint64_t IoBuildDeviceIoControlRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t sp;
    const uint32_t irp = build(k, (a[6] & 0xFF) ? 0x0B : 0x0A, a[1], a[8], &sp);
    if (irp) {
        wr32(k, sp + SL_OUT_LENGTH, a[5]);
        wr32(k, sp + SL_IN_BUFFER, a[2]);
        wr32(k, sp + SL_IN_LENGTH, a[3]);
        wr32(k, sp + SL_CODE, a[0]);
        wr32(k, irp + IRP_USER_BUFFER, a[4]);
        wr32(k, irp + IRP_USER_EVENT, a[7]);
        irp_thread_link(k, irp, current_ethread(k));
    }
    return irp;
}

/* A synchronous request's own event and IoStatusBlock, from the pool. */
#define SYNC_EVENT 0x00
#define SYNC_IOSB  0x10
#define SYNC_SIZE  0x18u

/* The request ended with status: its IoStatusBlock's Information to
 * ctx[1] (if given), the block freed. ctx: the block, the place for
 * Information. */
static uint64_t sync_done(NcKernel *k, const uint32_t *ctx, uint32_t status)
{
    if (ctx[1]) {
        wr32(k, ctx[1], rd32(k, ctx[0] + SYNC_IOSB + 4));
    }
    nc_ex_free(k, ctx[0]);
    return status;
}

/* A synchronous request's wait for a pending IRP ended. */
static uint64_t sync_waited(NcKernel *k, NcFrame *f, uint32_t eax, const uint32_t *ctx)
{
    return sync_done(k, ctx, rd32(k, ctx[0] + SYNC_IOSB));
}

/* The driver returned status for a synchronous request's IRP: a pending
 * one is waited for. */
static uint64_t sync_called(NcKernel *k, NcFrame *f, uint32_t status, const uint32_t *ctx)
{
    if (status != NC_STATUS_PENDING) {
        return sync_done(k, ctx, status);
    }
    nc_ke_wait_one(k, ctx[0] + SYNC_EVENT);
    if (k->current && k->current->state == NC_T_WAITING) {
        nc_kernel_wait_cont(k, f, NC_KCONT_IRP_WAITED, ctx);
        return 0;
    }
    return sync_done(k, ctx, rd32(k, ctx[0] + SYNC_IOSB));
}

/* A synchronous request's block, its event cleared, or 0. */
static uint32_t sync_block(NcKernel *k)
{
    const uint32_t block = nc_ex_alloc(k, SYNC_SIZE);
    if (block) {
        nc_ke_init_event(k, block + SYNC_EVENT, 0, false);
        wr32(k, block + SYNC_IOSB, 0);
        wr32(k, block + SYNC_IOSB + 4, 0);
    }
    return block;
}

/* Sends irp (built for block's event and IoStatusBlock) to dev and waits for
 * it; the call returns the status, Information going to returned (if not 0). */
static uint64_t sync_send(NcKernel *k, NcFrame *f, uint32_t dev, uint32_t irp, uint32_t block, uint32_t returned)
{
    const uint32_t ctx[NC_KCONT_WORDS] = {block, returned};
    wr32(k, irp + IRP_USER_EVENT, block + SYNC_EVENT);
    irp_thread_link(k, irp, current_ethread(k));
    uint32_t routine, sp;
    if (!irp_down(k, f, dev, irp, &routine, &sp)) {
        return 0;
    }
    if (!routine) {
        /* the kernel's own device (a driver without the major: invalid) */
        const uint32_t st = rd32(k, dev + DEV_DRIVER) ? STATUS_INVALID_DEVICE_REQUEST : kernel_device(k, dev, irp, sp);
        wr32(k, irp + IRP_STATUS, st);
        irp_complete(k, f, irp, st, IO_DISK_INCREMENT);
        return k->redirected ? 0 : sync_called(k, f, st, ctx); /* no completion routine on the kernel's own */
    }
    const uint32_t args[2] = {dev, irp};
    nc_kernel_callout_cont(k, f, routine, args, 2, 0xFF, NC_KCONT_IRP_CALLED, ctx);
    return 0;
}

/* IoSynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset) -> status:
 * the request built, sent and waited for */
static uint64_t IoSynchronousFsdRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t block = sync_block(k);
    if (!block) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    const uint32_t irp = build_fsd(k, a[0], a[1], a[2], a[3], a[4], block + SYNC_IOSB);
    if (!irp) {
        nc_ex_free(k, block);
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    return sync_send(k, f, a[1], irp, block, 0);
}

/* IoSynchronousDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
 *   OutputBuffer, OutputBufferLength, ReturnedOutputBufferLength, InternalDeviceIoControl) -> status:
 * IoBuildDeviceIoControlRequest's IRP, sent and waited for */
static uint64_t IoSynchronousDeviceIoControlRequest(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t block = sync_block(k);
    if (!block) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    const uint32_t b[9] = {a[0], a[1], a[2], a[3], a[4], a[5], a[7], 0, block + SYNC_IOSB};
    const uint32_t irp = (uint32_t)IoBuildDeviceIoControlRequest(k, f, b);
    if (!irp) {
        nc_ex_free(k, block);
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    irp_thread_unlink(k, irp); /* sync_send links it */
    return sync_send(k, f, a[1], irp, block, a[6]);
}

/* --------------------------------------------------------------- StartIo */

/* dev's StartIo for irp, at DISPATCH_LEVEL, as its current IRP. */
static uint64_t start_io(NcKernel *k, NcFrame *f, uint32_t dev, uint32_t irp)
{
    wr32(k, dev + DEV_CURRENT_IRP, irp);
    const uint32_t routine = rd32(k, rd32(k, dev + DEV_DRIVER) + DRV_STARTIO);
    if (!routine) {
        return 0;
    }
    wr8(k, dev + DEV_STARTIO, rd8(k, dev + DEV_STARTIO) | STARTIO_INSIDE);
    const uint32_t args[2] = {dev, irp};
    const uint32_t ctx[NC_KCONT_WORDS] = {dev};
    nc_kernel_callout_cont(k, f, routine, args, 2, 2, NC_KCONT_START_IO, ctx);
    return 0;
}

/* dev's next packet to StartIo (by key: the first of the key or above);
 * from inside StartIo, after it returns. */
static uint64_t next_packet(NcKernel *k, NcFrame *f, uint32_t dev, bool by_key, uint32_t key)
{
    const uint8_t flags = rd8(k, dev + DEV_STARTIO);
    if (flags & STARTIO_INSIDE) {
        wr8(k, dev + DEV_STARTIO, flags | STARTIO_DEFERRED | (by_key ? STARTIO_BY_KEY : 0));
        wr32(k, dev + DEV_STARTIO_KEY, key);
        return 0;
    }
    wr32(k, dev + DEV_CURRENT_IRP, 0);
    const uint32_t e = devq_remove(k, dev + DEV_QUEUE, by_key, key);
    return e ? start_io(k, f, dev, e - IRP_QUEUE_ENTRY) : 0;
}

/* StartIo returned: a next packet it asked for starts now. */
static uint64_t start_io_cont(NcKernel *k, NcFrame *f, uint32_t eax, const uint32_t *ctx)
{
    const uint32_t dev = ctx[0];
    const uint8_t flags = rd8(k, dev + DEV_STARTIO);
    wr8(k, dev + DEV_STARTIO, flags & ~(STARTIO_INSIDE | STARTIO_DEFERRED | STARTIO_BY_KEY));
    if (flags & STARTIO_DEFERRED) {
        return next_packet(k, f, dev, flags & STARTIO_BY_KEY, rd32(k, dev + DEV_STARTIO_KEY));
    }
    return 0;
}

/* IoStartPacket(DeviceObject, Irp, Key): into the device's queue (by *Key if
 * given), or to StartIo at once when the device is idle */
static uint64_t IoStartPacket(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t dev = a[0], irp = a[1];
    if (devq_insert(k, dev + DEV_QUEUE, irp + IRP_QUEUE_ENTRY, a[2] != 0, a[2] ? rd32(k, a[2]) : 0)) {
        return 0;
    }
    return start_io(k, f, dev, irp);
}

/* IoStartNextPacket(DeviceObject) */
static uint64_t IoStartNextPacket(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return next_packet(k, f, a[0], false, 0);
}

/* IoStartNextPacketByKey(DeviceObject, Key) */
static uint64_t IoStartNextPacketByKey(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return next_packet(k, f, a[0], true, a[1]);
}

/* ---------------------------------------------------------- share access */

/* FILE_OBJECT's access byte (+2): ReadAccess 2, WriteAccess 4, DeleteAccess
 * 8, SharedRead 0x10, SharedWrite 0x20, SharedDelete 0x40. SHARE_ACCESS:
 * OpenCount, Readers, Writers, Deleters, SharedRead, SharedWrite,
 * SharedDelete (bytes). */
#define FO_ACCESS 0x02
#define ACCESS_READ   (0x0001u | 0x0020u)   /* FILE_READ_DATA | FILE_EXECUTE */
#define ACCESS_WRITE  (0x0002u | 0x0004u)   /* FILE_WRITE_DATA | FILE_APPEND_DATA */
#define ACCESS_DELETE 0x00010000u
#define SHARE_READ    1u
#define SHARE_WRITE   2u
#define SHARE_DELETE  4u

typedef struct Share {
    bool read, write, del, s_read, s_write, s_delete;
} Share;

static Share share_of(uint32_t access, uint32_t share)
{
    return (Share){(access & ACCESS_READ) != 0,  (access & ACCESS_WRITE) != 0, (access & ACCESS_DELETE) != 0,
                   (share & SHARE_READ) != 0,    (share & SHARE_WRITE) != 0,   (share & SHARE_DELETE) != 0};
}

static void share_note(NcKernel *k, uint32_t fo, const Share *s)
{
    wr8(k, fo + FO_ACCESS, (uint8_t)((rd8(k, fo + FO_ACCESS) & 1) | s->read << 1 | s->write << 2 | s->del << 3 |
                                     s->s_read << 4 | s->s_write << 5 | s->s_delete << 6));
}

/* SHARE_ACCESS counts += d for an open with s. */
static void share_count(NcKernel *k, uint32_t sa, const Share *s, int d)
{
    uint8_t *c = nc_vm_host(k->vm, sa);
    c[0] = (uint8_t)(c[0] + d);
    c[1] = (uint8_t)(c[1] + d * s->read);
    c[2] = (uint8_t)(c[2] + d * s->write);
    c[3] = (uint8_t)(c[3] + d * s->del);
    c[4] = (uint8_t)(c[4] + d * s->s_read);
    c[5] = (uint8_t)(c[5] + d * s->s_write);
    c[6] = (uint8_t)(c[6] + d * s->s_delete);
}

/* IoCheckShareAccess(DesiredAccess, DesiredShareAccess, FileObject, ShareAccess, Update) -> status */
static uint64_t IoCheckShareAccess(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const Share s = share_of(a[0], a[1]);
    share_note(k, a[2], &s);
    if (!s.read && !s.write && !s.del) {
        return NC_STATUS_SUCCESS;
    }
    const uint8_t *c = nc_vm_host(k->vm, a[3]);
    const uint8_t open = c[0];
    if ((s.read && c[4] < open) || (s.write && c[5] < open) || (s.del && c[6] < open) ||
        (c[1] && !s.s_read) || (c[2] && !s.s_write) || (c[3] && !s.s_delete)) {
        return 0xC0000043u; /* STATUS_SHARING_VIOLATION */
    }
    if (a[4] & 0xFF) {
        share_count(k, a[3], &s, 1);
    }
    return NC_STATUS_SUCCESS;
}

/* IoSetShareAccess(DesiredAccess, DesiredShareAccess, FileObject, ShareAccess): the first open */
static uint64_t IoSetShareAccess(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const Share s = share_of(a[0], a[1]);
    share_note(k, a[2], &s);
    memset(nc_vm_host(k->vm, a[3]), 0, 7);
    if (s.read || s.write || s.del) {
        share_count(k, a[3], &s, 1);
    }
    return 0;
}

/* IoRemoveShareAccess(FileObject, ShareAccess): the file object's open out of the counts */
static uint64_t IoRemoveShareAccess(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint8_t b = rd8(k, a[0] + FO_ACCESS);
    const Share s = {b & 2, b & 4, b & 8, b & 0x10, b & 0x20, b & 0x40};
    if (s.read || s.write || s.del) {
        share_count(k, a[1], &s, -1);
    }
    return 0;
}

/* ---------------------------------------------------- completion ports */

/* The port and key of file object fo's CompletionContext, or 0. */
static uint32_t port_of(NcKernel *k, uint32_t fo, uint32_t *key)
{
    const uint32_t cc = rd32(k, fo + FO_COMPLETION);
    if (!cc) {
        return 0;
    }
    *key = rd32(k, cc + 4);
    return rd32(k, cc);
}

static uint32_t port_post(NcKernel *k, uint32_t port, uint32_t key, uint32_t apc_ctx, uint32_t status,
                          uint32_t info)
{
    const uint32_t p = nc_ex_alloc(k, PACKET_SIZE);
    if (!p) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    wr32(k, p + PK_TYPE, 1);
    wr32(k, p + PK_KEY, key);
    wr32(k, p + PK_APC_CONTEXT, apc_ctx);
    wr32(k, p + PK_STATUS, status);
    wr32(k, p + PK_INFORMATION, info);
    nc_ke_queue_insert(k, port, p, false);
    return NC_STATUS_SUCCESS;
}

static uint32_t port_handle(NcKernel *k, uint32_t handle, uint32_t *status)
{
    enum NcObType type;
    const uint32_t port = nc_ob_lookup(k, handle, &type);
    *status = !port ? NC_STATUS_INVALID_HANDLE
            : type != NC_OB_IO_COMPLETION ? NC_STATUS_OBJECT_TYPE_MISMATCH : NC_STATUS_SUCCESS;
    return *status == NC_STATUS_SUCCESS ? port : 0;
}

uint32_t nc_iomgr_file_port(NcKernel *k, uint32_t fo, uint32_t port_handle_v, uint32_t key)
{
    uint32_t st;
    const uint32_t port = port_handle(k, port_handle_v, &st);
    if (!port) {
        return st;
    }
    if (!fo || rd32(k, fo + FO_COMPLETION)) {
        return NC_STATUS_INVALID_PARAMETER;
    }
    const uint32_t cc = nc_ex_alloc(k, 8);
    if (!cc) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    wr32(k, cc, port);
    wr32(k, cc + 4, key);
    wr32(k, fo + FO_COMPLETION, cc);
    wr32(k, port - 0x10, rd32(k, port - 0x10) + 1); /* the file holds the port */
    return NC_STATUS_SUCCESS;
}

void nc_iomgr_file_post(NcKernel *k, uint32_t fo, uint32_t apc_ctx, uint32_t status, uint32_t info)
{
    uint32_t key = 0;
    const uint32_t port = apc_ctx ? port_of(k, fo, &key) : 0;
    if (port) {
        port_post(k, port, key, apc_ctx, status, info);
    }
}

/* NtCreateIoCompletion(IoCompletionHandle, DesiredAccess, ObjectAttributes, Count): a KQUEUE */
static uint64_t NtCreateIoCompletion(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    const uint32_t port = nc_ob_create(k, NC_OB_IO_COMPLETION, 0x28);
    if (!port) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    nc_ke_queue_init(k, port, a[3]);
    const uint32_t h = nc_ob_handle(k, port, NC_OB_IO_COMPLETION);
    if (!h) {
        return NC_STATUS_INSUFFICIENT_RESOURCES;
    }
    wr32(k, a[0], h);
    return NC_STATUS_SUCCESS;
}

/* NtSetIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatus, IoStatusInformation) */
static uint64_t NtSetIoCompletion(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t st;
    const uint32_t port = port_handle(k, a[0], &st);
    return port ? port_post(k, port, a[1], a[2], a[3], a[4]) : st;
}

/* IoSetIoCompletion(IoCompletion, KeyContext, ApcContext, IoStatus, IoStatusInformation) */
static uint64_t IoSetIoCompletion(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    return port_post(k, a[0], a[1], a[2], a[3], a[4]);
}

/* NtQueryIoCompletion(IoCompletionHandle, IoCompletionInformation) -> { Depth } */
static uint64_t NtQueryIoCompletion(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t st;
    const uint32_t port = port_handle(k, a[0], &st);
    if (port) {
        wr32(k, a[1], (uint32_t)nc_ke_queue_depth(k, port));
    }
    return st;
}

/* A port's entry out to NtRemoveIoCompletion's KeyContext, ApcContext and
 * IoStatusBlock (ctx), and freed; a status (no entry) returns as it is. */
static uint64_t removed(NcKernel *k, uint32_t entry, const uint32_t *ctx)
{
    if (entry == NC_STATUS_TIMEOUT || entry == STATUS_USER_APC) {
        return entry;
    }
    uint32_t block, key, apc_ctx, status, info;
    if (rd32(k, entry + PK_TYPE) == 1) {
        block = entry;
        key = rd32(k, entry + PK_KEY);
        apc_ctx = rd32(k, entry + PK_APC_CONTEXT);
        status = rd32(k, entry + PK_STATUS);
        info = rd32(k, entry + PK_INFORMATION);
    } else {
        block = entry - IRP_LIST_ENTRY;
        key = rd32(k, block + IRP_KEY);
        apc_ctx = rd32(k, block + IRP_APC_CONTEXT);
        status = rd32(k, block + IRP_STATUS);
        info = rd32(k, block + IRP_INFORMATION);
    }
    nc_ex_free(k, block);
    wr32(k, ctx[0], key);
    wr32(k, ctx[1], apc_ctx);
    wr32(k, ctx[2], status);
    wr32(k, ctx[2] + 4, info);
    return NC_STATUS_SUCCESS;
}

static uint64_t remove_waited(NcKernel *k, NcFrame *f, uint32_t eax, const uint32_t *ctx)
{
    return removed(k, eax, ctx);
}

/* NtRemoveIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatusBlock, Timeout) */
static uint64_t NtRemoveIoCompletion(NcKernel *k, NcFrame *f, const uint32_t *a)
{
    uint32_t st;
    const uint32_t port = port_handle(k, a[0], &st);
    if (!port) {
        return st;
    }
    const uint32_t ctx[NC_KCONT_WORDS] = {a[1], a[2], a[3]};
    const uint32_t r = nc_ke_queue_remove(k, port, a[4]);
    if (k->current && k->current->state == NC_T_WAITING) {
        nc_kernel_wait_cont(k, f, NC_KCONT_IO_REMOVE, ctx);
        return 0;
    }
    return removed(k, r, ctx);
}

void nc_iomgr_register(NcKernel *k)
{
    nc_kernel_impl(k, "KeInitializeDeviceQueue", KeInitializeDeviceQueue);
    nc_kernel_impl(k, "KeInsertDeviceQueue", KeInsertDeviceQueue);
    nc_kernel_impl(k, "KeInsertByKeyDeviceQueue", KeInsertByKeyDeviceQueue);
    nc_kernel_impl(k, "KeRemoveDeviceQueue", KeRemoveDeviceQueue);
    nc_kernel_impl(k, "KeRemoveByKeyDeviceQueue", KeRemoveByKeyDeviceQueue);
    nc_kernel_impl(k, "KeRemoveEntryDeviceQueue", KeRemoveEntryDeviceQueue);

    nc_kernel_impl(k, "IoAllocateIrp", IoAllocateIrp);
    nc_kernel_impl(k, "IoInitializeIrp", IoInitializeIrp);
    nc_kernel_impl(k, "IoFreeIrp", IoFreeIrp);
    nc_kernel_impl(k, "IoQueueThreadIrp", IoQueueThreadIrp);
    nc_kernel_impl(k, "IoMarkIrpMustComplete", IoMarkIrpMustComplete);
    nc_kernel_impl(k, "IofCallDriver", IofCallDriver);
    nc_kernel_impl(k, "IofCompleteRequest", IofCompleteRequest);
    nc_kernel_impl(k, "IoInvalidDeviceRequest", IoInvalidDeviceRequest);
    nc_kernel_impl(k, "IoBuildAsynchronousFsdRequest", IoBuildAsynchronousFsdRequest);
    nc_kernel_impl(k, "IoBuildSynchronousFsdRequest", IoBuildSynchronousFsdRequest);
    nc_kernel_impl(k, "IoBuildDeviceIoControlRequest", IoBuildDeviceIoControlRequest);
    nc_kernel_impl(k, "IoSynchronousFsdRequest", IoSynchronousFsdRequest);
    nc_kernel_impl(k, "IoSynchronousDeviceIoControlRequest", IoSynchronousDeviceIoControlRequest);
    nc_kernel_impl(k, "IoStartPacket", IoStartPacket);
    nc_kernel_impl(k, "IoStartNextPacket", IoStartNextPacket);
    nc_kernel_impl(k, "IoStartNextPacketByKey", IoStartNextPacketByKey);

    nc_kernel_impl(k, "IoCheckShareAccess", IoCheckShareAccess);
    nc_kernel_impl(k, "IoSetShareAccess", IoSetShareAccess);
    nc_kernel_impl(k, "IoRemoveShareAccess", IoRemoveShareAccess);

    nc_kernel_impl(k, "NtCreateIoCompletion", NtCreateIoCompletion);
    nc_kernel_impl(k, "NtSetIoCompletion", NtSetIoCompletion);
    nc_kernel_impl(k, "IoSetIoCompletion", IoSetIoCompletion);
    nc_kernel_impl(k, "NtQueryIoCompletion", NtQueryIoCompletion);
    nc_kernel_impl(k, "NtRemoveIoCompletion", NtRemoveIoCompletion);

    nc_kernel_cont(NC_KCONT_IRP_COMPLETE, irp_complete_cont);
    nc_kernel_cont(NC_KCONT_IRP_CALLED, sync_called);
    nc_kernel_cont(NC_KCONT_IRP_WAITED, sync_waited);
    nc_kernel_cont(NC_KCONT_START_IO, start_io_cont);
    nc_kernel_cont(NC_KCONT_IO_REMOVE, remove_waited);
}
