/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Failed-commit regression test, run by kernel_mm.py.
 * Repeat ten times: reject 256 MiB, then commit, write, and release 16 MiB.
 * Leaked pages block the smaller commit; leaked reservations exhaust VA space.
 * Code is at 0x11000; thunks at 0x10f00 follow kernel_mm.py's IMPORTS order.
 * Print the result and halt. */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,                THUNK + 4*0
.set NtAllocateVirtualMemory, THUNK + 4*1
.set NtFreeVirtualMemory,     THUNK + 4*2

.set MEM_COMMIT,     0x1000
.set MEM_RELEASE,    0x8000
.set PAGE_READWRITE, 0x04

/* push imm32 (LLVM's Intel syntax makes "push offset sym" a 16-bit push) */
.macro PUSHI sym
    .byte 0x68
    .long \sym
.endm

.macro STEP n
    mov dword ptr [step], \n
.endm

/* NtAllocateVirtualMemory(&base = 0, 0, &size = bytes, MEM_COMMIT, PAGE_READWRITE) */
.macro COMMIT bytes
    mov dword ptr [base], 0
    mov dword ptr [size], \bytes
    push PAGE_READWRITE
    push MEM_COMMIT
    PUSHI size
    push 0
    PUSHI base
    call dword ptr [NtAllocateVirtualMemory]
.endm

.text
.globl _start
_start:
    mov dword ptr [round], 0
again:
    /* 1. Reject a commit larger than physical RAM. */
    STEP 1
    COMMIT 0x10000000
    cmp eax, 0xC0000017                 /* STATUS_NO_MEMORY */
    jne fail

    /* 2. A smaller commit must succeed. */
    STEP 2
    COMMIT 0x1000000
    test eax, eax
    jnz fail
    mov edi, dword ptr [base]
    test edi, edi
    jz fail
    cmp dword ptr [size], 0x1000000
    jne fail

    /* 3. Verify writes to the first and last pages. */
    STEP 3
    mov dword ptr [edi], 0x12345678
    mov dword ptr [edi + 0xfffffc], 0x9abcdef0
    cmp dword ptr [edi], 0x12345678
    jne fail
    cmp dword ptr [edi + 0xfffffc], 0x9abcdef0
    jne fail

    /* 4. Release the entire reservation. */
    STEP 4
    mov dword ptr [size], 0
    push MEM_RELEASE
    PUSHI size
    PUSHI base
    call dword ptr [NtFreeVirtualMemory]
    test eax, eax
    jnz fail

    inc dword ptr [round]
    cmp dword ptr [round], 10
    jb again

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

.align 4
step: .long 0
round: .long 0
base: .long 0
size: .long 0
passed: .asciz "kmm: all passed"
failed: .asciz "kmm: failed at step %d"
