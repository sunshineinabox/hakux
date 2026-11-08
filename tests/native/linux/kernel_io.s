/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest test of the kernel's queues, I/O completion ports, IRPs and drivers
 * (a driver with StartIo and completion routines, IRPs to \Device\CdRom0),
 * thread notify routines and RtlSprintf, run by kernel_io.py. Linked at
 * 0x11000; the kernel thunks are at 0x10f00 in kernel_io.py's IMPORTS order.
 * Ends in HLT after DbgPrint "k85: all passed", or "k85: failed at step n". */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,                     THUNK + 4*0
.set KeInitializeQueue,            THUNK + 4*1
.set KeInsertQueue,                THUNK + 4*2
.set KeRemoveQueue,                THUNK + 4*3
.set PsCreateSystemThread,         THUNK + 4*4
.set NtCreateIoCompletion,         THUNK + 4*5
.set NtSetIoCompletion,            THUNK + 4*6
.set NtRemoveIoCompletion,         THUNK + 4*7
.set NtQueryIoCompletion,          THUNK + 4*8
.set IoCreateDevice,               THUNK + 4*9
.set IoAllocateIrp,                THUNK + 4*10
.set IofCallDriver,                THUNK + 4*11
.set IofCompleteRequest,           THUNK + 4*12
.set IoStartPacket,                THUNK + 4*13
.set IoStartNextPacket,            THUNK + 4*14
.set IoFreeIrp,                    THUNK + 4*15
.set IoInvalidDeviceRequest,       THUNK + 4*16
.set ObReferenceObjectByName,      THUNK + 4*17
.set IoSynchronousFsdRequest,      THUNK + 4*18
.set IoBuildSynchronousFsdRequest, THUNK + 4*19
.set KeInitializeEvent,            THUNK + 4*20
.set KeDelayExecutionThread,       THUNK + 4*21
.set RtlSprintf,                   THUNK + 4*22
.set PsSetCreateThreadNotifyRoutine, THUNK + 4*23
.set KeInsertHeadQueue,            THUNK + 4*24
.set KeRundownQueue,               THUNK + 4*25
.set IoSynchronousDeviceIoControlRequest, THUNK + 4*26
.set KeWaitForSingleObject,        THUNK + 4*27
.set KeSetEvent,                   THUNK + 4*28
.set KeSetDisableBoostThread,      THUNK + 4*29
.set KeQueryBasePriorityThread,    THUNK + 4*30
.set KeQuerySystemTime,            THUNK + 4*31

/* push imm32 (LLVM's Intel syntax makes "push offset sym" a 16-bit push) */
.macro PUSHI sym
    .byte 0x68
    .long \sym
.endm

.macro STEP n
    mov dword ptr [step], \n
.endm

.text
.globl _start
_start:
    /* 1. a kernel queue: first in, first out; a poll times out */
    STEP 1
    push 0
    PUSHI queue
    call dword ptr [KeInitializeQueue]
    PUSHI e1
    PUSHI queue
    call dword ptr [KeInsertQueue]
    test eax, eax
    jnz fail
    PUSHI e2
    PUSHI queue
    call dword ptr [KeInsertQueue]
    cmp eax, 1
    jne fail
    STEP 2
    push 0
    push 0
    PUSHI queue
    call dword ptr [KeRemoveQueue]
    cmp eax, offset e1
    jne fail
    push 0
    push 0
    PUSHI queue
    call dword ptr [KeRemoveQueue]
    cmp eax, offset e2
    jne fail
    STEP 3
    PUSHI zero64
    push 0
    PUSHI queue
    call dword ptr [KeRemoveQueue]
    cmp eax, 0x102
    jne fail
    /* head insert, then rundown returns the entries as a ring */
    PUSHI e1
    PUSHI queue
    call dword ptr [KeInsertQueue]
    PUSHI e2
    PUSHI queue
    call dword ptr [KeInsertHeadQueue]
    PUSHI queue
    call dword ptr [KeRundownQueue]
    cmp eax, offset e2
    jne fail
    cmp dword ptr [e2], offset e1
    jne fail
    cmp dword ptr [e1], offset e2
    jne fail

    /* 2. a blocking remove: a new thread inserts e3 */
    STEP 4
    push 0
    PUSHI queue
    PUSHI worker
    push 0
    PUSHI hthread
    call dword ptr [PsCreateSystemThread]
    test eax, eax
    jnz fail
    push 0
    push 0
    PUSHI queue
    call dword ptr [KeRemoveQueue]
    cmp eax, offset e3
    jne fail

    /* 3. a completion port */
    STEP 5
    push 0
    push 0
    push 0
    PUSHI hport
    call dword ptr [NtCreateIoCompletion]
    test eax, eax
    jnz fail
    push 0x44
    push 0x33
    push 0x22
    push 0x11
    push dword ptr [hport]
    call dword ptr [NtSetIoCompletion]
    test eax, eax
    jnz fail
    STEP 6
    PUSHI depth
    push dword ptr [hport]
    call dword ptr [NtQueryIoCompletion]
    test eax, eax
    jnz fail
    cmp dword ptr [depth], 1
    jne fail
    STEP 7
    push 0
    PUSHI iosb
    PUSHI apcctx
    PUSHI key
    push dword ptr [hport]
    call dword ptr [NtRemoveIoCompletion]
    test eax, eax
    jnz fail
    cmp dword ptr [key], 0x11
    jne fail
    cmp dword ptr [apcctx], 0x22
    jne fail
    cmp dword ptr [iosb], 0x33
    jne fail
    cmp dword ptr [iosb + 4], 0x44
    jne fail
    STEP 8
    push 0
    PUSHI hport
    PUSHI port_worker
    push 0
    PUSHI hthread
    call dword ptr [PsCreateSystemThread]
    test eax, eax
    jnz fail
    push 0
    PUSHI iosb
    PUSHI apcctx
    PUSHI key
    push dword ptr [hport]
    call dword ptr [NtRemoveIoCompletion]
    test eax, eax
    jnz fail
    cmp dword ptr [key], 0x55
    jne fail
    cmp dword ptr [apcctx], 0x66
    jne fail
    STEP 9
    PUSHI zero64
    PUSHI iosb
    PUSHI apcctx
    PUSHI key
    push dword ptr [hport]
    call dword ptr [NtRemoveIoCompletion]
    cmp eax, 0x102
    jne fail

    /* 4. a driver: the dispatch routine queues to StartIo, which completes
     * the IRP; its completion routine stops the completion */
    STEP 10
    mov eax, dword ptr [IoInvalidDeviceRequest]
    mov dword ptr [driver + 0x0C + 4*3], eax
    PUSHI dev
    push 0
    push 0x22
    push 0
    push 0
    PUSHI driver
    call dword ptr [IoCreateDevice]
    test eax, eax
    jnz fail
    call new_irp
    mov dword ptr [irpA], eax
    STEP 11
    mov ecx, dword ptr [dev]
    mov edx, dword ptr [irpA]
    call dword ptr [IofCallDriver]
    cmp eax, 0x103
    jne fail
    STEP 12
    cmp dword ptr [startio_count], 1
    jne fail
    cmp dword ptr [comp_count], 1
    jne fail
    cmp dword ptr [comp_ctx], 0x55
    jne fail
    cmp dword ptr [comp_dev], 0
    jne fail
    cmp dword ptr [comp_pending], 1
    jne fail
    cmp dword ptr [comp_info], 7
    jne fail
    STEP 13
    mov eax, dword ptr [dev]
    cmp dword ptr [eax + 0x10], 0
    jne fail
    cmp byte ptr [eax + 0x2B], 0
    jne fail
    push dword ptr [irpA]
    call dword ptr [IoFreeIrp]

    /* 5. two packets: the second waits in the device queue for the first */
    STEP 14
    mov dword ptr [hold], 1
    call new_irp
    mov dword ptr [irpA], eax
    call new_irp
    mov dword ptr [irpB], eax
    mov ecx, dword ptr [dev]
    mov edx, dword ptr [irpA]
    call dword ptr [IofCallDriver]
    cmp eax, 0x103
    jne fail
    mov ecx, dword ptr [dev]
    mov edx, dword ptr [irpB]
    call dword ptr [IofCallDriver]
    cmp eax, 0x103
    jne fail
    STEP 15
    cmp dword ptr [startio_count], 2
    jne fail
    mov eax, dword ptr [irpA]
    cmp dword ptr [last_irp], eax
    jne fail
    mov eax, dword ptr [dev]
    cmp byte ptr [eax + 0x2B], 1
    jne fail
    STEP 16
    mov dword ptr [hold], 0
    mov eax, dword ptr [irpA]
    mov dword ptr [eax + 0x10], 0
    mov dword ptr [eax + 0x14], 7
    mov ecx, eax
    xor edx, edx
    call dword ptr [IofCompleteRequest]
    push dword ptr [dev]
    call dword ptr [IoStartNextPacket]
    STEP 17
    cmp dword ptr [startio_count], 3
    jne fail
    mov eax, dword ptr [irpB]
    cmp dword ptr [last_irp], eax
    jne fail
    cmp dword ptr [comp_count], 3
    jne fail
    mov eax, dword ptr [dev]
    cmp dword ptr [eax + 0x10], 0
    jne fail
    cmp byte ptr [eax + 0x2B], 0
    jne fail

    /* 6. IoInvalidDeviceRequest as a dispatch routine; a completion
     * routine that lets the completion go on (the kernel frees the IRP) */
    STEP 18
    push 1
    call dword ptr [IoAllocateIrp]
    test eax, eax
    jz fail
    mov ebx, dword ptr [eax + 0x5C]
    sub ebx, 0x24
    mov byte ptr [ebx], 3
    mov dword ptr [ebx + 0x1C], offset comp2
    mov byte ptr [ebx + 3], 0xE0
    mov ecx, dword ptr [dev]
    mov edx, eax
    call dword ptr [IofCallDriver]
    cmp eax, 0xC0000010
    jne fail
    cmp dword ptr [comp2_count], 1
    jne fail
    cmp dword ptr [comp2_status], 0xC0000010
    jne fail

    /* 7. the kernel's own device: the disc's sectors */
    STEP 19
    PUSHI cd
    push 0
    push 0
    push 0
    PUSHI cdname
    call dword ptr [ObReferenceObjectByName]
    test eax, eax
    jnz fail
    PUSHI off32
    push 2048
    PUSHI buf
    push dword ptr [cd]
    push 2
    call dword ptr [IoSynchronousFsdRequest]
    test eax, eax
    jnz fail
    cmp dword ptr [buf], 0x5243494D      /* "MICR" */
    jne fail
    STEP 20
    push 0
    push 0
    PUSHI ev
    call dword ptr [KeInitializeEvent]
    PUSHI iosb
    PUSHI ev
    PUSHI off32
    push 2048
    PUSHI buf2
    push dword ptr [cd]
    push 2
    call dword ptr [IoBuildSynchronousFsdRequest]
    test eax, eax
    jz fail
    mov ecx, dword ptr [cd]
    mov edx, eax
    call dword ptr [IofCallDriver]
    test eax, eax
    jnz fail
    cmp dword ptr [iosb], 0
    jne fail
    cmp dword ptr [iosb + 4], 2048
    jne fail
    cmp dword ptr [ev + 4], 1
    jne fail
    cmp dword ptr [buf2], 0x5243494D
    jne fail

    /* 8. thread notify routines: in the creator, then as the thread ends */
    STEP 21
    PUSHI notify
    call dword ptr [PsSetCreateThreadNotifyRoutine]
    test eax, eax
    jnz fail
    push 0
    push 0
    PUSHI idle_thread
    push 0
    PUSHI hthread
    call dword ptr [PsCreateSystemThread]
    test eax, eax
    jnz fail
    cmp dword ptr [notify_create], 1
    jne fail
    STEP 22
    PUSHI delay
    push 0
    push 0
    call dword ptr [KeDelayExecutionThread]
    cmp dword ptr [notify_exit], 1
    jne fail
    cmp dword ptr [idle_ran], 1
    jne fail

    /* 9. RtlSprintf (cdecl) */
    STEP 23
    PUSHI ok
    push 42
    PUSHI fmt
    PUSHI buf
    call dword ptr [RtlSprintf]
    add esp, 16
    cmp eax, 5
    jne fail
    cmp dword ptr [buf], 0x6F2D3234      /* "42-o" */
    jne fail

    /* 10. IoSynchronousDeviceIoControlRequest on the kernel's devices: the
     * disc's geometry (2048-byte sectors), a partition's (512), an unknown
     * code refused */
    STEP 24
    push 0
    PUSHI returned
    push 24
    PUSHI geometry
    push 0
    push 0
    push dword ptr [cd]
    push 0x2404C                        /* IOCTL_CDROM_GET_DRIVE_GEOMETRY */
    call dword ptr [IoSynchronousDeviceIoControlRequest]
    test eax, eax
    jnz fail
    cmp dword ptr [returned], 24
    jne fail
    cmp dword ptr [geometry + 20], 2048
    jne fail
    cmp dword ptr [geometry], 0
    je fail
    STEP 25
    PUSHI hd
    push 0
    push 0
    push 0
    PUSHI hdname
    call dword ptr [ObReferenceObjectByName]
    test eax, eax
    jnz fail
    push 0
    PUSHI returned
    push 24
    PUSHI geometry
    push 0
    push 0
    push dword ptr [hd]
    push 0x70000                        /* IOCTL_DISK_GET_DRIVE_GEOMETRY */
    call dword ptr [IoSynchronousDeviceIoControlRequest]
    test eax, eax
    jnz fail
    cmp dword ptr [geometry + 20], 512
    jne fail
    STEP 26
    push 0
    PUSHI returned
    push 24
    PUSHI geometry
    push 0
    push 0
    push dword ptr [cd]
    push 0x12345678
    call dword ptr [IoSynchronousDeviceIoControlRequest]
    cmp eax, 0xC0000010
    jne fail

    /* 11. priority boosts: a thread woken by KeSetEvent with increment 1 at
     * its waker's priority runs before the waker goes on, its priority one
     * up and its base kept; with increment 0, or its boosts disabled, it
     * waits for its turn */
    STEP 27
    PUSHI bev1
    call start_waiter
    push 0
    push 1
    PUSHI bev1
    call dword ptr [KeSetEvent]
    cmp dword ptr [bev1_ran], 1         /* ran already: preempted the waker */
    jne fail
    STEP 28
    mov eax, dword ptr [bev1_thread]
    cmp byte ptr [eax + 0x32], 9        /* KTHREAD.Priority: 8 + 1 */
    jne fail
    push eax
    call dword ptr [KeQueryBasePriorityThread]
    test eax, eax                       /* the base's increment: still 0 */
    jnz fail
    STEP 29
    PUSHI bev2
    call start_waiter
    push 0
    push 0
    PUSHI bev2
    call dword ptr [KeSetEvent]
    cmp dword ptr [bev2_ran], 0         /* no boost: the waker goes on */
    jne fail
    PUSHI delay
    push 0
    push 0
    call dword ptr [KeDelayExecutionThread]
    cmp dword ptr [bev2_ran], 1
    jne fail
    STEP 30
    PUSHI bev3
    call start_waiter
    push 1
    push dword ptr [bev3_thread]
    call dword ptr [KeSetDisableBoostThread]
    push 0
    push 1
    PUSHI bev3
    call dword ptr [KeSetEvent]
    cmp dword ptr [bev3_ran], 0         /* boosts disabled */
    jne fail
    PUSHI delay
    push 0
    push 0
    call dword ptr [KeDelayExecutionThread]
    cmp dword ptr [bev3_ran], 1
    jne fail

    /* 12. KeQuerySystemTime (guest code, KeSystemTime): a time after 2001,
     * and later after a 10 ms delay */
    STEP 31
    PUSHI time1
    call dword ptr [KeQuerySystemTime]
    cmp dword ptr [time1 + 4], 0x01C0739B /* 2001-01-01's high part */
    jb fail
    PUSHI delay
    push 0
    push 0
    call dword ptr [KeDelayExecutionThread]
    PUSHI time2
    call dword ptr [KeQuerySystemTime]
    mov eax, dword ptr [time2]
    mov edx, dword ptr [time2 + 4]
    sub eax, dword ptr [time1]
    sbb edx, dword ptr [time1 + 4]
    jnz fail                            /* not over 7 minutes */
    cmp eax, 90000                      /* at least 9 ms in 100 ns */
    jb fail

    PUSHI passed
    call dword ptr [DbgPrint]
    add esp, 4
    hlt
fail:
    push dword ptr [step]
    PUSHI failed
    call dword ptr [DbgPrint]
    add esp, 8
    hlt

/* An IRP of one stack location: a read, with the completion routine on
 * success, error and cancel, context 0x55. */
new_irp:
    push 1
    call dword ptr [IoAllocateIrp]
    test eax, eax
    jz fail
    mov ecx, dword ptr [eax + 0x5C]
    sub ecx, 0x24
    mov byte ptr [ecx], 2
    mov dword ptr [ecx + 0x1C], offset completion
    mov dword ptr [ecx + 0x20], 0x55
    mov byte ptr [ecx + 3], 0xE0
    ret

/* start_waiter(block): a new thread that waits on the synchronization event
 * at block (KEVENT, then the thread's KTHREAD, then 1 once it ran), and the
 * caller sleeps until it waits */
start_waiter:
    mov eax, dword ptr [esp + 4]
    push 0
    push 1
    push eax
    call dword ptr [KeInitializeEvent]
    push 0
    push dword ptr [esp + 8]
    PUSHI waiter
    push 0
    PUSHI hthread
    call dword ptr [PsCreateSystemThread]
    test eax, eax
    jnz fail
    PUSHI delay
    push 0
    push 0
    call dword ptr [KeDelayExecutionThread]
    ret 4

waiter:                               /* waiter(block) */
    mov eax, dword ptr [esp + 4]
    mov ecx, dword ptr fs:[0x28]
    mov dword ptr [eax + 0x10], ecx
    push 0
    push 0
    push 0
    push 0
    push eax
    call dword ptr [KeWaitForSingleObject]
    mov eax, dword ptr [esp + 4]
    mov dword ptr [eax + 0x14], 1
    ret 4

worker:                               /* worker(queue) */
    PUSHI e3
    push dword ptr [esp + 8]
    call dword ptr [KeInsertQueue]
    ret 4

port_worker:                          /* port_worker(&hport) */
    mov eax, dword ptr [esp + 4]
    push 0
    push 0
    push 0x66
    push 0x55
    push dword ptr [eax]
    call dword ptr [NtSetIoCompletion]
    ret 4

idle_thread:
    mov dword ptr [idle_ran], 1
    ret 4

dispatch:                             /* dispatch(dev, irp): pending, to StartIo */
    mov eax, dword ptr [esp + 8]
    mov ecx, dword ptr [eax + 0x5C]
    or byte ptr [ecx + 3], 1
    push 0
    push eax
    push dword ptr [esp + 12]
    call dword ptr [IoStartPacket]
    mov eax, 0x103
    ret 8

startio:                              /* startio(dev, irp) */
    inc dword ptr [startio_count]
    mov eax, dword ptr [esp + 8]
    mov dword ptr [last_irp], eax
    cmp dword ptr [hold], 0
    jne 1f
    mov dword ptr [eax + 0x10], 0
    mov dword ptr [eax + 0x14], 7
    mov ecx, eax
    xor edx, edx
    call dword ptr [IofCompleteRequest]
    push dword ptr [esp + 4]
    call dword ptr [IoStartNextPacket]
1:  ret 8

completion:                           /* completion(dev, irp, context) */
    inc dword ptr [comp_count]
    mov eax, dword ptr [esp + 4]
    mov dword ptr [comp_dev], eax
    mov eax, dword ptr [esp + 12]
    mov dword ptr [comp_ctx], eax
    mov eax, dword ptr [esp + 8]
    movzx ecx, byte ptr [eax + 0x1A]
    mov dword ptr [comp_pending], ecx
    mov ecx, dword ptr [eax + 0x14]
    mov dword ptr [comp_info], ecx
    mov eax, 0xC0000016
    ret 12

comp2:
    inc dword ptr [comp2_count]
    mov eax, dword ptr [esp + 8]
    mov ecx, dword ptr [eax + 0x10]
    mov dword ptr [comp2_status], ecx
    xor eax, eax
    ret 12

notify:                               /* notify(thread, id, create) */
    cmp byte ptr [esp + 12], 0
    je 1f
    inc dword ptr [notify_create]
    ret 12
1:  inc dword ptr [notify_exit]
    ret 12

/* data, in the same (writable) section */
.align 8
step: .long 0
zero64: .quad 0
delay: .quad -100000                  /* 10 ms */
off32: .quad 0x10000                  /* sector 32: the volume descriptor */
queue: .space 0x28
e1: .space 8
e2: .space 8
e3: .space 8
hthread: .long 0
hport: .long 0
depth: .long 0
key: .long 0
apcctx: .long 0
iosb: .quad 0
dev: .long 0
irpA: .long 0
irpB: .long 0
hold: .long 0
startio_count: .long 0
last_irp: .long 0
comp_count: .long 0
comp_ctx: .long 0
comp_dev: .long 0
comp_pending: .long 0
comp_info: .long 0
comp2_count: .long 0
comp2_status: .long 0
notify_create: .long 0
notify_exit: .long 0
idle_ran: .long 0
cd: .long 0
ev: .space 0x10
driver:                               /* StartIo, DeleteDevice, Dismount, MajorFunction[14] */
    .long startio, 0, 0
    .long 0, 0, dispatch, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
cdname: .word 14, 15
    .long cdstr
cdstr: .asciz "\\Device\\CdRom0"
hd: .long 0
time1: .quad 0
time2: .quad 0
/* start_waiter's blocks: KEVENT, KTHREAD, ran */
bev1: .space 0x10
bev1_thread: .long 0
bev1_ran: .long 0
bev2: .space 0x10
bev2_thread: .long 0
bev2_ran: .long 0
bev3: .space 0x10
bev3_thread: .long 0
bev3_ran: .long 0
returned: .long 0
geometry: .space 24
hdname: .word 28, 29
    .long hdstr
hdstr: .asciz "\\Device\\Harddisk0\\Partition3"
fmt: .asciz "%d-%s"
ok: .asciz "ok"
passed: .asciz "k85: all passed"
failed: .asciz "k85: failed at step %d"
.align 16
buf: .space 2048
buf2: .space 2048
