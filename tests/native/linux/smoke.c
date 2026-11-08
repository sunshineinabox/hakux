/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/hle/hle_native.h"
#include "hw/xbox/hle/host.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static NcCore core;
static uint32_t mmio_value, result, calls;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        exit(1);
    }
}

static void fault(int sig, siginfo_t *info, void *context)
{
    if (core.cpu && nc_core_fault(&core, sig, info, context)) {
        return;
    }
    static const char message[] = "FAIL: unexpected host fault\n";
    write(2, message, sizeof(message) - 1);
    _exit(3);
}

static uint64_t read_mmio(void *opaque, uint32_t offset, unsigned size)
{
    return mmio_value;
}

static void write_mmio(void *opaque, uint32_t offset, uint64_t value, unsigned size)
{
    mmio_value = (uint32_t)value;
}

/* What nc_vm_prot_hook was told, page by page, as a CPU backend with its own
 * page tables (KVM) mirrors it: the protection, and the frame vm->pte had
 * when the hook was called (a backend reads it there). */
static uint8_t hooked[1u << 20];
static uint32_t hooked_pte[1u << 20];

static void prot_hook(uint32_t va, uint32_t len, int prot)
{
    for (uint32_t i = va / NC_PAGE; i < (uint32_t)(((uint64_t)va + len) / NC_PAGE); i++) {
        hooked[i] = (uint8_t)prot;
        hooked_pte[i] = core.vm.pte[i];
    }
}

/* Every page of the guest's 4 GiB as the host has it (/proc/self/maps)
 * against vm->vprot and the hook's record. */
static bool prot_mirrored(const NcVm *vm)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        return false;
    }
    const uintptr_t lo = (uintptr_t)vm->base, hi = lo + (1ull << 32);
    unsigned seen = 0, bad = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long a, b;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3 || b <= lo || a >= hi) {
            continue;
        }
        const int prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0);
        for (uintptr_t p = a < lo ? lo : a; p < (b > hi ? hi : b); p += NC_PAGE) {
            const uint32_t i = (uint32_t)((p - lo) / NC_PAGE);
            seen++;
            if (vm->vprot[i] != prot || hooked[i] != prot || hooked_pte[i] != vm->pte[i]) {
                if (bad++ < 4) {
                    printf("  va %08x: host %d, vprot %d, hook %d; pte %08x, at the hook %08x\n", i * NC_PAGE,
                           prot, vm->vprot[i], hooked[i], vm->pte[i], hooked_pte[i]);
                }
            }
        }
    }
    fclose(f);
    return seen == (1u << 20) && !bad;
}

/* Another thread protecting a page the guest keeps storing to: once
 * hle_host_mem_test_and_clear() returns, the guest's next store must fault
 * and be seen (dirty), as mprotect guarantees under FEX; a backend with its
 * own page tables (KVM) must take the old entry from the guest's TLB first.
 * A store seen as a changed counter on a page still clean went through a
 * stale entry. */
#define RACE_ROUNDS 1000
static uint32_t race_pa, race_stop_pa;
static unsigned race_unseen, race_rounds;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *race(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)(core.vm.phys + race_pa);
    for (unsigned i = 0; i < RACE_ROUNDS; i++) {
        hle_host_mem_test_and_clear(race_pa, NC_PAGE);
        const uint32_t before = *counter;
        const uint64_t t0 = now_ns();
        while (*counter == before && now_ns() - t0 < 1000000000ull) {
        }
        race_rounds++;
        if (!hle_host_mem_dirty(race_pa)) {
            race_unseen++;
        }
    }
    *(volatile uint32_t *)(core.vm.phys + race_stop_pa) = 1;
    return NULL;
}

static bool system_op(void *opaque, NcFrame *frame, enum NcSysOp op, unsigned size)
{
    if (op == NC_SYSOP_CPUID) {
        uint32_t r[4];
        nc_kernel_cpuid(nc_frame_gpr(frame, NC_EAX), nc_frame_gpr(frame, NC_ECX), r);
        nc_frame_set_gpr(frame, NC_EAX, r[0]);
        nc_frame_set_gpr(frame, NC_EBX, r[1]);
        nc_frame_set_gpr(frame, NC_ECX, r[2]);
        nc_frame_set_gpr(frame, NC_EDX, r[3]);
        nc_frame_set_eip(frame, nc_frame_eip(frame) + size);
        return true;
    }
    if (op != NC_SYSOP_INT || size != 2) {
        return false;
    }
    calls++;
    result = nc_frame_gpr(frame, NC_EAX);
    nc_frame_set_eip(frame, nc_frame_eip(frame) + size);
    return true;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    check(sysconf(_SC_PAGESIZE) == NC_PAGE, "host page size supports Xbox page mappings");
    nc_vm_prot_hook = prot_hook;
    check(nc_vm_init(&core.vm, 64u << 20), "memfd VM initialization");
    check(nc_kernel_init(&core.kernel, &core.vm), "replacement kernel initialization");
    /* Keep the title memory between the kernel and top reserve free at boot. */
    bool layouts_free = true;
    for (uint32_t pa = NC_KERNEL_PA + NC_KERNEL_SIZE; pa < core.vm.ram_size - NC_TOP_RESERVE; pa += NC_PAGE) {
        layouts_free = layouts_free && !core.vm.page_used[pa / NC_PAGE];
    }
    check(layouts_free, "kernel boot leaves the titles' fixed physical layouts free");
    check(nc_vm_alloc_at(&core.vm, 0x10000, NC_PAGE, NULL), "guest code page");
    uint32_t pa;
    check(nc_vm_alloc_at(&core.vm, 0x20000, NC_PAGE, &pa), "guest data page");
    const uint32_t alias = NC_CONTIG_WINDOW + pa;
    /* Volatile accesses preserve ordering across distinct virtual addresses
     * which the C optimizer cannot know alias the same physical page. */
    *(volatile uint32_t *)nc_vm_host(&core.vm, 0x20000) = 0x12345678;
    check(*(volatile uint32_t *)nc_vm_host(&core.vm, alias) == 0x12345678, "memory aliases share bytes");

    nc_hle_host_init(&core.kernel, &core.vm, NULL);
    nc_hle_dirty_init(&core.vm);
    hle_guestmem_init(core.vm.phys, core.vm.ram_size, 0);
    hle_barrier_init(core.vm.ram_size);
    hle_barrier_arm(pa, NC_PAGE, 1);
    check(!hle_host_mem_dirty(pa), "data page is tracked and read-only");
    NcMmioDev dev = {"smoke", 0xFD000000, NC_PAGE, read_mmio, write_mmio, NULL};
    nc_mmio_register(&dev);
    struct sigaction sa = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGSEGV, &sa, NULL) == 0 && sigaction(SIGBUS, &sa, NULL) == 0,
          "glibc signal handlers installed");

    /* mov eax,6; mov ebx,7; add; cmp 13; jne fail; write data;
     * read alias; write/read MMIO; int 42; hlt. Failure reports 0xdead. */
    uint8_t code[] = {
        0xb8,6,0,0,0, 0xbb,7,0,0,0, 0x01,0xd8, 0x83,0xf8,13,
        0x75,23, 0xa3,0,0,2,0, 0xa1,0,0,3,0,
        0xa3,0,0,0,0xfd, 0xa1,0,0,0,0xfd, 0xcd,0x42, 0xf4,
        0xb8,0xad,0xde,0,0, 0xcd,0x42, 0xf4
    };
    nc_wr32(code + 23, alias);
    memcpy(nc_vm_host(&core.vm, 0x10000), code, sizeof(code));
    NcCpuConfig cfg = {.membase = core.vm.base, .sysop = system_op};
    nc_core_cpu_backend(&core, &cfg);
    core.cpu = nc_cpu_create(&cfg, core.kernel.kpcr_va);
    const bool kvm = !strcmp(nc_cpu_backend, "kvm");
    check(core.cpu != NULL, kvm ? "actual NcCpu/KVM guest initialized" : "actual NcCpu/FEX embedding initialized");
    NcCpuContext context;
    nc_context_init(core.cpu, &context, 0x10000, 0);
    nc_cpu_run(core.cpu, &context);
    check(calls == 1, "system-operation callback and HLT exit");
    check(result == (argc > 1 && !strcmp(argv[1], "--wrong-expectation") ? 14u : 13u),
          "translated arithmetic, branch and registers");
    check(*(volatile uint32_t *)nc_vm_host(&core.vm, alias) == 13, "translated write visible through alias");
    check(g_nc_faults_dirty == 1 && hle_host_mem_dirty(pa), "translated write fault resumed and dirtied page");
    /* FEX's MMIO is a host fault each; KVM's an exit, counted by the device layer */
    check((kvm ? nc_mmio_total() == 2 : g_nc_faults_mmio == 2) && mmio_value == 13,
          "translated MMIO read/write faults resumed");
    /* x86 permits a locked dword at an odd address. Older ARM hosts need
     * the SIGBUS handler; newer ones may execute it directly. */
    const uint8_t unaligned[] = {0xf0, 0xff, 0x05, 3, 0, 2, 0, 0xf4}; /* lock inc [0x20003]; hlt */
    memcpy(nc_vm_host(&core.vm, 0x10100), unaligned, sizeof(unaligned));
    nc_wr32(nc_vm_host(&core.vm, 0x20003), 41);
    nc_context_init(core.cpu, &context, 0x10100, 0);
    nc_cpu_run(core.cpu, &context);
    check(nc_rd32(nc_vm_host(&core.vm, 0x20003)) == 42, "translated unaligned locked increment");
    check(prot_mirrored(&core.vm), "page protections mirrored after the tracked write opened one view");

    /* CPUID as the kernel's table answers it: leaf 0's vendor, leaf 1's
     * signature, zeros past the highest leaf (7). */
    struct {
        uint32_t leaf;
        uint8_t then[6];
        uint32_t want;
        const char *what;
    } leaves[] = {
        {0, {0x89, 0xd8, 0x90, 0x90, 0x90, 0x90}, 0x756E6547, "CPUID leaf 0: GenuineIntel"}, /* mov eax, ebx */
        {1, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 0x068A, "CPUID leaf 1: the Pentium III's signature"},
        {7, {0x09, 0xd8, 0x09, 0xc8, 0x09, 0xd0}, 0, "CPUID leaf 7: zeros past leaf 2"}, /* or eax, ebx/ecx/edx */
    };
    leaves[2].want = kvm ? 4 : 0; /* KVM's one difference: UMIP (ECX bit 2), which CR4.UMIP needs */
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        /* mov eax, leaf; xor ecx, ecx; cpuid; <then>; int 42h; hlt */
        uint8_t prog[18] = {0xb8, 0, 0, 0, 0, 0x31, 0xc9, 0x0f, 0xa2};
        nc_wr32(prog + 1, leaves[i].leaf);
        memcpy(prog + 9, leaves[i].then, 6);
        prog[15] = 0xcd;
        prog[16] = 0x42;
        prog[17] = 0xf4;
        memcpy(nc_vm_host(&core.vm, 0x10200 + 0x40 * i), prog, sizeof(prog));
        nc_context_init(core.cpu, &context, 0x10200 + 0x40 * i, 0);
        nc_cpu_run(core.cpu, &context);
        check(result == leaves[i].want, leaves[i].what);
    }

    /* Fable's pager: a page-directory entry pointing at the directory makes
     * the page tables a 4 MiB window at 0xC0000000, and it reads an entry
     * through the window before any flush. The access faults; the tables
     * are applied (nc_paging_fault) before the title's handler is asked. */
    const uint32_t pd = nc_paging_cr3(&core.kernel);
    check(pd != 0, "a page directory for CR3");
    nc_wr32(core.vm.phys + pd + 4 * 0x300, pd | 3);
    const uint8_t recursive[] = {0xa1, 0x00, 0x0c, 0x30, 0xc0, 0xcd, 0x42, 0xf4}; /* mov eax, [0xC0300C00] */
    memcpy(nc_vm_host(&core.vm, 0x10300), recursive, sizeof(recursive));
    nc_context_init(core.cpu, &context, 0x10300, 0);
    nc_cpu_run(core.cpu, &context);
    check(result == (pd | 3), "recursive PDE read through before any flush");
    check(prot_mirrored(&core.vm), "page protections mirrored after the tables mapped a page");

    hle_host_mem_noaccess(pa, NC_PAGE, true);
    check(prot_mirrored(&core.vm), "page protections mirrored with a page closed");
    hle_host_mem_noaccess(pa, NC_PAGE, false);
    check(prot_mirrored(&core.vm), "page protections mirrored with it open again");

    /* L: inc dword [0x22000]; mov eax, [0x24000]; test eax, eax; jz L; hlt */
    check(nc_vm_alloc_at(&core.vm, 0x22000, NC_PAGE, &race_pa) &&
          nc_vm_alloc_at(&core.vm, 0x24000, NC_PAGE, &race_stop_pa), "pages for the protection race");
    const uint8_t stores[] = {0xff, 0x05, 0x00, 0x20, 0x02, 0x00, 0xa1, 0x00, 0x40, 0x02, 0x00,
                              0x85, 0xc0, 0x74, 0xf1, 0xf4};
    memcpy(nc_vm_host(&core.vm, 0x10400), stores, sizeof(stores));
    pthread_t racer;
    pthread_create(&racer, NULL, race, NULL);
    nc_context_init(core.cpu, &context, 0x10400, 0);
    nc_cpu_run(core.cpu, &context);
    pthread_join(racer, NULL);
    printf("  protection race: %u rounds, %u stores unseen\n", race_rounds, race_unseen);
    check(race_rounds == RACE_ROUNDS && race_unseen == 0,
          "every guest store after another thread protects its page is seen");

    /* A read of an unmapped page goes to the thread's SEH handler while
     * write tracking has the stack page read-only. Under FEX the dispatch
     * runs in the SIGSEGV handler, so its stores to the stack must not fault.
     * The handler reports the exception code. */
    uint32_t stack_pa;
    check(nc_vm_alloc_at(&core.vm, 0x26000, NC_PAGE, &stack_pa), "a stack page for an access violation");
    const uint32_t seh_reg = 0x26F00;
    nc_wr32(nc_vm_host(&core.vm, seh_reg), 0xFFFFFFFFu); /* Next: the end of the chain */
    nc_wr32(nc_vm_host(&core.vm, seh_reg + 4), 0x10580); /* Handler */
    const uint32_t kpcr = core.kernel.kpcr_va;
    nc_wr32(nc_vm_host(&core.vm, kpcr), seh_reg);     /* ExceptionList */
    nc_wr32(nc_vm_host(&core.vm, kpcr + 4), 0x27000); /* StackBase */
    nc_wr32(nc_vm_host(&core.vm, kpcr + 8), 0x26000); /* StackLimit */
    core.kernel.current = &core.kernel.threads[0];
    hle_host_mem_test_and_clear(stack_pa, NC_PAGE);
    check(!hle_host_mem_dirty(stack_pa), "stack page is tracked and read-only");
    /* mov eax, [0x30000000]; hlt (ESP 0x26F00) */
    const uint8_t violate[] = {0xa1, 0x00, 0x00, 0x00, 0x30, 0xf4};
    /* handler: mov eax, [esp+4] (ExceptionRecord); mov eax, [eax]; int 42h; hlt */
    const uint8_t handler[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00, 0xcd, 0x42, 0xf4};
    memcpy(nc_vm_host(&core.vm, 0x10500), violate, sizeof(violate));
    memcpy(nc_vm_host(&core.vm, 0x10580), handler, sizeof(handler));
    result = 0;
    nc_context_init(core.cpu, &context, 0x10500, seh_reg);
    nc_cpu_run(core.cpu, &context);
    check(result == 0xC0000005u, "access violation dispatched to the SEH handler over a tracked stack");
    check(hle_host_mem_dirty(stack_pa), "the dispatch's stack writes are tracked");
    core.kernel.current = NULL;

    /* The same fault with a page-fault handler in the IDT: its interrupt
     * frame goes on the tracked stack. The handler reports the faulting EIP. */
    const uint32_t gate = core.kernel.idt_va + 8 * 14;
    const uint32_t gate_lo = nc_rd32(nc_vm_host(&core.vm, gate));
    const uint32_t gate_hi = nc_rd32(nc_vm_host(&core.vm, gate + 4));
    nc_wr32(nc_vm_host(&core.vm, gate), (0x08u << 16) | 0x0600u);
    nc_wr32(nc_vm_host(&core.vm, gate + 4), 0x00010000u | 0x8E00u);
    hle_host_mem_test_and_clear(stack_pa, NC_PAGE);
    check(!hle_host_mem_dirty(stack_pa), "stack page is tracked again");
    /* handler: mov eax, [esp+4] (EIP); int 42h; hlt */
    const uint8_t pf_handler[] = {0x8b, 0x44, 0x24, 0x04, 0xcd, 0x42, 0xf4};
    memcpy(nc_vm_host(&core.vm, 0x10600), pf_handler, sizeof(pf_handler));
    result = 0;
    nc_context_init(core.cpu, &context, 0x10500, seh_reg);
    nc_cpu_run(core.cpu, &context);
    check(result == 0x10500u, "page fault delivered to the IDT's handler over a tracked stack");
    check(hle_host_mem_dirty(stack_pa), "the interrupt frame's stack writes are tracked");
    nc_wr32(nc_vm_host(&core.vm, gate), gate_lo);
    nc_wr32(nc_vm_host(&core.vm, gate + 4), gate_hi);
    if (argc > 1 && !strcmp(argv[1], "--host-fault")) {
        /* Host code writing the reservation's low guard, after the CPU ran:
         * nc_core_fault must decline it rather than resume it as the guest's. */
        *(volatile uint32_t *)core.vm.reservation = 1;
        return 1;
    }
    return 0;
}
