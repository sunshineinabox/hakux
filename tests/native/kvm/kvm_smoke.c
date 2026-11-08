/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The KVM backend's first milestone (#56): each thing the backend does,
 * with guest code written for it and a system-op callback that records
 * what came out. Needs /dev/kvm (an x86-64 Linux host; CI's runner).
 *
 * - privilege: CPL 3, IOPL 3, IF; SIDT, SGDT and SMSW answered as FEX does;
 * - every exit a title makes, trapped and resumed (INT, INT3, UD2, WBINVD,
 *   RDMSR, MOV from CR0, IN, OUT, MMIO, a page fault, HLT);
 * - a thread switch inside a trapped kernel call, registers and SSE state
 *   each thread's own;
 * - an interrupt delivered at IF=1, and held through CLI until STI;
 * - RDTSC at 733 MHz, and continuous across a pause the guest leaves out;
 * - a hooked address, its system op and the title's instruction after it;
 * - a 16-byte MMIO store, which KVM splits; x87 state through a savestate's
 *   export and import mid-calculation; the kick signal's handler given back.
 */
#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/hle/hle_native.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CODE  0x10000u
#define DATA  0x20000u
#define FLAG  (DATA + 0x200)
#define LATE  0x30000u /* unmapped until the guest touches it */
#define IDT   0x8001F000u
#define MMIO  0xFE000000u
#define HOOK  (CODE + 0x805)

static NcCore core;
static NcCpu *cpu;
static unsigned failures;

static uint32_t reports[16];
static unsigned nreports;
static struct {
    enum NcSysOp op;
    unsigned size;
    uint32_t eip;
} ops[64];
static unsigned nops;
static NcCpuContext ctx[2];
static int cur;
static int irqs, hooks, late_faults;
static uint32_t irq_eip, irq_ecx;
static uint16_t out_port;
static uint32_t out_value;
static uint64_t mmio_reg;
static struct {
    uint32_t off;
    uint64_t value;
    unsigned size;
} mmio_writes[8];
static unsigned nmmio_writes;
static int (*core_fault)(void *, NcFrame *, uint32_t, bool, const char **);

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

static bool system_op(void *opaque, NcFrame *f, enum NcSysOp op, unsigned size)
{
    const uint32_t eip = nc_frame_eip(f);
    if (nops < 64) {
        ops[nops].op = op;
        ops[nops].size = size;
        ops[nops].eip = eip;
        nops++;
    }
    switch (op) {
    case NC_SYSOP_INT: {
        const uint8_t v = *(const uint8_t *)nc_vm_host(&core.vm, eip + 1);
        if (v == 0x41) {
            /* a kernel call that switches threads: this one resumes past it */
            nc_frame_set_eip(f, eip + size);
            nc_frame_save(f, &ctx[cur]);
            cur ^= 1;
            nc_frame_load(f, &ctx[cur]);
            return true;
        }
        if (v == 0x42 && nreports < 16) {
            reports[nreports++] = nc_frame_gpr(f, NC_EAX);
        } else if (v == 0x43) {
            /* 100 ms the guest's TSC leaves out, as a pause's */
            const uint64_t t0 = nc_cpu_host_cycles();
            usleep(100000);
            nc_cpu_cycle_offset_add(cpu, nc_cpu_host_cycles() - t0);
        } else if (v == 0x44) {
            usleep(50000); /* 50 ms it counts */
        } else if (v == 0x45) {
            /* the thread through a savestate's export and import */
            NcCpuContext saved, back;
            NcCpuState st;
            nc_frame_set_eip(f, eip + size);
            nc_frame_save(f, &saved);
            nc_context_export(&saved, &st);
            nc_context_import(&back, &st);
            nc_frame_load(f, &back);
            return true;
        }
        nc_frame_set_eip(f, eip + size);
        return true;
    }
    case NC_SYSOP_RDMSR:
        nc_frame_set_gpr(f, NC_EAX, 0x5A5A);
        nc_frame_set_gpr(f, NC_EDX, 0);
        nc_frame_set_eip(f, eip + size);
        return true;
    case NC_SYSOP_MOVCR:
        nc_frame_set_gpr(f, NC_EAX, 0x8001003B);
        nc_frame_set_eip(f, eip + size);
        return true;
    case NC_SYSOP_INT3:
    case NC_SYSOP_UD2:
    case NC_SYSOP_WBINVD:
        nc_frame_set_eip(f, eip + size);
        return true;
    case NC_SYSOP_INTERRUPT:
        irqs++;
        irq_eip = eip;
        irq_ecx = nc_frame_gpr(f, NC_ECX);
        nc_wr32(nc_vm_host(&core.vm, FLAG), 1);
        nc_frame_clear_interrupt(f);
        return true;
    case NC_SYSOP_HOOK:
        hooks++;
        nc_frame_bypass_hook(f);
        return true;
    default:
        return false;
    }
}

static uint32_t port_in(uint16_t port, unsigned bytes)
{
    return port == 0x1234 && bytes == 4 ? 0xCAFEF00Du : 0xFFFFFFFFu;
}

static void port_out(uint16_t port, unsigned bytes, uint32_t value)
{
    out_port = port;
    out_value = value;
}

static uint64_t mmio_read(void *opaque, uint32_t off, unsigned size)
{
    return mmio_reg;
}

static void mmio_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    mmio_reg = value;
    if (nmmio_writes < 8) {
        mmio_writes[nmmio_writes].off = off;
        mmio_writes[nmmio_writes].value = value;
        mmio_writes[nmmio_writes].size = size;
        nmmio_writes++;
    }
}

/* A page the guest touches first maps on its fault; the rest is the core's. */
static int fault(void *opaque, NcFrame *f, uint32_t va, bool write, const char **why)
{
    if (va - LATE < NC_PAGE) {
        late_faults++;
        return nc_vm_alloc_at(&core.vm, LATE, NC_PAGE, NULL) ? 1 : 0;
    }
    return core_fault(opaque, f, va, write, why);
}

static unsigned hook_list(void *opaque, uint32_t *eips, unsigned max)
{
    eips[0] = HOOK;
    return max ? 1 : 0;
}

static void *interrupt_later(void *arg)
{
    usleep((useconds_t)(uintptr_t)arg);
    nc_cpu_request_interrupt(cpu);
    return NULL;
}

/* Where an instruction's bytes first are in a program (its offset). */
static uint32_t offset_of(const uint8_t *code, size_t n, const uint8_t *insn, size_t m)
{
    for (size_t i = 0; i + m <= n; i++) {
        if (!memcmp(code + i, insn, m)) {
            return (uint32_t)i;
        }
    }
    return UINT32_MAX;
}

static void put(uint32_t at, const uint8_t *code, size_t n)
{
    memcpy(nc_vm_host(&core.vm, at), code, n);
}

static void run_at(uint32_t eip)
{
    NcCpuContext c;
    nc_context_init(cpu, &c, eip, DATA + 0xF00);
    nc_cpu_run(cpu, &c);
}

static bool saw(enum NcSysOp op, unsigned size, uint32_t eip)
{
    for (unsigned i = 0; i < nops; i++) {
        if (ops[i].op == op && ops[i].size == size && ops[i].eip == eip) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!nc_vm_init(&core.vm, 64u << 20) || !nc_kernel_init(&core.kernel, &core.vm) ||
        !nc_vm_alloc_at(&core.vm, CODE, 4 * NC_PAGE, NULL) || !nc_vm_alloc_at(&core.vm, DATA, NC_PAGE, NULL)) {
        printf("FAIL: guest memory\n");
        return 1;
    }
    NcMmioDev dev = {"kvm-smoke", MMIO, NC_PAGE, mmio_read, mmio_write, NULL};
    nc_mmio_register(&dev);
    NcCpuConfig cfg = {.membase = core.vm.base, .sysop = system_op, .idt_base = IDT, .idt_limit = 0x7FF};
    nc_core_cpu_backend(&core, &cfg);
    core_fault = cfg.fault;
    cfg.fault = fault;
    cfg.port_in = port_in;
    cfg.port_out = port_out;
    cfg.hook_list = hook_list;
    cpu = core.cpu = nc_cpu_create(&cfg, core.kernel.kpcr_va);
    if (!cpu) {
        printf("FAIL: a KVM guest CPU (needs /dev/kvm with TSC scaling and UMIP)\n");
        return 1;
    }
    printf("PASS: a KVM guest CPU (%s)\n", nc_cpu_backend);

    /* Privilege, UMIP and each exit, in one run. */
    const uint8_t exits[] = {
        0x9C, 0x58, 0xCD, 0x42,                         /* pushfd; pop eax; int 42h      (1) */
        0x8C, 0xC8, 0xCD, 0x42,                         /* mov eax, cs; int 42h          (2) */
        0x0F, 0x01, 0x0D, 0x00, 0x01, 0x02, 0x00,       /* sidt [0x20100] */
        0xA1, 0x02, 0x01, 0x02, 0x00, 0xCD, 0x42,       /* mov eax, [0x20102]; int 42h   (3) */
        0x0F, 0x01, 0x05, 0x10, 0x01, 0x02, 0x00,       /* sgdt [0x20110] */
        0xA1, 0x12, 0x01, 0x02, 0x00, 0xCD, 0x42,       /* mov eax, [0x20112]; int 42h   (4) */
        0x0F, 0x01, 0xE0, 0xCD, 0x42,                   /* smsw eax; int 42h             (5) */
        0xCC,                                           /* int3 */
        0x0F, 0x0B,                                     /* ud2 */
        0x0F, 0x09,                                     /* wbinvd */
        0xB9, 0x1B, 0x00, 0x00, 0x00, 0x0F, 0x32,       /* mov ecx, 1Bh; rdmsr */
        0xCD, 0x42,                                     /* int 42h                       (6) */
        0x0F, 0x20, 0xC0, 0xCD, 0x42,                   /* mov eax, cr0; int 42h         (7) */
        0xBA, 0x34, 0x12, 0x00, 0x00, 0xED, 0xCD, 0x42, /* mov edx, 1234h; in eax, dx; int 42h (8) */
        0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0xEF,             /* mov eax, DEADBEEFh; out dx, eax */
        0xA3, 0x00, 0x00, 0x00, 0xFE,                   /* mov [0xFE000000], eax */
        0x31, 0xC0,                                     /* xor eax, eax */
        0xA1, 0x00, 0x00, 0x00, 0xFE, 0xCD, 0x42,       /* mov eax, [0xFE000000]; int 42h (9) */
        0xA1, 0x00, 0x00, 0x03, 0x00, 0xCD, 0x42,       /* mov eax, [0x30000]; int 42h   (10) */
        0xF4,                                           /* hlt */
    };
    put(CODE, exits, sizeof(exits));
    run_at(CODE);
    check(nreports == 10, "ten reports from the exits run");
    check((reports[0] & 0x3000) == 0x3000 && (reports[0] & 0x200), "EFLAGS: IOPL 3, IF set");
    check((reports[1] & 3) == 3, "CS: CPL 3");
    check(reports[2] == IDT && nc_rd16(nc_vm_host(&core.vm, DATA + 0x100)) == 0x7FF, "SIDT: the kernel's IDT");
    check(reports[3] == 0xFFFE0000u && nc_rd16(nc_vm_host(&core.vm, DATA + 0x110)) == 0, "SGDT: FEX's answer");
    check(reports[4] == 0x80050033u, "SMSW: FEX's CR0");
#define AT(...) (CODE + offset_of(exits, sizeof(exits), (const uint8_t[]){__VA_ARGS__}, \
                                  sizeof((const uint8_t[]){__VA_ARGS__})))
    check(saw(NC_SYSOP_INT3, 1, AT(0xCC)), "INT3: system op, size 1");
    check(saw(NC_SYSOP_UD2, 2, AT(0x0F, 0x0B)), "UD2: system op, size 2");
    check(saw(NC_SYSOP_WBINVD, 2, AT(0x0F, 0x09)), "WBINVD: system op, size 2");
    check(saw(NC_SYSOP_RDMSR, 2, AT(0x0F, 0x32)) && reports[5] == 0x5A5A, "RDMSR: system op, result in EAX");
    check(saw(NC_SYSOP_MOVCR, 3, AT(0x0F, 0x20, 0xC0)) && reports[6] == 0x8001003B, "MOV EAX, CR0: system op, size 3");
    for (unsigned i = 0; i < nops && failures; i++) {
        printf("  system op %d, size %u, at %08x\n", (int)ops[i].op, ops[i].size, ops[i].eip);
    }
    check(reports[7] == 0xCAFEF00Du, "IN: the port's value");
    check(out_port == 0x1234 && out_value == 0xDEADBEEFu, "OUT: the port and value");
    check(reports[8] == 0xDEADBEEFu && mmio_reg == 0xDEADBEEFu, "MMIO: a write, then a read of it");
    check(reports[9] == 0 && late_faults == 1, "a page fault: mapped by the fault path, the access run again");

    /* MMX and SSE registers loaded while the vCPU's saved FPU state is in
     * its initial configuration (no guest code above used them): through
     * KVM_SET_FPU they were dropped at the next entry, the XSAVE header
     * saying x87 and SSE were initial, and the guest read zeros (Burnout 3's
     * XMV decoder lost its alpha mask so after a thread switch). */
    const uint8_t fpregs[] = {
        0x0F, 0x7E, 0xF8, 0xCD, 0x42,       /* movd eax, mm7; int 42h */
        0x66, 0x0F, 0x7E, 0xC8, 0xCD, 0x42, /* movd eax, xmm1; int 42h */
        0x0F, 0x77, 0xF4,                   /* emms; hlt */
    };
    put(CODE + 0xA00, fpregs, sizeof(fpregs));
    {
        NcCpuContext c;
        NcCpuState st;
        nc_context_init(cpu, &c, CODE + 0xA00, DATA + 0xF00);
        nc_context_export(&c, &st);
        const uint32_t mm7 = 0x77777777u, xmm1 = 0x11223344u;
        memcpy(st.mm[7], &mm7, 4);
        memcpy(st.xmm[1], &xmm1, 4);
        nc_context_import(&c, &st);
        nreports = 0;
        nc_cpu_run(cpu, &c);
    }
    check(nreports == 2 && reports[0] == 0x77777777u && reports[1] == 0x11223344u,
          "MMX and SSE registers set over an initial FPU state: the guest reads them");

    /* A thread switch inside a trapped kernel call (int 41h). */
    const uint8_t thread_a[] = {
        0xBB, 0xAA, 0xAA, 0x00, 0x00,       /* mov ebx, 0AAAAh */
        0xB8, 0x11, 0x11, 0x11, 0x11,       /* mov eax, 11111111h */
        0x66, 0x0F, 0x6E, 0xC0,             /* movd xmm0, eax */
        0xCD, 0x41,                         /* int 41h: to B */
        0x83, 0xC3, 0x01, 0x89, 0xD8,       /* add ebx, 1; mov eax, ebx */
        0xCD, 0x42,                         /* int 42h */
        0x66, 0x0F, 0x7E, 0xC0, 0xCD, 0x42, /* movd eax, xmm0; int 42h */
        0xCD, 0x41,                         /* int 41h: to B */
        0xF4,                               /* hlt */
    };
    uint8_t thread_b[sizeof(thread_a)];
    memcpy(thread_b, thread_a, sizeof(thread_a));
    thread_b[1] = thread_b[2] = 0xBB;
    memset(thread_b + 6, 0x22, 4);
    put(CODE + 0x400, thread_a, sizeof(thread_a));
    put(CODE + 0x480, thread_b, sizeof(thread_b));
    nc_context_init(cpu, &ctx[0], CODE + 0x400, DATA + 0xF00);
    nc_context_init(cpu, &ctx[1], CODE + 0x480, DATA + 0x800);
    nreports = 0;
    cur = 0;
    nc_cpu_run(cpu, &ctx[0]);
    check(nreports == 4 && reports[0] == 0xAAAB && reports[2] == 0xBBBC, "thread switch: each its own EBX and EIP");
    check(reports[1] == 0x11111111u && reports[3] == 0x22222222u, "thread switch: each its own XMM0");

    /* An interrupt at IF=1: the guest spins until the handler sets the flag. */
    const uint8_t spin[] = {
        0xA1, 0x00, 0x02, 0x02, 0x00, /* L: mov eax, [FLAG] */
        0x85, 0xC0, 0x74, 0xF7,       /* test eax, eax; jz L */
        0xCD, 0x42, 0xF4,             /* int 42h; hlt */
    };
    put(CODE + 0x600, spin, sizeof(spin));
    nc_wr32(nc_vm_host(&core.vm, FLAG), 0);
    pthread_t t;
    pthread_create(&t, NULL, interrupt_later, (void *)(uintptr_t)20000);
    irqs = 0;
    run_at(CODE + 0x600);
    pthread_join(t, NULL);
    check(irqs == 1 && irq_eip >= CODE + 0x600 && irq_eip < CODE + 0x609, "interrupt at IF=1: taken in the loop");

    /* At IF=0 it waits for STI: requested 10 ms into a CLI loop of ~100 ms. */
    const uint8_t held[] = {
        0xFA,                         /* cli */
        0xB9, 0x00, 0x00, 0x00, 0x10, /* mov ecx, 10000000h */
        0x49, 0x75, 0xFD,             /* L: dec ecx; jnz L */
        0xFB, 0x90, 0x90,             /* sti; nop; nop */
        0xCD, 0x42, 0xF4,             /* int 42h; hlt */
    };
    put(CODE + 0x680, held, sizeof(held));
    pthread_create(&t, NULL, interrupt_later, (void *)(uintptr_t)10000);
    irqs = 0;
    run_at(CODE + 0x680);
    pthread_join(t, NULL);
    check(irqs == 1 && irq_ecx == 0 && irq_eip > CODE + 0x689, "interrupt at IF=0: held until after STI");

    /* RDTSC: 733 MHz, and a pause left out. */
    const uint8_t tsc[] = {
        0x0F, 0x31, 0x89, 0xC3, 0x89, 0xD6, /* rdtsc; mov ebx, eax; mov esi, edx */
        0xCD, 0x43,                         /* int 43h: a 100 ms pause */
        0x0F, 0x31, 0x29, 0xD8, 0x19, 0xF2, /* rdtsc; sub eax, ebx; sbb edx, esi */
        0xCD, 0x42,                         /* int 42h */
        0x0F, 0x31, 0x89, 0xC3, 0x89, 0xD6,
        0xCD, 0x44,                         /* int 44h: 50 ms counted */
        0x0F, 0x31, 0x29, 0xD8, 0x19, 0xF2,
        0xCD, 0x42, 0xF4,
    };
    put(CODE + 0x700, tsc, sizeof(tsc));
    nreports = 0;
    run_at(CODE + 0x700);
    printf("  TSC: %u ticks across the pause, %u across 50 ms\n", reports[0], reports[1]);
    check(nreports == 2 && reports[0] < 7333333u, "RDTSC: a 100 ms pause left out (under 10 ms of ticks)");
    check(reports[1] > 33000000u && reports[1] < 45000000u, "RDTSC: 733 MHz (50 ms ~ 36.7 M ticks)");

    /* A hooked address, twice through a loop. */
    const uint8_t hooked[] = {
        0xB9, 0x02, 0x00, 0x00, 0x00, /* mov ecx, 2 */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* L: mov eax, 5  (hooked: CODE + 0x805) */
        0x01, 0xC2, 0x49, 0x75, 0xF6, /* add edx, eax; dec ecx; jnz L */
        0x89, 0xD0, 0xCD, 0x42, 0xF4, /* mov eax, edx; int 42h; hlt */
    };
    put(CODE + 0x800, hooked, sizeof(hooked));
    nreports = 0;
    hooks = 0;
    nc_cpu_invalidate(cpu, 0, 0xFFFFF000u); /* the hook list read again, patched */
    nops = 0;
    run_at(CODE + 0x800);
    printf("  hook: %d hits, %u reports, the first %u\n", hooks, nreports, nreports ? reports[0] : 0);
    for (unsigned i = 0; i < nops; i++) {
        printf("  system op %d, size %u, at %08x\n", (int)ops[i].op, ops[i].size, ops[i].eip);
    }
    check(hooks == 2 && nreports == 1 && reports[0] == 10, "hook: its system op each time, the instruction run");

    /* A 16-byte SSE store to a device: KVM hands it over as two 8-byte parts. */
    const uint8_t wide[] = {
        0x0F, 0x10, 0x0D, 0x00, 0x03, 0x02, 0x00, /* movups xmm1, [0x20300] */
        0x0F, 0x11, 0x0D, 0x10, 0x00, 0x00, 0xFE, /* movups [0xFE000010], xmm1 */
        0xCD, 0x42, 0xF4,
    };
    const uint64_t lo = 0x0123456789ABCDEFull, hi = 0xFEDCBA9876543210ull;
    memcpy(nc_vm_host(&core.vm, DATA + 0x300), &lo, 8);
    memcpy(nc_vm_host(&core.vm, DATA + 0x308), &hi, 8);
    put(CODE + 0x900, wide, sizeof(wide));
    nmmio_writes = 0;
    run_at(CODE + 0x900);
    uint64_t got_lo = 0, got_hi = 0;
    for (unsigned i = 0; i < nmmio_writes; i++) {
        printf("  MMIO write +%x: %016llx (%u bytes)\n", mmio_writes[i].off,
               (unsigned long long)mmio_writes[i].value, mmio_writes[i].size);
        if (mmio_writes[i].off == 0x10 && mmio_writes[i].size == 8) {
            got_lo = mmio_writes[i].value;
        } else if (mmio_writes[i].off == 0x18 && mmio_writes[i].size == 8) {
            got_hi = mmio_writes[i].value;
        }
    }
    check(got_lo == lo && got_hi == hi, "16-byte MMIO store: both halves reach the device");

    /* x87 mid-calculation through a savestate: fld1, then export and import
     * (int 45h), then fld1; faddp; fistp. TOP lost would make it garbage. */
    const uint8_t x87[] = {
        0xD9, 0xE8, 0xCD, 0x45,                   /* fld1; int 45h */
        0xD9, 0xE8, 0xDE, 0xC1,                   /* fld1; faddp st(1), st */
        0xDB, 0x1D, 0x20, 0x03, 0x02, 0x00,       /* fistp dword [0x20320] */
        0xA1, 0x20, 0x03, 0x02, 0x00, 0xCD, 0x42, /* mov eax, [0x20320]; int 42h */
        0xF4,
    };
    put(CODE + 0x980, x87, sizeof(x87));
    nreports = 0;
    run_at(CODE + 0x980);
    check(nreports == 1 && reports[0] == 2, "x87 state, TOP included, through a savestate's export and import");

    nc_cpu_destroy(cpu);
    struct sigaction kick;
    sigaction(SIGRTMIN + 3, NULL, &kick);
    check(kick.sa_handler == SIG_DFL, "the kick signal's handler given back on destroy");
    printf("%s: %u failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
