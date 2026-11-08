/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Boot memory regression test (kernel_boot_mem.py, 64 MiB RAM).
 * Check the boot reservation, copied window PDEs without a flush,
 * alias removal after a flush, zero padding above the stack, and more
 * than 4,096 contiguous blocks held at once.
 * Code: 0x11000; thunks: 0x10f00, in IMPORTS order. Print and halt. */
.intel_syntax noprefix
.set THUNK, 0x10f00
.set DbgPrint,                   THUNK + 4*0
.set MmAllocateContiguousMemory, THUNK + 4*1

.set TOP_HELD,   0x83FE0000     /* 0x80000000 + 64 MiB - 128 KiB */
.set ALIAS,      0x08000000     /* 0x88000000 - 0x80000000 */
.set PDE_WINDOW, 0x800          /* PDE 0x200 (0x80000000), bytes into the PD */
.set PDE_ALIAS,  0x880          /* PDE 0x220 (0x88000000) */
.set FX_SAVE,    0x210

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
    /* 1. The top of RAM is held from boot. */
    STEP 1
    push 0x20000
    call dword ptr [MmAllocateContiguousMemory]
    test eax, eax
    jz fail
    mov dword ptr [block], eax
    lea edx, [eax + 0x20000]
    cmp edx, TOP_HELD
    ja fail
    mov dword ptr [edx], 0x4655424c     /* "LBUF" just past the block */

    /* 2. The window's PDEs, copied without a flush, are a view of RAM. */
    STEP 2
    mov eax, cr3
    and eax, 0xfffff000
    or eax, 0x80000000
    mov dword ptr [pd], eax
    mov ecx, dword ptr [eax + PDE_WINDOW]
    and ecx, 0x81
    cmp ecx, 0x81                       /* present, 4 MiB */
    jne fail
    xor ecx, ecx
copy:
    mov edx, dword ptr [eax + PDE_WINDOW + ecx*4]
    and edx, 0xffffffef
    mov dword ptr [eax + PDE_ALIAS + ecx*4], edx
    inc ecx
    cmp ecx, 16
    jb copy

    STEP 3
    mov edi, dword ptr [block]
    mov dword ptr [edi + ALIAS], 0x12345678
    cmp dword ptr [edi], 0x12345678
    jne fail
    mov dword ptr [edi + 0x1ffc], 0x9abcdef0
    cmp dword ptr [edi + ALIAS + 0x1ffc], 0x9abcdef0
    jne fail

    /* 4. Cleared and flushed, the view goes (and nothing else does). */
    STEP 4
    mov eax, dword ptr [pd]
    xor ecx, ecx
clear:
    mov dword ptr [eax + PDE_ALIAS + ecx*4], 0
    inc ecx
    cmp ecx, 16
    jb clear
    mov eax, cr3
    mov cr3, eax
    cmp dword ptr [edi], 0x12345678
    jne fail

    /* 5. The page over the stack's top reads zero. */
    STEP 5
    mov eax, dword ptr fs:[4]           /* TIB StackBase, below the FX save area */
    cmp dword ptr [eax + FX_SAVE + 0x80], 0
    jne fail

    /* 6. 5,000 4 KiB blocks held at once (Wings of War holds 4,077 and
     * asks for more). */
    STEP 6
    mov dword ptr [count], 0
blocks:
    push 0x1000
    call dword ptr [MmAllocateContiguousMemory]
    test eax, eax
    jz fail
    inc dword ptr [count]
    cmp dword ptr [count], 5000
    jb blocks

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
block: .long 0
pd: .long 0
count: .long 0
passed: .asciz "kbm: all passed"
failed: .asciz "kbm: failed at step %d"
