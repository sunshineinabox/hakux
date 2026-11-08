/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — the guest CPU on x86-64 Linux through KVM (#56).
 *
 * The title's code runs on the host CPU in a KVM guest, at CPL 3 with IOPL 3
 * so that cli, sti, pushfd, popfd, in and out behave as on the Xbox; every
 * other system instruction faults (#GP, UMIP's for SIDT and the like) into
 * a ring-0 stub, and the stub's HLT brings it here. The kernel sees what
 * FEX's backend gives it: system ops on a frame.
 *
 * Memory: guest RAM is one memslot over the host's view of it (vm->phys,
 * never protected); device windows have none, so their accesses exit as
 * KVM_EXIT_MMIO, decoded. The guest runs with paging on, on page tables
 * kept here as a mirror of vm.c's mappings and protections
 * (nc_vm_prot_hook): KVM can't tell a guest access to a page the host has
 * protected from any other failure. GDT, IDT, TSS and stubs are guest
 * pages too, supervisor-only, at 0x1000-0x6FFF, below anything a title
 * maps.
 *
 * An exit: device work first (port and MMIO handlers fill a read's data),
 * then the access completed (KVM_RUN with immediate_exit), and only then
 * the frame built and the kernel called, so nothing KVM completes lands on
 * a frame the kernel replaced (a thread switch). A stub's frame is the
 * title's: GPRs as the stub left them (it clobbers none), EIP, EFLAGS and
 * ESP from the exception frame on the ring-0 stack, written back there for
 * the stub's IRET.
 */
#include "native/cpu/cpu.h"
#include "native/core/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

const char nc_cpu_backend[] = "kvm";

/* Guest physical: RAM at 0, the backend's own pages at SYS_GPA. */
#define SYS_GPA   0x40000000u
#define SYS_PD0   0x000000u /* two identical page directories: CR3 moved between */
#define SYS_PD1   0x001000u /* them flushes the guest's TLB */
#define SYS_PT    0x002000u /* page table i: VAs i << 22 */
#define SYS_LOW   (SYS_PT + 1024u * NC_PAGE)
/* the guest's view of them: linear 0x1000.., supervisor-only */
#define LIN_GDT   0x1000u
#define LIN_IDT   0x2000u
#define LIN_TSS   0x3000u
#define LIN_STUBS 0x4000u
#define LIN_STACK 0x7000u /* top of two pages: ESP0 */
#define LIN_END   0x7000u
#define SYS_SIZE  (SYS_LOW + (LIN_END - LIN_GDT))

#define SEL_CODE0 0x08u
#define SEL_DATA0 0x10u
#define SEL_CODE3 0x1Bu
#define SEL_DATA3 0x23u
#define SEL_FS    0x2Bu
#define SEL_TSS   0x30u

#define PTE_P   0x001u
#define PTE_RW  0x002u
#define PTE_US  0x004u
#define PTE_PCD 0x010u

#define EFL_TF   0x100u
#define EFL_IF   0x200u
#define EFL_IOPL 0x3000u

#define GUEST_KHZ 733333u
#define STUB_HLT  8u /* stub v's HLT ends at LIN_STUBS + 16 v + STUB_HLT */
#define KICK_SIGNAL (SIGRTMIN + 3)

struct NcFrame {
    NcCpu *cpu;
    uint64_t regs[8]; /* x86 order (nc_frame_regs); ESP the title's */
    uint64_t rip;
    uint32_t eflags;
    bool fpu_valid, fpu_dirty;
    struct kvm_fpu fpu;
};

/* A guest thread's state while it isn't running (NcCpuContext's blob). */
typedef struct KvmContext {
    uint64_t regs[8];
    uint64_t rip;
    uint32_t eflags, pad;
    struct kvm_fpu fpu;
} KvmContext;
_Static_assert(sizeof(KvmContext) <= sizeof(NcCpuContext), "KvmContext outgrew NcCpuContext");

typedef struct Hook {
    uint32_t eip;
    uint8_t orig;
} Hook;
#define MAX_HOOKS 64

struct NcCpu {
    NcCpuConfig cfg;
    NcVm *vm;
    int kvm, vmfd, vcpu;
    struct kvm_run *run;
    size_t run_size;
    struct kvm_xsave *xsave; /* fpu_get / fpu_set's buffer; NULL: KVM_GET/SET_FPU */
    size_t xsave_size;
    bool xsave2;           /* KVM_GET_XSAVE2 reads it (larger than struct kvm_xsave) */
    uint8_t *sys;          /* host view of SYS_GPA */
    uint32_t fs_base;
    struct kvm_sregs sregs; /* the ring-3 state's, CR3 the current directory */
    bool pd1;              /* CR3 at SYS_PD1 */
    pthread_t thread;      /* nc_cpu_run's */
    bool running;
    volatile int irq;      /* nc_cpu_request_interrupt */
    volatile int flush;    /* a PTE lost access since the last entry */
    volatile int in_guest; /* from before the flush check to KVM_RUN's return */
    volatile uint64_t exits; /* KVM_RUN returns, counted at once */
    NcFrame frame;
    bool in_stub;          /* the frame is a stub exit's (ring-0 stack holds the title's EIP...) */
    uint32_t stub_esp;
    uint32_t last_eip;
    /* time */
    uint32_t host_khz;
    uint64_t paused;       /* host ticks left out of the guest's count */
    /* hooks: int3 over each, the title's byte kept */
    Hook hooks[MAX_HOOKS];
    unsigned nhooks;
    uint32_t stepping;     /* a hook's EIP run once with TF set (nc_frame_bypass_hook), 0 none */
};

static NcCpu *g_cpu; /* the one CPU, for nc_vm_prot_hook */
static struct sigaction g_prev_kick; /* the kick signal's handler before, given back */
static void (*g_prev_hook)(uint32_t va, uint32_t len, int prot); /* chained */

static inline uint64_t host_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void fatal(NcCpu *cpu, const char *what)
{
    struct kvm_regs r;
    memset(&r, 0, sizeof(r));
    ioctl(cpu->vcpu, KVM_GET_REGS, &r);
    nc_log("hakuX-native-kvm", "%s: eip %08llx esp %08llx eflags %08llx eax %08llx (%s)", what,
           (unsigned long long)r.rip, (unsigned long long)r.rsp, (unsigned long long)r.rflags,
           (unsigned long long)r.rax, strerror(errno));
    abort();
}

/* ------------------------------------------------------------ page tables */

static inline uint32_t *pt_entry(NcCpu *cpu, uint32_t va)
{
    return (uint32_t *)(cpu->sys + SYS_PT) + (va >> 12);
}

/* What vm.c has at va, as a PTE: its frame and protection; a device's
 * window (identity, no memslot: MMIO exits); the backend's pages. */
static uint32_t pte_for(NcCpu *cpu, uint32_t va)
{
    if (va >= LIN_GDT && va < LIN_END) {
        return (SYS_GPA + SYS_LOW + (va - LIN_GDT)) | PTE_P | PTE_RW;
    }
    if (va < 0x10000u) {
        return 0;
    }
    const uint32_t page = va >> 12;
    const uint32_t e = cpu->vm->pte[page];
    if (e & NC_PTE_VALID) {
        const uint8_t prot = cpu->vm->vprot[page];
        if (!(prot & PROT_READ)) {
            return 0;
        }
        return (e & ~0xFFFu) | PTE_P | PTE_US | ((prot & PROT_WRITE) ? PTE_RW : 0);
    }
    if (cpu->cfg.mmio_window && cpu->cfg.mmio_window(va & ~0xFFFu)) {
        return (va & ~0xFFFu) | PTE_P | PTE_RW | PTE_US | PTE_PCD;
    }
    return 0;
}

/* PTEs for [va, va + len) again; a lost permission makes the next entry
 * flush the TLB (a gained one needs none: the stale entry faults once and
 * the #PF path finds the PTE allows it). True if one was lost. */
static bool pte_update(NcCpu *cpu, uint32_t va, uint32_t len)
{
    bool any = false;
    for (uint64_t a = va & ~0xFFFu; a < (uint64_t)va + len; a += NC_PAGE) {
        uint32_t *p = pt_entry(cpu, (uint32_t)a);
        const uint32_t old = __atomic_load_n(p, __ATOMIC_RELAXED);
        const uint32_t now = pte_for(cpu, (uint32_t)a);
        if (old == now) {
            continue;
        }
        __atomic_store_n(p, now, __ATOMIC_RELEASE);
        const bool lost = (old & PTE_P) && (!(now & PTE_P) || ((old & PTE_RW) && !(now & PTE_RW)) ||
                                            ((old ^ now) & ~0xFFFu));
        if (lost) {
            __atomic_store_n(&cpu->flush, 1, __ATOMIC_SEQ_CST);
            any = true;
        }
    }
    return any;
}

/* A page lost access on another thread (the decode thread re-protecting
 * guest RAM) while the guest may be running: back only once the guest can
 * no longer use the old entry, as mprotect is. The vCPU flushes before
 * every entry (tlb_flush), so it is enough that it has been outside KVM_RUN
 * since the PTE changed: out of it now, or kicked out and counted. It never
 * waits on this thread (exits counts before any handler runs), so a vCPU in
 * a kernel call or a fault handler doesn't hold it up. */
static void shootdown(NcCpu *cpu)
{
    if (!__atomic_load_n(&cpu->in_guest, __ATOMIC_SEQ_CST)) {
        return; /* its next entry reads the flush */
    }
    const uint64_t seen = __atomic_load_n(&cpu->exits, __ATOMIC_SEQ_CST);
    __atomic_store_n(&cpu->run->immediate_exit, 1, __ATOMIC_SEQ_CST);
    pthread_kill(cpu->thread, KICK_SIGNAL);
    for (unsigned spins = 0; __atomic_load_n(&cpu->in_guest, __ATOMIC_SEQ_CST) &&
                             __atomic_load_n(&cpu->exits, __ATOMIC_SEQ_CST) == seen;
         spins++) {
        if (spins > 64) {
            sched_yield();
        }
    }
}

/* vm.c: a guest view's mapping or protection changed (under its lock), on
 * whichever thread made it. */
static void prot_hook(uint32_t va, uint32_t len, int prot)
{
    if (g_prev_hook) {
        g_prev_hook(va, len, prot);
    }
    NcCpu *cpu = g_cpu;
    if (!cpu) {
        return;
    }
    if (va < 0x10000u) {
        static unsigned warned;
        if (warned++ < 4) {
            nc_log("hakuX-native-kvm", "vm: a guest mapping at %08x, over the backend's own pages", va);
        }
    }
    if (pte_update(cpu, va, len) && __atomic_load_n(&cpu->running, __ATOMIC_ACQUIRE) &&
        !pthread_equal(pthread_self(), cpu->thread)) {
        shootdown(cpu);
    }
}

static void tlb_flush(NcCpu *cpu)
{
    if (!__atomic_exchange_n(&cpu->flush, 0, __ATOMIC_SEQ_CST)) {
        return;
    }
    struct kvm_sregs s;
    if (ioctl(cpu->vcpu, KVM_GET_SREGS, &s) < 0) {
        fatal(cpu, "KVM_GET_SREGS");
    }
    cpu->pd1 = !cpu->pd1;
    s.cr3 = SYS_GPA + (cpu->pd1 ? SYS_PD1 : SYS_PD0);
    if (ioctl(cpu->vcpu, KVM_SET_SREGS, &s) < 0) {
        fatal(cpu, "KVM_SET_SREGS (TLB flush)");
    }
    cpu->sregs.cr3 = s.cr3;
}

/* ------------------------------------------------------------ setup */

static uint64_t desc(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    return (uint64_t)(limit & 0xFFFFu) | ((uint64_t)(base & 0xFFFFFFu) << 16) | ((uint64_t)access << 40) |
           ((uint64_t)((limit >> 16) & 0xFu) << 48) | ((uint64_t)(flags & 0xFu) << 52) |
           ((uint64_t)(base >> 24) << 56);
}

static struct kvm_segment seg(uint16_t sel, uint32_t base, bool code, unsigned dpl)
{
    return (struct kvm_segment){.base = base, .limit = 0xFFFFFFFFu, .selector = sel, .type = code ? 0xB : 0x3,
                                .present = 1, .dpl = dpl, .db = 1, .s = 1, .g = 1};
}

static bool errcode_vector(unsigned v)
{
    return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30;
}

static uint8_t *lin(NcCpu *cpu, uint32_t l)
{
    return cpu->sys + SYS_LOW + (l - LIN_GDT);
}

static void build_tables(NcCpu *cpu)
{
    uint64_t *gdt = (uint64_t *)lin(cpu, LIN_GDT);
    gdt[SEL_CODE0 >> 3] = desc(0, 0xFFFFF, 0x9A, 0xC);
    gdt[SEL_DATA0 >> 3] = desc(0, 0xFFFFF, 0x92, 0xC);
    gdt[SEL_CODE3 >> 3] = desc(0, 0xFFFFF, 0xFA, 0xC);
    gdt[SEL_DATA3 >> 3] = desc(0, 0xFFFFF, 0xF2, 0xC);
    gdt[SEL_FS >> 3] = desc(cpu->fs_base, 0xFFFFF, 0xF2, 0xC);
    gdt[SEL_TSS >> 3] = desc(LIN_TSS, 0x67, 0x89, 0);

    uint8_t *tss = lin(cpu, LIN_TSS);
    nc_wr32(tss + 4, LIN_STACK);  /* ESP0 */
    nc_wr32(tss + 8, SEL_DATA0);  /* SS0 */
    nc_wr32(tss + 0x64, 0x68u << 16); /* no I/O bitmap: IOPL 3 allows every port */

    uint8_t *stubs = lin(cpu, LIN_STUBS);
    uint32_t *idt = (uint32_t *)lin(cpu, LIN_IDT);
    for (unsigned v = 0; v < 256; v++) {
        uint8_t *s = stubs + 16 * v;
        unsigned n = 0;
        if (!errcode_vector(v)) {
            s[n++] = 0x6A; /* push 0: a uniform frame */
            s[n++] = 0x00;
        }
        s[n++] = 0x68; /* push imm32 v */
        nc_wr32(s + n, v);
        n += 4;
        s[n++] = 0xF4; /* hlt: to the host */
        /* the HLT ends at STUB_HLT in every stub: NOPs before a shorter one */
        if (n < STUB_HLT) {
            memmove(s + STUB_HLT - n, s, n);
            memset(s, 0x90, STUB_HLT - n);
            n = STUB_HLT;
        }
        s[n++] = 0x83; /* add esp, 8 */
        s[n++] = 0xC4;
        s[n++] = 0x08;
        s[n++] = 0xCF; /* iret */
        const uint32_t off = LIN_STUBS + 16 * v;
        /* vectors with an error code take software INTs as #GP (DPL 0) */
        const unsigned dpl = errcode_vector(v) ? 0 : 3;
        idt[2 * v] = (SEL_CODE0 << 16) | (off & 0xFFFFu);
        idt[2 * v + 1] = (off & 0xFFFF0000u) | 0x8E00u | (dpl << 13);
    }

    uint32_t *pd0 = (uint32_t *)(cpu->sys + SYS_PD0), *pd1 = (uint32_t *)(cpu->sys + SYS_PD1);
    for (unsigned i = 0; i < 1024; i++) {
        pd0[i] = pd1[i] = (SYS_GPA + SYS_PT + i * NC_PAGE) | PTE_P | PTE_RW | PTE_US;
    }
    for (uint64_t va = 0; va < (1ull << 32); va += NC_PAGE) {
        *pt_entry(cpu, (uint32_t)va) = pte_for(cpu, (uint32_t)va);
    }
}

static bool set_cpuid(NcCpu *cpu)
{
    const unsigned max = 64;
    struct kvm_cpuid2 *c = calloc(1, sizeof(*c) + max * sizeof(struct kvm_cpuid_entry2));
    if (!c) {
        return false;
    }
    unsigned n = 0;
    for (uint32_t leaf = 0; leaf <= 0x1F; leaf++) {
        struct kvm_cpuid_entry2 *e = &c->entries[n++];
        uint32_t r[4] = {0};
        cpu->cfg.cpuid(leaf, 0, r);
        *e = (struct kvm_cpuid_entry2){.function = leaf, .eax = r[0], .ebx = r[1], .ecx = r[2], .edx = r[3]};
        if (leaf == 7) {
            /* the one difference from the Pentium III's: KVM takes CR4.UMIP only
             * from a guest that has it (leaf 0 still says 2 is the highest) */
            e->flags = KVM_CPUID_FLAG_SIGNIFCANT_INDEX;
            e->ecx |= 1u << 2;
        }
    }
    for (uint32_t leaf = 0x80000000u; leaf <= 0x80000008u; leaf++) {
        uint32_t r[4] = {0};
        cpu->cfg.cpuid(leaf, 0, r);
        c->entries[n++] = (struct kvm_cpuid_entry2){.function = leaf, .eax = r[0], .ebx = r[1], .ecx = r[2],
                                                     .edx = r[3]};
    }
    c->nent = n;
    const int ret = ioctl(cpu->vcpu, KVM_SET_CPUID2, c);
    free(c);
    return ret == 0;
}

static bool host_has_umip(int kvm)
{
    const unsigned max = 256;
    struct kvm_cpuid2 *c = calloc(1, sizeof(*c) + max * sizeof(struct kvm_cpuid_entry2));
    bool has = false;
    if (c) {
        c->nent = max;
        if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, c) == 0) {
            for (unsigned i = 0; i < c->nent; i++) {
                if (c->entries[i].function == 7 && c->entries[i].index == 0) {
                    has = (c->entries[i].ecx >> 2) & 1;
                }
            }
        }
        free(c);
    }
    return has;
}

static void set_tsc_offset(NcCpu *cpu)
{
    /* KVM adds the offset after scaling: P host ticks are P x 733333 / host
     * kHz guest ticks */
    int64_t off = -(int64_t)(((unsigned __int128)cpu->paused * GUEST_KHZ + cpu->host_khz / 2) / cpu->host_khz);
    struct kvm_device_attr at = {.group = KVM_VCPU_TSC_CTRL, .attr = KVM_VCPU_TSC_OFFSET,
                                 .addr = (uint64_t)(uintptr_t)&off};
    if (ioctl(cpu->vcpu, KVM_SET_DEVICE_ATTR, &at) < 0) {
        nc_log("hakuX-native-kvm", "TSC offset: %s", strerror(errno));
    }
}

static void kick_handler(int sig) {}

static bool refuse(NcCpu *cpu, const char *why)
{
    nc_log("hakuX-native-kvm", "no guest CPU: %s%s%s", why, errno ? ": " : "", errno ? strerror(errno) : "");
    nc_cpu_destroy(cpu);
    return false;
}

NcCpu *nc_cpu_create(const NcCpuConfig *cfg, uint32_t fs_base)
{
    if (!cfg->vm || !cfg->fault || !cfg->mmio || !cfg->port_in || !cfg->port_out || !cfg->cpuid) {
        nc_log("hakuX-native-kvm", "no guest CPU: the core gave the KVM backend no devices or memory");
        return NULL;
    }
    NcCpu *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) {
        return NULL;
    }
    cpu->cfg = *cfg;
    cpu->vm = cfg->vm;
    cpu->fs_base = fs_base;
    cpu->kvm = cpu->vmfd = cpu->vcpu = -1;
    cpu->frame.cpu = cpu;
    errno = 0;
    cpu->kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (cpu->kvm < 0) {
        return refuse(cpu, "/dev/kvm"), NULL;
    }
    errno = 0;
    if (ioctl(cpu->kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION ||
        ioctl(cpu->kvm, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT) <= 0) {
        return refuse(cpu, "this KVM lacks the API version or immediate exits"), NULL;
    }
    if (ioctl(cpu->kvm, KVM_CHECK_EXTENSION, KVM_CAP_TSC_CONTROL) <= 0) {
        return refuse(cpu, "no TSC scaling (KVM_CAP_TSC_CONTROL): the guest's RDTSC must run at 733 MHz"), NULL;
    }
    if (!host_has_umip(cpu->kvm)) {
        return refuse(cpu, "no UMIP: SIDT would report the backend's IDT, not the kernel's"), NULL;
    }
    cpu->vmfd = ioctl(cpu->kvm, KVM_CREATE_VM, 0);
    if (cpu->vmfd < 0) {
        return refuse(cpu, "KVM_CREATE_VM"), NULL;
    }
    cpu->sys = mmap(NULL, SYS_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cpu->sys == MAP_FAILED) {
        cpu->sys = NULL;
        return refuse(cpu, "the backend's pages"), NULL;
    }
    struct kvm_userspace_memory_region ram = {.slot = 0, .guest_phys_addr = 0, .memory_size = cpu->vm->ram_size,
                                              .userspace_addr = (uint64_t)(uintptr_t)cpu->vm->phys};
    struct kvm_userspace_memory_region sys = {.slot = 1, .guest_phys_addr = SYS_GPA, .memory_size = SYS_SIZE,
                                              .userspace_addr = (uint64_t)(uintptr_t)cpu->sys};
    if (ioctl(cpu->vmfd, KVM_SET_USER_MEMORY_REGION, &ram) < 0 ||
        ioctl(cpu->vmfd, KVM_SET_USER_MEMORY_REGION, &sys) < 0) {
        return refuse(cpu, "memory slots"), NULL;
    }
    cpu->vcpu = ioctl(cpu->vmfd, KVM_CREATE_VCPU, 0);
    if (cpu->vcpu < 0) {
        return refuse(cpu, "KVM_CREATE_VCPU"), NULL;
    }
    const int rs = ioctl(cpu->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    cpu->run_size = rs > 0 ? (size_t)rs : 0;
    cpu->run = cpu->run_size ? mmap(NULL, cpu->run_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu->vcpu, 0) : MAP_FAILED;
    if (cpu->run == MAP_FAILED) {
        cpu->run = NULL;
        return refuse(cpu, "the vCPU's run structure"), NULL;
    }
    if (!set_cpuid(cpu)) {
        return refuse(cpu, "KVM_SET_CPUID2"), NULL;
    }
    /* the FPU through the XSAVE area (fpu_get, fpu_set), sized once the
     * guest's CPUID is set */
    if (ioctl(cpu->kvm, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) > 0) {
        cpu->xsave_size = sizeof(struct kvm_xsave);
#ifdef KVM_CAP_XSAVE2
        const int xs = ioctl(cpu->vmfd, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE2);
        if (xs > (int)sizeof(struct kvm_xsave)) {
            cpu->xsave_size = (size_t)xs;
            cpu->xsave2 = true;
        }
#endif
        cpu->xsave = calloc(1, cpu->xsave_size);
        if (!cpu->xsave) {
            return refuse(cpu, "the XSAVE buffer"), NULL;
        }
    }
    const int khz = ioctl(cpu->vcpu, KVM_GET_TSC_KHZ, 0);
    if (khz <= 0 || ioctl(cpu->vcpu, KVM_SET_TSC_KHZ, GUEST_KHZ) < 0) {
        return refuse(cpu, "KVM_SET_TSC_KHZ 733333"), NULL;
    }
    cpu->host_khz = (uint32_t)khz;
    struct kvm_device_attr at = {.group = KVM_VCPU_TSC_CTRL, .attr = KVM_VCPU_TSC_OFFSET};
    if (ioctl(cpu->vcpu, KVM_HAS_DEVICE_ATTR, &at) < 0) {
        return refuse(cpu, "no KVM_VCPU_TSC_OFFSET"), NULL;
    }
    set_tsc_offset(cpu);

    build_tables(cpu);
    struct kvm_sregs *s = &cpu->sregs;
    if (ioctl(cpu->vcpu, KVM_GET_SREGS, s) < 0) {
        return refuse(cpu, "KVM_GET_SREGS"), NULL;
    }
    s->cs = seg(SEL_CODE3, 0, true, 3);
    s->ds = s->es = s->ss = s->gs = seg(SEL_DATA3, 0, false, 3);
    s->fs = seg(SEL_FS, fs_base, false, 3);
    s->tr = (struct kvm_segment){.base = LIN_TSS, .limit = 0x67, .selector = SEL_TSS, .type = 0xB, .present = 1};
    s->ldt = (struct kvm_segment){.selector = 0, .unusable = 1, .type = 0x2, .present = 0};
    s->gdt = (struct kvm_dtable){.base = LIN_GDT, .limit = 7 * 8 - 1};
    s->idt = (struct kvm_dtable){.base = LIN_IDT, .limit = 256 * 8 - 1};
    /* PE, MP, ET, NE, WP, PG; AM off: a title at CPL 3 with EFLAGS.AC set
     * would take #AC where the Xbox's ring 0 doesn't */
    s->cr0 = 0x80010033u;
    s->cr3 = SYS_GPA + SYS_PD0;
    s->cr4 = (1u << 9) | (1u << 10) | (1u << 11); /* OSFXSR, OSXMMEXCPT, UMIP */
    s->efer = 0;
    if (ioctl(cpu->vcpu, KVM_SET_SREGS, s) < 0) {
        return refuse(cpu, "KVM_SET_SREGS (protected mode, paging, UMIP)"), NULL;
    }

    struct sigaction sa = {.sa_handler = kick_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(KICK_SIGNAL, &sa, &g_prev_kick);

    g_cpu = cpu;
    g_prev_hook = nc_vm_prot_hook;
    nc_vm_prot_hook = prot_hook;
    nc_log("hakuX-native-kvm", "guest CPU: KVM, CPL 3 with IOPL 3, TSC %u kHz on a %u kHz host", GUEST_KHZ,
           cpu->host_khz);
    return cpu;
}

void nc_cpu_destroy(NcCpu *cpu)
{
    if (!cpu) {
        return;
    }
    if (g_cpu == cpu) {
        nc_vm_prot_hook = g_prev_hook;
        g_prev_hook = NULL;
        g_cpu = NULL;
        sigaction(KICK_SIGNAL, &g_prev_kick, NULL);
    }
    if (cpu->run) {
        munmap(cpu->run, cpu->run_size);
    }
    if (cpu->vcpu >= 0) {
        close(cpu->vcpu);
    }
    if (cpu->vmfd >= 0) {
        close(cpu->vmfd);
    }
    if (cpu->kvm >= 0) {
        close(cpu->kvm);
    }
    if (cpu->sys) {
        munmap(cpu->sys, SYS_SIZE);
    }
    free(cpu->xsave);
    free(cpu);
}

void nc_cpu_thread_done(void) {}

/* ------------------------------------------------------------ contexts */

static const KvmContext *kctx(const NcCpuContext *c)
{
    return (const KvmContext *)c->blob;
}

static KvmContext *kctx_w(NcCpuContext *c)
{
    return (KvmContext *)c->blob;
}

void nc_context_init(NcCpu *cpu, NcCpuContext *c, uint32_t eip, uint32_t esp)
{
    memset(c, 0, sizeof(*c));
    KvmContext *k = kctx_w(c);
    k->rip = eip;
    k->regs[NC_ESP] = esp;
    k->eflags = 0x202u;
    k->fpu.fcw = 0x037F;
    k->fpu.mxcsr = 0x1F80;
}

void nc_context_set_gpr(NcCpuContext *c, int reg, uint32_t v) { kctx_w(c)->regs[reg] = v; }
uint32_t nc_context_gpr(const NcCpuContext *c, int reg) { return (uint32_t)kctx(c)->regs[reg]; }
uint32_t nc_context_eip(const NcCpuContext *c) { return (uint32_t)kctx(c)->rip; }
void nc_context_set_eip(NcCpuContext *c, uint32_t eip) { kctx_w(c)->rip = eip; }

void nc_context_export(const NcCpuContext *c, NcCpuState *s)
{
    const KvmContext *k = kctx(c);
    memset(s, 0, sizeof(*s));
    s->rip = k->rip;
    for (int i = 0; i < 8; i++) {
        s->gregs[i] = (uint32_t)k->regs[i];
    }
    for (int i = 0; i < 32; i++) {
        s->flags[i] = (k->eflags >> i) & 1;
    }
    /* the x87 status word (TOP, C0-C3) after EFLAGS' bits: KVM's states
     * only, as FEX keeps its own there */
    s->flags[40] = (uint8_t)k->fpu.fsw;
    s->flags[41] = (uint8_t)(k->fpu.fsw >> 8);
    for (int i = 0; i < 8; i++) {
        memcpy(s->mm[i], k->fpu.fpr[i], 16);
        memcpy(s->xmm[i], k->fpu.xmm[i], 16);
    }
    s->mxcsr = k->fpu.mxcsr;
    s->fcw = k->fpu.fcw;
    s->ftw = k->fpu.ftwx;
    s->cs_idx = SEL_CODE3;
    s->ds_idx = s->es_idx = s->ss_idx = s->gs_idx = SEL_DATA3;
    s->fs_idx = SEL_FS;
}

void nc_context_import(NcCpuContext *c, const NcCpuState *s)
{
    memset(c, 0, sizeof(*c));
    KvmContext *k = kctx_w(c);
    k->rip = (uint32_t)s->rip;
    for (int i = 0; i < 8; i++) {
        k->regs[i] = (uint32_t)s->gregs[i];
    }
    for (int i = 0; i < 32; i++) {
        k->eflags |= (uint32_t)(s->flags[i] & 1) << i;
    }
    k->fpu.fsw = (uint16_t)(s->flags[40] | s->flags[41] << 8);
    k->eflags |= 2;
    for (int i = 0; i < 8; i++) {
        memcpy(k->fpu.fpr[i], s->mm[i], 16);
        memcpy(k->fpu.xmm[i], s->xmm[i], 16);
    }
    k->fpu.mxcsr = s->mxcsr;
    k->fpu.fcw = s->fcw;
    k->fpu.ftwx = s->ftw;
}

/* ------------------------------------------------------------ the FPU */

/* The vCPU's x87, MMX and SSE registers, through its XSAVE area. KVM_GET_FPU
 * and KVM_SET_FPU copy only the area's legacy part and leave its header as
 * the last save made it: after a guest thread whose x87 or SSE state was in
 * its initial configuration (XSTATE_BV's bit clear), the state set for the
 * next thread was dropped at the next entry, its registers initialized
 * (zero), and the state read back stale. Burnout 3's XMV decoder lost the
 * MMX register holding its alpha mask so mid-frame: the rest of the movie
 * frame alpha 0, drawn transparent over black. Here the header says both
 * are set, so the next entry loads them. */
enum { XS_FCW = 0, XS_FSW = 2, XS_FTW = 4, XS_FOP = 6, XS_FIP = 8, XS_FDP = 16, XS_MXCSR = 24,
       XS_ST = 32, XS_XMM = 160, XS_BV = 512 };
#define XFEATURE_X87_SSE 3u

static bool xsave_read(NcCpu *cpu)
{
#ifdef KVM_GET_XSAVE2
    if (cpu->xsave2) {
        return ioctl(cpu->vcpu, KVM_GET_XSAVE2, cpu->xsave) == 0;
    }
#endif
    return ioctl(cpu->vcpu, KVM_GET_XSAVE, cpu->xsave) == 0;
}

static bool fpu_get(NcCpu *cpu, struct kvm_fpu *f)
{
    if (!cpu->xsave) {
        return ioctl(cpu->vcpu, KVM_GET_FPU, f) == 0;
    }
    if (!xsave_read(cpu)) {
        return false;
    }
    /* the legacy part as KVM_GET_XSAVE gives it: a component in its
     * initial state reads as that state */
    const uint8_t *b = (const uint8_t *)cpu->xsave->region;
    memset(f, 0, sizeof(*f));
    memcpy(&f->fcw, b + XS_FCW, 2);
    memcpy(&f->fsw, b + XS_FSW, 2);
    f->ftwx = b[XS_FTW];
    memcpy(&f->last_opcode, b + XS_FOP, 2);
    memcpy(&f->last_ip, b + XS_FIP, 8);
    memcpy(&f->last_dp, b + XS_FDP, 8);
    memcpy(&f->mxcsr, b + XS_MXCSR, 4);
    memcpy(f->fpr, b + XS_ST, sizeof(f->fpr));
    memcpy(f->xmm, b + XS_XMM, sizeof(f->xmm));
    return true;
}

static bool fpu_set(NcCpu *cpu, const struct kvm_fpu *f)
{
    if (!cpu->xsave) {
        return ioctl(cpu->vcpu, KVM_SET_FPU, f) == 0;
    }
    /* the other components (and their header bits) as they are */
    if (!xsave_read(cpu)) {
        return false;
    }
    uint8_t *b = (uint8_t *)cpu->xsave->region;
    memcpy(b + XS_FCW, &f->fcw, 2);
    memcpy(b + XS_FSW, &f->fsw, 2);
    b[XS_FTW] = f->ftwx;
    memcpy(b + XS_FOP, &f->last_opcode, 2);
    memcpy(b + XS_FIP, &f->last_ip, 8);
    memcpy(b + XS_FDP, &f->last_dp, 8);
    memcpy(b + XS_MXCSR, &f->mxcsr, 4);
    memcpy(b + XS_ST, f->fpr, sizeof(f->fpr));
    memcpy(b + XS_XMM, f->xmm, sizeof(f->xmm));
    uint64_t bv;
    memcpy(&bv, b + XS_BV, 8);
    bv |= XFEATURE_X87_SSE;
    memcpy(b + XS_BV, &bv, 8);
    return ioctl(cpu->vcpu, KVM_SET_XSAVE, cpu->xsave) == 0;
}

/* ------------------------------------------------------------ frames */

static void frame_fpu(NcFrame *f)
{
    if (!f->fpu_valid) {
        if (!fpu_get(f->cpu, &f->fpu)) {
            fatal(f->cpu, "reading the FPU");
        }
        f->fpu_valid = true;
    }
}

void nc_frame_save(const NcFrame *cf, NcCpuContext *c)
{
    NcFrame *f = (NcFrame *)cf;
    frame_fpu(f);
    KvmContext *k = kctx_w(c);
    memcpy(k->regs, f->regs, sizeof(k->regs));
    k->rip = f->rip;
    k->eflags = f->eflags;
    k->fpu = f->fpu;
}

void nc_frame_load(NcFrame *f, const NcCpuContext *c)
{
    const KvmContext *k = kctx(c);
    memcpy(f->regs, k->regs, sizeof(f->regs));
    f->rip = k->rip;
    f->eflags = k->eflags;
    f->fpu = k->fpu;
    f->fpu_valid = f->fpu_dirty = true;
}

uint32_t nc_frame_gpr(const NcFrame *f, int reg) { return (uint32_t)f->regs[reg]; }
void nc_frame_set_gpr(NcFrame *f, int reg, uint32_t v) { f->regs[reg] = v; }
uint32_t nc_frame_eip(const NcFrame *f) { return (uint32_t)f->rip; }
void nc_frame_set_eip(NcFrame *f, uint32_t eip) { f->rip = eip; }
bool nc_frame_if(const NcFrame *f) { return f->eflags & EFL_IF; }
void nc_frame_set_if(NcFrame *f, bool on) { f->eflags = on ? f->eflags | EFL_IF : f->eflags & ~EFL_IF; }
void nc_frame_clear_interrupt(NcFrame *f) { __atomic_store_n(&f->cpu->irq, 0, __ATOMIC_RELEASE); }
uint16_t nc_frame_cs(const NcFrame *f) { return SEL_CODE3; }
uint32_t nc_frame_eflags(NcFrame *f) { return f->eflags; }
void nc_frame_set_eflags(NcFrame *f, uint32_t eflags) { f->eflags = eflags | 2; }
uint64_t *nc_frame_regs(NcFrame *f) { return f->regs; }
uint64_t *nc_frame_rip(NcFrame *f) { return &f->rip; }

void nc_frame_bypass_hook(NcFrame *f)
{
    f->cpu->stepping = (uint32_t)f->rip;
}

/* kvm_regs in their order, from and to the frame's x86 order */
static void regs_to_frame(const struct kvm_regs *r, NcFrame *f)
{
    f->regs[NC_EAX] = (uint32_t)r->rax;
    f->regs[NC_ECX] = (uint32_t)r->rcx;
    f->regs[NC_EDX] = (uint32_t)r->rdx;
    f->regs[NC_EBX] = (uint32_t)r->rbx;
    f->regs[NC_ESP] = (uint32_t)r->rsp;
    f->regs[NC_EBP] = (uint32_t)r->rbp;
    f->regs[NC_ESI] = (uint32_t)r->rsi;
    f->regs[NC_EDI] = (uint32_t)r->rdi;
    f->rip = (uint32_t)r->rip;
    f->eflags = (uint32_t)r->rflags;
}

static void frame_to_regs(const NcFrame *f, struct kvm_regs *r)
{
    r->rax = (uint32_t)f->regs[NC_EAX];
    r->rcx = (uint32_t)f->regs[NC_ECX];
    r->rdx = (uint32_t)f->regs[NC_EDX];
    r->rbx = (uint32_t)f->regs[NC_EBX];
    r->rbp = (uint32_t)f->regs[NC_EBP];
    r->rsi = (uint32_t)f->regs[NC_ESI];
    r->rdi = (uint32_t)f->regs[NC_EDI];
}

/* The title's frame at an exit: from its registers, or for a stub exit
 * from the exception frame on the ring-0 stack (vector, error code, EIP,
 * CS, EFLAGS, ESP, SS). */
static void frame_read(NcCpu *cpu, bool stub, uint32_t *vector, uint32_t *err)
{
    struct kvm_regs r;
    if (ioctl(cpu->vcpu, KVM_GET_REGS, &r) < 0) {
        fatal(cpu, "KVM_GET_REGS");
    }
    NcFrame *f = &cpu->frame;
    regs_to_frame(&r, f);
    f->fpu_valid = f->fpu_dirty = false;
    cpu->in_stub = stub;
    if (stub) {
        const uint32_t esp0 = (uint32_t)r.rsp;
        const uint8_t *k = lin(cpu, esp0);
        *vector = nc_rd32(k);
        *err = nc_rd32(k + 4);
        f->rip = nc_rd32(k + 8);
        f->eflags = nc_rd32(k + 16);
        f->regs[NC_ESP] = nc_rd32(k + 20);
        cpu->stub_esp = esp0;
    }
    cpu->last_eip = (uint32_t)f->rip;
}

/* The frame back as the title's state: a stub's IRET takes EIP, EFLAGS
 * and ESP from the ring-0 stack. IOPL stays 3. */
static void frame_write(NcCpu *cpu)
{
    NcFrame *f = &cpu->frame;
    struct kvm_regs r;
    if (ioctl(cpu->vcpu, KVM_GET_REGS, &r) < 0) {
        fatal(cpu, "KVM_GET_REGS");
    }
    frame_to_regs(f, &r);
    /* TF only for a hooked instruction run once: its #DB puts the hook back */
    const uint32_t eflags = ((f->eflags | EFL_IOPL | 2) & ~EFL_TF) | (cpu->stepping ? EFL_TF : 0);
    if (cpu->in_stub) {
        uint8_t *k = lin(cpu, cpu->stub_esp);
        nc_wr32(k + 8, (uint32_t)f->rip);
        nc_wr32(k + 12, SEL_CODE3);
        nc_wr32(k + 16, eflags);
        nc_wr32(k + 20, (uint32_t)f->regs[NC_ESP]);
        nc_wr32(k + 24, SEL_DATA3);
    } else {
        r.rip = (uint32_t)f->rip;
        r.rsp = (uint32_t)f->regs[NC_ESP];
        r.rflags = eflags;
    }
    if (ioctl(cpu->vcpu, KVM_SET_REGS, &r) < 0) {
        fatal(cpu, "KVM_SET_REGS");
    }
    if (f->fpu_dirty && !fpu_set(cpu, &f->fpu)) {
        fatal(cpu, "setting the FPU");
    }
    f->fpu_dirty = false;
}

/* ------------------------------------------------------------ guest code */

/* Guest bytes at a linear address, through the host's view (0 past RAM). */
static bool guest_read(NcCpu *cpu, uint32_t va, void *out, unsigned n)
{
    uint8_t *o = out;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = nc_vm_phys(cpu->vm, va + i);
        if (pa == UINT32_MAX || pa >= cpu->vm->ram_size) {
            return false;
        }
        o[i] = cpu->vm->phys[pa];
    }
    return true;
}

/* A store the backend makes for the guest: through the host's view, after
 * the write barrier (nc_vm_will_write: it may wait for the GPU, or bring a
 * newer image in first). */
static bool guest_write(NcCpu *cpu, uint32_t va, const void *in, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const uint32_t pa = nc_vm_phys(cpu->vm, va + i);
        if (pa == UINT32_MAX || pa >= cpu->vm->ram_size) {
            return false;
        }
    }
    nc_vm_will_write(va, n);
    const uint8_t *s = in;
    for (unsigned i = 0; i < n; i++) {
        cpu->vm->phys[nc_vm_phys(cpu->vm, va + i)] = s[i];
    }
    return true;
}

/* A 32-bit ModRM operand: its length past the ModRM byte and, for memory,
 * its address (segment override applied: FS's base, others flat). */
typedef struct Insn {
    unsigned len;     /* the whole instruction */
    uint8_t op[3];    /* opcode bytes after the prefixes */
    unsigned prefixes;
    bool op16, addr16, fs;
    uint8_t modrm;
    bool mem;
    uint32_t ea;
} Insn;

static bool decode_modrm(NcCpu *cpu, uint32_t at, Insn *in)
{
    uint8_t b[6];
    if (!guest_read(cpu, at, b, 1)) {
        return false;
    }
    in->modrm = b[0];
    const unsigned mod = b[0] >> 6, rm = b[0] & 7;
    unsigned n = 1;
    in->mem = mod != 3;
    if (!in->mem) {
        in->len += n;
        return true;
    }
    if (in->addr16) {
        return false; /* 16-bit addressing: no title's system op uses it */
    }
    const NcFrame *f = &cpu->frame;
    uint32_t ea = 0;
    if (rm == 4) {
        if (!guest_read(cpu, at + n, b + 1, 1)) {
            return false;
        }
        const uint8_t sib = b[1];
        n++;
        const unsigned scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        if (index != 4) {
            ea += nc_frame_gpr(f, (int)index) << scale;
        }
        if (base == 5 && mod == 0) {
            uint32_t d;
            if (!guest_read(cpu, at + n, &d, 4)) {
                return false;
            }
            ea += d;
            n += 4;
        } else {
            ea += nc_frame_gpr(f, (int)base);
        }
    } else if (rm == 5 && mod == 0) {
        uint32_t d;
        if (!guest_read(cpu, at + n, &d, 4)) {
            return false;
        }
        ea = d;
        n += 4;
    } else {
        ea = nc_frame_gpr(f, (int)rm);
    }
    if (mod == 1) {
        int8_t d;
        if (!guest_read(cpu, at + n, &d, 1)) {
            return false;
        }
        ea += (uint32_t)(int32_t)d;
        n += 1;
    } else if (mod == 2) {
        uint32_t d;
        if (!guest_read(cpu, at + n, &d, 4)) {
            return false;
        }
        ea += d;
        n += 4;
    }
    in->ea = ea + (in->fs ? cpu->fs_base : 0);
    in->len += n;
    return true;
}

/* The system instruction at eip that faulted (#GP, or UMIP's), classified
 * as FEX's backend reports it. SGDT, SIDT and SMSW are answered here, as
 * FEX answers them (NC_SYSOP_NONE: done, EIP past it). */
static enum NcSysOp classify(NcCpu *cpu, uint32_t eip, Insn *in)
{
    memset(in, 0, sizeof(*in));
    uint8_t b;
    for (;;) {
        if (!guest_read(cpu, eip + in->len, &b, 1) || in->len > 14) {
            return NC_SYSOP_OTHER;
        }
        if (b == 0x66) {
            in->op16 = true;
        } else if (b == 0x67) {
            in->addr16 = true;
        } else if (b == 0x64) {
            in->fs = true;
        } else if (b != 0xF0 && b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x36 && b != 0x3E && b != 0x26 &&
                   b != 0x65) {
            break;
        }
        in->len++;
    }
    in->prefixes = in->len;
    in->op[0] = b;
    in->len++;
    if (b == 0xF4) {
        return NC_SYSOP_HLT;
    }
    if (b == 0xCD) {
        in->len++;
        return NC_SYSOP_INT;
    }
    if (b != 0x0F || !guest_read(cpu, eip + in->len, &in->op[1], 1)) {
        return NC_SYSOP_OTHER;
    }
    in->len++;
    const uint8_t o = in->op[1];
    switch (o) {
    case 0x06: return NC_SYSOP_CLTS;
    case 0x08: return NC_SYSOP_INVD;
    case 0x09: return NC_SYSOP_WBINVD;
    case 0x30: return NC_SYSOP_WRMSR;
    case 0x32: return NC_SYSOP_RDMSR;
    case 0x33: return NC_SYSOP_RDPMC;
    case 0x20: case 0x22:
        return decode_modrm(cpu, eip + in->len, in) ? NC_SYSOP_MOVCR : NC_SYSOP_OTHER;
    case 0x21: case 0x23:
        return decode_modrm(cpu, eip + in->len, in) ? NC_SYSOP_MOVDR : NC_SYSOP_OTHER;
    case 0x00:
    case 0x01: {
        if (!decode_modrm(cpu, eip + in->len, in)) {
            return NC_SYSOP_OTHER;
        }
        const unsigned reg = (in->modrm >> 3) & 7;
        if (o == 0x00) {
            return reg == 2 ? NC_SYSOP_LLDT : reg == 3 ? NC_SYSOP_LTR : NC_SYSOP_OTHER;
        }
        switch (reg) {
        case 0: /* SGDT: limit 0, base 0xFFFE0000 (FEX's, as Linux reports) */
        case 1: { /* SIDT: the kernel's table */
            if (!in->mem) {
                return NC_SYSOP_OTHER;
            }
            uint8_t v[6];
            const uint16_t limit = reg ? cpu->cfg.idt_limit : 0;
            const uint32_t base = reg ? cpu->cfg.idt_base : 0xFFFE0000u;
            memcpy(v, &limit, 2);
            memcpy(v + 2, &base, 4);
            return guest_write(cpu, in->ea, v, 6) ? NC_SYSOP_NONE : NC_SYSOP_OTHER;
        }
        case 4: { /* SMSW: FEX's CR0 */
            const uint32_t cr0 = 0x80050033u;
            if (in->mem) {
                const uint16_t w = (uint16_t)cr0;
                return guest_write(cpu, in->ea, &w, 2) ? NC_SYSOP_NONE : NC_SYSOP_OTHER;
            }
            const int r = in->modrm & 7;
            const uint32_t old = nc_frame_gpr(&cpu->frame, r);
            nc_frame_set_gpr(&cpu->frame, r, in->op16 ? (old & 0xFFFF0000u) | (cr0 & 0xFFFFu) : cr0);
            return NC_SYSOP_NONE;
        }
        case 2: return NC_SYSOP_LGDT;
        case 3: return NC_SYSOP_LIDT;
        case 6: return NC_SYSOP_LMSW;
        case 7: return NC_SYSOP_INVLPG;
        default: return NC_SYSOP_OTHER;
        }
    }
    default:
        return NC_SYSOP_OTHER;
    }
}

static int hook_at(NcCpu *cpu, uint32_t eip)
{
    for (unsigned i = 0; i < cpu->nhooks; i++) {
        if (cpu->hooks[i].eip == eip) {
            return (int)i;
        }
    }
    return -1;
}

static void hooks_patch(NcCpu *cpu, uint32_t va, uint32_t len);

/* The hooked addresses again (cfg.hook_list): the old ones' bytes back,
 * the new ones patched. Hooks come from the HLE layer, which starts after
 * the CPU is made. */
static void hooks_refresh(NcCpu *cpu)
{
    for (unsigned i = 0; i < cpu->nhooks; i++) {
        uint8_t b;
        if (guest_read(cpu, cpu->hooks[i].eip, &b, 1) && b == 0xCC) {
            guest_write(cpu, cpu->hooks[i].eip, &cpu->hooks[i].orig, 1);
        }
    }
    cpu->nhooks = 0;
    if (cpu->cfg.hook_list) {
        uint32_t eips[MAX_HOOKS];
        const unsigned n = cpu->cfg.hook_list(cpu->cfg.opaque, eips, MAX_HOOKS);
        for (unsigned i = 0; i < n && i < MAX_HOOKS; i++) {
            cpu->hooks[cpu->nhooks++] = (Hook){.eip = eips[i]};
        }
    }
    hooks_patch(cpu, 0, 0xFFFFFFFFu);
}

static void hooks_patch(NcCpu *cpu, uint32_t va, uint32_t len)
{
    for (unsigned i = 0; i < cpu->nhooks; i++) {
        Hook *h = &cpu->hooks[i];
        if (h->eip - va >= len || h->eip == cpu->stepping) {
            continue;
        }
        uint8_t b;
        if (guest_read(cpu, h->eip, &b, 1) && b != 0xCC) {
            const uint8_t cc = 0xCC;
            h->orig = b;
            guest_write(cpu, h->eip, &cc, 1);
        }
    }
}

/* ------------------------------------------------------------ running */

static bool sysop(NcCpu *cpu, enum NcSysOp op, unsigned size)
{
    return cpu->cfg.sysop(cpu->cfg.opaque, &cpu->frame, op, size);
}

static void port_io(NcCpu *cpu);
static void mmio(NcCpu *cpu);

/* The access an IN, OUT or MMIO exit stopped at, completed by KVM without
 * running guest code. An access KVM splits (16 bytes of SSE, one across
 * pages) comes back as another exit for its next part: served, until KVM
 * returns EINTR. */
static void complete(NcCpu *cpu)
{
    __atomic_store_n(&cpu->run->immediate_exit, 1, __ATOMIC_RELEASE);
    for (;;) {
        if (ioctl(cpu->vcpu, KVM_RUN, 0) < 0) {
            if (errno != EINTR) {
                fatal(cpu, "completing an I/O exit");
            }
            break;
        }
        if (cpu->run->exit_reason == KVM_EXIT_MMIO) {
            mmio(cpu);
        } else if (cpu->run->exit_reason == KVM_EXIT_IO) {
            port_io(cpu);
        } else {
            fatal(cpu, "an exit while completing an I/O exit");
        }
    }
    __atomic_store_n(&cpu->run->immediate_exit, 0, __ATOMIC_RELEASE);
}

static void port_io(NcCpu *cpu)
{
    struct kvm_run *run = cpu->run;
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    for (uint32_t i = 0; i < run->io.count; i++, data += run->io.size) {
        if (run->io.direction == KVM_EXIT_IO_OUT) {
            uint32_t v = 0;
            memcpy(&v, data, run->io.size);
            cpu->cfg.port_out(run->io.port, run->io.size, v);
        } else {
            const uint32_t v = cpu->cfg.port_in(run->io.port, run->io.size);
            memcpy(data, &v, run->io.size);
        }
    }
}

static void mmio(NcCpu *cpu)
{
    struct kvm_run *run = cpu->run;
    uint64_t v = 0;
    const uint32_t addr = (uint32_t)run->mmio.phys_addr;
    if (run->mmio.is_write) {
        memcpy(&v, run->mmio.data, run->mmio.len);
    }
    if (!cpu->cfg.mmio(addr, run->mmio.len, run->mmio.is_write, &v)) {
        static unsigned logged;
        if (logged++ < 8) {
            nc_log("hakuX-native-kvm", "%s of %u bytes at %08x: no device there", run->mmio.is_write ? "write" : "read",
                   run->mmio.len, addr);
        }
        v = UINT64_MAX;
    }
    if (!run->mmio.is_write) {
        memcpy(run->mmio.data, &v, run->mmio.len);
    }
}

typedef enum { RUN_ON, RUN_STOP } RunState;

/* A guest page fault (#PF): the PTE allows it now (a stale TLB entry,
 * already dropped by the fault), a device window mapped on first use, or
 * the core's fault path. */
static RunState page_fault(NcCpu *cpu, uint32_t err)
{
    struct kvm_sregs s;
    if (ioctl(cpu->vcpu, KVM_GET_SREGS, &s) < 0) {
        fatal(cpu, "KVM_GET_SREGS");
    }
    const uint32_t va = (uint32_t)s.cr2;
    const bool write = err & 2;
    const uint32_t pte = __atomic_load_n(pt_entry(cpu, va), __ATOMIC_ACQUIRE);
    if ((pte & PTE_P) && (pte & PTE_US) && (!write || (pte & PTE_RW))) {
        return RUN_ON;
    }
    if (!(pte & PTE_P) && va >= 0x10000u) {
        pte_update(cpu, va, 1);
        const uint32_t now = __atomic_load_n(pt_entry(cpu, va), __ATOMIC_ACQUIRE);
        if ((now & PTE_P) && (!write || (now & PTE_RW))) {
            return RUN_ON;
        }
    }
    const char *why = NULL;
    switch (cpu->cfg.fault(cpu->cfg.opaque, &cpu->frame, va, write, &why)) {
    case 1: /* NC_GF_RETRY */
    case 2: /* NC_GF_FRAME */
        return RUN_ON;
    default:
        nc_log("hakuX-native-kvm", "guest fault at %08x (%s), eip %08x: %s", va, write ? "write" : "read",
               nc_frame_eip(&cpu->frame), why ? why : "not the guest's");
        abort();
    }
}

/* An exception or INT through a stub: the system op it is, as FEX's
 * backend reports them (EIP the instruction, size its length). */
static RunState stub_exit(NcCpu *cpu)
{
    uint32_t vector = 0, err = 0;
    frame_read(cpu, true, &vector, &err);
    NcFrame *f = &cpu->frame;
    const uint32_t eip = (uint32_t)f->rip;
    uint8_t b = 0;
    switch (vector) {
    case 14:
        return page_fault(cpu, err);
    case 13: {
        Insn in;
        if ((err & 3) == 2) { /* an INT to a DPL-0 gate (an error-code vector) */
            sysop(cpu, NC_SYSOP_INT, 2);
            return RUN_ON;
        }
        const enum NcSysOp op = classify(cpu, eip, &in);
        if (op == NC_SYSOP_HLT) {
            return RUN_STOP;
        }
        if (op == NC_SYSOP_NONE) {
            f->rip = eip + in.len;
            return RUN_ON;
        }
        if (!sysop(cpu, op, op == NC_SYSOP_OTHER ? 0 : in.len)) {
            nc_log("hakuX-native-kvm", "system op %d at %08x: nobody took it", (int)op, eip);
            abort();
        }
        return RUN_ON;
    }
    case 6:
        if (guest_read(cpu, eip, &b, 1) && b == 0x0F && guest_read(cpu, eip + 1, &b, 1) && b == 0x0B) {
            sysop(cpu, NC_SYSOP_UD2, 2);
            return RUN_ON;
        }
        nc_log("hakuX-native-kvm", "#UD at %08x", eip);
        abort();
    case 3: {
        /* EIP is past the INT3 (CC) or INT 3 (CD 03) */
        const int h = hook_at(cpu, eip - 1);
        if (h >= 0) {
            f->rip = eip - 1;
            sysop(cpu, NC_SYSOP_HOOK, 0);
            return RUN_ON;
        }
        if (guest_read(cpu, eip - 1, &b, 1) && b == 0xCC) {
            f->rip = eip - 1;
            sysop(cpu, NC_SYSOP_INT3, 1);
        } else {
            f->rip = eip - 2;
            sysop(cpu, NC_SYSOP_INT, 2);
        }
        return RUN_ON;
    }
    case 4:
        f->rip = eip - 1;
        sysop(cpu, NC_SYSOP_INTO, 1);
        return RUN_ON;
    case 1:
        if (cpu->stepping) {
            /* the hooked instruction ran (TF): its hook back, TF off */
            const uint32_t at = cpu->stepping;
            cpu->stepping = 0;
            f->eflags &= ~EFL_TF;
            hooks_patch(cpu, at, 1);
            return RUN_ON;
        }
        if (guest_read(cpu, eip - 1, &b, 1) && b == 0xF1) {
            f->rip = eip - 1;
            sysop(cpu, NC_SYSOP_INT1, 1);
            return RUN_ON;
        }
        break;
    default:
        if (vector >= 32) {
            f->rip = eip - 2;
            sysop(cpu, NC_SYSOP_INT, 2);
            return RUN_ON;
        }
        break;
    }
    nc_log("hakuX-native-kvm", "exception %u (error %x) at %08x: not one the core takes", vector, err, eip);
    abort();
}

static void deliver_interrupt(NcCpu *cpu)
{
    uint32_t v, e;
    frame_read(cpu, false, &v, &e);
    sysop(cpu, NC_SYSOP_INTERRUPT, 0);
    __atomic_store_n(&cpu->irq, 0, __ATOMIC_RELEASE);
    frame_write(cpu);
}

static bool in_stub_code(uint64_t rip)
{
    return rip >= LIN_STUBS && rip < LIN_STUBS + 256 * 16;
}

static void load_context(NcCpu *cpu, const NcCpuContext *c)
{
    NcFrame *f = &cpu->frame;
    nc_frame_load(f, c);
    cpu->in_stub = false;
    if (ioctl(cpu->vcpu, KVM_SET_SREGS, &cpu->sregs) < 0) {
        fatal(cpu, "KVM_SET_SREGS");
    }
    struct kvm_regs r;
    memset(&r, 0, sizeof(r));
    frame_to_regs(f, &r);
    r.rip = (uint32_t)f->rip;
    r.rsp = (uint32_t)f->regs[NC_ESP];
    r.rflags = (f->eflags | EFL_IOPL | 2) & ~EFL_TF;
    if (ioctl(cpu->vcpu, KVM_SET_REGS, &r) < 0 || !fpu_set(cpu, &f->fpu)) {
        fatal(cpu, "loading the context");
    }
    f->fpu_dirty = false;
}

void nc_cpu_run(NcCpu *cpu, const NcCpuContext *c)
{
    cpu->thread = pthread_self();
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, KICK_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
    hooks_refresh(cpu);
    load_context(cpu, c);
    __atomic_store_n(&cpu->running, true, __ATOMIC_RELEASE);
    for (;;) {
        /* immediate_exit cleared before in_guest is set: a shootdown that
         * sees in_guest sets it after, and it stays set for KVM_RUN */
        __atomic_store_n(&cpu->run->immediate_exit, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&cpu->in_guest, 1, __ATOMIC_SEQ_CST);
        tlb_flush(cpu);
        /* an interrupt waits for KVM's window: IF set and no STI or MOV SS
         * shadow; none while a hooked instruction runs once (TF) */
        cpu->run->request_interrupt_window = __atomic_load_n(&cpu->irq, __ATOMIC_ACQUIRE) && !cpu->stepping;
        const int ran = ioctl(cpu->vcpu, KVM_RUN, 0);
        __atomic_add_fetch(&cpu->exits, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&cpu->in_guest, 0, __ATOMIC_SEQ_CST);
        if (ran < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                fatal(cpu, "KVM_RUN");
            }
            /* a kick: the next entry asks for the window */
            continue;
        }
        RunState st = RUN_ON;
        switch (cpu->run->exit_reason) {
        case KVM_EXIT_HLT: {
            struct kvm_regs r;
            if (ioctl(cpu->vcpu, KVM_GET_REGS, &r) < 0 || (r.rip - LIN_STUBS) % 16 != STUB_HLT ||
                !in_stub_code(r.rip)) {
                fatal(cpu, "a HLT outside the stubs");
            }
            st = stub_exit(cpu);
            if (st == RUN_STOP) {
                __atomic_store_n(&cpu->running, false, __ATOMIC_RELEASE);
                return;
            }
            if (cpu->stepping && (uint32_t)cpu->frame.rip == cpu->stepping) {
                /* nc_frame_bypass_hook: the title's byte back for one
                 * instruction, which TF stops after (#DB, vector 1) */
                const int h = hook_at(cpu, cpu->stepping);
                if (h >= 0) {
                    guest_write(cpu, cpu->stepping, &cpu->hooks[h].orig, 1);
                }
            }
            frame_write(cpu);
            break;
        }
        case KVM_EXIT_IO:
            port_io(cpu);
            complete(cpu);
            break;
        case KVM_EXIT_MMIO:
            mmio(cpu);
            complete(cpu);
            break;
        case KVM_EXIT_IRQ_WINDOW_OPEN:
            if (__atomic_load_n(&cpu->irq, __ATOMIC_ACQUIRE) && !cpu->stepping && cpu->run->ready_for_interrupt_injection) {
                deliver_interrupt(cpu);
            }
            break;
        case KVM_EXIT_SHUTDOWN:
            fatal(cpu, "triple fault (KVM_EXIT_SHUTDOWN)");
            break;
        case KVM_EXIT_FAIL_ENTRY:
            nc_log("hakuX-native-kvm", "entry failed: hardware reason %llx",
                   (unsigned long long)cpu->run->fail_entry.hardware_entry_failure_reason);
            fatal(cpu, "KVM_EXIT_FAIL_ENTRY");
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            nc_log("hakuX-native-kvm", "internal error, suberror %u", cpu->run->internal.suberror);
            fatal(cpu, "KVM_EXIT_INTERNAL_ERROR");
            break;
        default:
            nc_log("hakuX-native-kvm", "exit reason %u", cpu->run->exit_reason);
            fatal(cpu, "an exit the backend doesn't take");
        }
    }
}

void nc_cpu_invalidate(NcCpu *cpu, uint32_t va, uint32_t len)
{
    if (!cpu) {
        return;
    }
    if (va == 0 && len >= 0xFFFFF000u) {
        hooks_refresh(cpu); /* everything: the hook set may have changed */
    } else if (cpu->nhooks) {
        hooks_patch(cpu, va, len);
    }
}

bool nc_cpu_x87_single_check(NcCpu *cpu) { return false; }
bool nc_cpu_x87_single(NcCpu *cpu) { return false; }
void nc_cpu_x87_single_restore(NcCpu *cpu) {}

void nc_cpu_request_interrupt(NcCpu *cpu)
{
    if (!cpu) {
        return;
    }
    __atomic_store_n(&cpu->irq, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&cpu->running, __ATOMIC_ACQUIRE) && !pthread_equal(pthread_self(), cpu->thread)) {
        __atomic_store_n(&cpu->run->immediate_exit, 1, __ATOMIC_RELEASE);
        pthread_kill(cpu->thread, KICK_SIGNAL);
    }
}

void nc_cpu_cycle_offset_add(NcCpu *cpu, uint64_t ticks)
{
    cpu->paused += ticks;
    set_tsc_offset(cpu);
}

uint64_t nc_cpu_host_cycles(void) { return host_tsc(); }
uint64_t nc_cpu_guest_cycles(NcCpu *cpu) { return host_tsc() - cpu->paused; }

void nc_cpu_guest_cycles_set(NcCpu *cpu, uint64_t ticks)
{
    cpu->paused = host_tsc() - ticks;
    set_tsc_offset(cpu);
}

uint32_t nc_cpu_guest_eip_at(NcCpu *cpu, uintptr_t host_pc) { return 0; }

bool nc_cpu_sample_pc(NcCpu *cpu, uintptr_t host_pc, uint32_t *eip)
{
    *eip = cpu ? cpu->last_eip : 0;
    return false;
}

/* Guest code never raises a host signal here: a host fault is host code's. */
NcFrame *nc_cpu_fault_frame(NcCpu *cpu, void *ucontext) { return NULL; }
void nc_cpu_fault_resume(NcCpu *cpu, void *ucontext) {}
bool nc_cpu_fault_is_write(const void *ucontext) { return false; }
bool nc_cpu_unaligned_fault(NcCpu *cpu, int code, void *ucontext) { return false; }
bool nc_cpu_callret_fault(NcCpu *cpu, uintptr_t fault_addr, void *ucontext) { return false; }
