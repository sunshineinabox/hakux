/*
 * hakuX native core — boot and run a title.
 */
#include "native/core/core.h"
#include "native/core/hostctx.h"
#include "native/dev/dev.h"
#include "native/dev/usb.h"
#include "native/hle/hle_native.h"
#include "native/core/mmio.h"
#include "native/core/irq.h"
#include "native/core/mods.h"
#include "native/net/net.h"
#include "native/state/save.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <unistd.h>

#define MAIN_STACK_SIZE (256u * 1024u)

static NcThread *g_boot;
static bool g_relaunch;
static bool g_dashboard;        /* boot the dashboard (nc_core_set_dashboard) */
static char g_loadstate[512];   /* boot into this savestate (nc_core_set_loadstate) */
static bool g_loaded;

void nc_core_set_relaunch(bool on)
{
    g_relaunch = on;
}

void nc_core_set_dashboard(bool on)
{
    g_dashboard = on;
}

void nc_core_set_loadstate(const char *path)
{
    snprintf(g_loadstate, sizeof(g_loadstate), "%s", path ? path : "");
}
static bool core_hook_query(void *opaque, uint32_t eip);

/* An XBE header's certificate: the title ID and version. */
static bool xbe_title(const uint8_t *h, size_t len, uint32_t *tid, uint32_t *ver)
{
    if (len < 0x180 || memcmp(h, "XBEH", 4)) {
        return false;
    }
    const uint32_t cert = nc_rd32(h + 0x118) - nc_rd32(h + 0x104);
    if (cert > len - 0xB0) {
        return false;
    }
    *tid = nc_rd32(h + cert + 0x08);
    *ver = nc_rd32(h + cert + 0xAC);
    return true;
}

/* An Xbox Live title update of a disc XBE (path "\default.xbe" or
 * "\Device\CdRom0\default.xbe"): as the Xbox kernel does, the file of that
 * name in E:\TDATA\<title ID>\$u boots instead when it is the same title's
 * and no older (the title's updater installs it there, then relaunches the
 * disc). Its path in out; false when there is none. */
static bool title_update(const NcDisc *disc, const char *path, char *out, size_t n)
{
    static const char cd[] = "\\Device\\CdRom0\\";
    const char *rel = !strncasecmp(path, cd, sizeof(cd) - 1)                ? path + sizeof(cd) - 1
                      : path[0] == '\\' && strncasecmp(path, "\\Device\\", 8) ? path + 1
                                                                            : NULL;
    NcDiscFile f;
    uint8_t h[0x1000], u[0x1000];
    uint32_t tid, ver, utid, uver;
    if (!rel || strchr(rel, '\\') || !nc_disc_find(disc, rel, &f) || f.size < sizeof(h) ||
        !nc_disc_read(disc, &f, 0, h, sizeof(h)) || !xbe_title(h, sizeof(h), &tid, &ver)) {
        return false;
    }
    char host[512];
    snprintf(out, n, "\\Device\\Harddisk0\\Partition1\\TDATA\\%08x\\$u\\%s", tid, rel);
    if (!nc_io_host_file(out, host, sizeof(host))) {
        return false;
    }
    FILE *fp = fopen(host, "rb");
    const bool read = fp && fread(u, 1, sizeof(u), fp) == sizeof(u);
    if (fp) {
        fclose(fp);
    }
    if (!read || !xbe_title(u, sizeof(u), &utid, &uver) || utid != tid || uver < ver) {
        nc_log("hakuX-native-frontier", "title update %s: not this title's newer version (title %08x version %x, "
               "the disc's %08x %x): the disc's boots", out, read ? utid : 0, read ? uver : 0, tid, ver);
        return false;
    }
    nc_log("hakuX-native-frontier", "title update: %s (version %x, the disc's %x) boots for %s", out, uver, ver, path);
    return true;
}

/* The title can't start: why, for the activity's dialog (NC_FATAL_BOOT). */
static bool boot_failed(const char *why)
{
    nc_hle_set_fatal(NC_FATAL_BOOT, why);
    return false;
}

/* NC_BOOT_FAIL_AT=<stage> (the tests): the boot fails once it is past that
 * stage, leaving what it made by then to the stop: kernel, disc, xbe, cpu,
 * thread, hle (the HLE GPU started). */
static bool fail_at(const char *stage)
{
    const char *e = getenv("NC_BOOT_FAIL_AT");
    if (!e || strcmp(e, stage)) {
        return false;
    }
    nc_log("hakuX-native-frontier", "boot: failing after %s (NC_BOOT_FAIL_AT)", stage);
    return true;
}

bool nc_core_boot(NcCore *c, int disc_fd)
{
    memset(c, 0, sizeof(*c));
    c->disc.fd = -1;
    c->xbe.fd = -1;
    /* NC_RAM_MB=128 (debug kit size): a dev switch for memory investigations. */
    const char *mb = getenv("NC_RAM_MB");
    const uint32_t ram = mb && atoi(mb) == 128 ? 128u << 20 : NC_RAM_RETAIL;
    /* a savestate's kernel objects are where its boot made them (KERN v3
     * moved the pool's first range) */
    nc_kernel_pool_for(g_loadstate[0] ? nc_kern_state_kprocess(g_loadstate) : 0);
    if (!nc_vm_init(&c->vm, ram) || !nc_kernel_init(&c->kernel, &c->vm)) {
        if (disc_fd >= 0) {
            close(disc_fd);
        }
        return boot_failed("The emulated console couldn't be set up.");
    }
    if (fail_at("kernel")) {
        if (disc_fd >= 0) {
            close(disc_fd);
        }
        return boot_failed("The emulated console couldn't be set up.");
    }
    if (g_relaunch) {
        /* the last title's kept memory (D3D's saved display), where it was */
        nc_kernel_av_saved_set(nc_mm_keep_restore(&c->kernel));
    }
    nc_dev_nv2a_init();
    nc_dev_apu_init(&c->vm); /* before the catch-all, which would claim its window */
    nc_dev_ohci_init(&c->vm);
    nc_dev_aci_init(&c->vm);
    nc_net_start();          /* the network the NIC finds, before it */
    nc_dev_nvnet_init(&c->vm);
    nc_dev_misc_init();
    /* The dashboard boots with no disc (the tray empty); a title from the
     * disc, or the dashboard relaunched from one, has it. */
    if (disc_fd >= 0) {
        if (!nc_disc_open(&c->disc, disc_fd)) {
            close(disc_fd);
            return boot_failed("The game's file isn't an Xbox disc image.");
        }
        if (fail_at("disc")) {
            return boot_failed("The game's file isn't an Xbox disc image.");
        }
        nc_mods_install(&c->disc);      /* the mods' files and XBE patches, the cheats */
    } else if (!g_dashboard && !g_loadstate[0]) {
        return boot_failed("The game's file can't be opened.");
    }
    /* A relaunch (XLaunchNewImage, NativeCoreActivity starts the core again):
     * the XBE the saved launch data page names, and the page for the title. */
    static uint8_t page[NC_LAUNCH_PAGE];
    bool have_page = false;
    char xbe_path[NC_LAUNCH_PAGE] = "\\default.xbe";
    if (g_dashboard) {
        snprintf(xbe_path, sizeof(xbe_path), "%s", NC_DASHBOARD_PATH);
        snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", NC_DASHBOARD_PATH);
    }
    if (g_loadstate[0]) {
        /* a savestate: the XBE it was running (a relaunched title's own, the
         * dashboard's) */
        NcStateInfo info;
        static const char cd[] = "\\Device\\CdRom0";
        static const char hd[] = "\\Device\\Harddisk0\\";
        if (!nc_state_read_info(g_loadstate, &info)) {
            nc_hle_set_fatal(NC_FATAL_STATE, "the savestate can't be read");
            return false;
        }
        if ((!strncasecmp(info.xbe_path, cd, sizeof(cd) - 1) && info.xbe_path[sizeof(cd) - 1] == '\\') ||
            !strncasecmp(info.xbe_path, hd, sizeof(hd) - 1)) {
            snprintf(xbe_path, sizeof(xbe_path), "%s", info.xbe_path);
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", info.xbe_path);
        }
        nc_log("hakuX-native-frontier", "savestate: booting %s for %s", xbe_path, g_loadstate);
    } else if (g_relaunch) {
        char file[512];
        nc_kernel_relaunch_file(file, sizeof(file));
        FILE *fp = fopen(file, "rb");
        if (fp) {
            have_page = fread(page, 1, NC_LAUNCH_PAGE, fp) == NC_LAUNCH_PAGE;
            fclose(fp);
            unlink(file);
        }
        if (have_page && nc_kernel_launch_xbe(page, xbe_path, sizeof(xbe_path))) {
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", xbe_path);
            nc_log("hakuX-native-frontier", "relaunch: %s (launch type %u)", c->kernel.image_path,
                   *(const uint32_t *)page);
        } else {
            have_page = false;
            snprintf(xbe_path, sizeof(xbe_path), "%s", g_dashboard ? NC_DASHBOARD_PATH : "\\default.xbe");
            snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", g_dashboard ? NC_DASHBOARD_PATH : "");
            nc_log("hakuX-native-frontier", "relaunch: no launch data page, booting %s", xbe_path);
        }
    }
    /* a disc XBE with a title update installed: the update (a savestate's
     * memory is the XBE it ran, booted as it was) */
    static char update[NC_LAUNCH_PAGE];
    if (!g_loadstate[0] && disc_fd >= 0 && title_update(&c->disc, xbe_path, update, sizeof(update))) {
        snprintf(xbe_path, sizeof(xbe_path), "%s", update);
        snprintf(c->kernel.image_path, sizeof(c->kernel.image_path), "%s", update);
    }
    /* the frontend's XBE patch for the XBE booting (not a savestate's, whose
     * memory holds the code it ran) */
    if (!g_loadstate[0]) {
        nc_mods_xbe_patches(disc_fd >= 0 ? &c->disc : NULL, xbe_path);
    }
    if (!nc_xbe_load(&c->vm, &c->disc, xbe_path, &c->kernel, &c->xbe)) {
        char why[256], host[512];
        if (!strcasecmp(xbe_path, NC_DASHBOARD_PATH) && !nc_io_host_file(xbe_path, host, sizeof(host))) {
            return boot_failed("The dashboard isn't installed: its files (xboxdash.xbe and its folders) go on "
                               "the hard disk's C: drive.");
        }
        const char *name = strrchr(xbe_path, '\\');
        snprintf(why, sizeof(why), "The game's program (%s) can't be loaded: it's damaged, or hakuX doesn't support "
                 "it yet.", name ? name + 1 : xbe_path);
        return boot_failed(why);
    }
    if (fail_at("xbe")) {
        return boot_failed("The game's program can't be loaded.");
    }
    nc_kernel_title_loaded(&c->kernel, &c->disc, &c->xbe.file, c->xbe.fd, c->xbe.info.base,
                           c->xbe.info.sizeof_image);
    if (have_page) {
        nc_kernel_launch_page_restore(&c->kernel, page);
    }
    nc_kernel_title_fixups(&c->kernel, &c->xbe.info);

    NcCpuConfig cfg = {
        .membase = c->vm.base,
        .sysop = nc_kernel_sysop,
        .opaque = &c->kernel,
        .hook_query = core_hook_query,
        .idt_base = c->kernel.idt_va,
        .idt_limit = 0x7FF,
    };
    nc_core_cpu_backend(c, &cfg);
    c->cpu = nc_cpu_create(&cfg, c->kernel.kpcr_va);
    if (!c->cpu || fail_at("cpu")) {
        return boot_failed("The emulated CPU couldn't start.");
    }
    nc_mods_bind(&c->vm, c->cpu, &c->xbe.info);

    uint32_t stack = MAIN_STACK_SIZE;
    if (c->xbe.stack_commit > stack) {
        stack = nc_align_up(c->xbe.stack_commit, NC_PAGE);
    }
    g_boot = nc_kernel_boot_thread(&c->kernel, c->cpu, c->xbe.info.entry, stack);
    if (!g_boot || fail_at("thread")) {
        return boot_failed("The emulated CPU couldn't start.");
    }
    nc_log("hakuX-native-frontier", "stage S1 entry %08x, stack %08x-%08x, fs %08x",
           c->xbe.info.entry, g_boot->stack_limit, g_boot->stack_base, c->kernel.kpcr_va);

    /* The HLE GPU layer: its hooks come from the title's signature scan. */
    c->kernel.hle_hook = nc_hle_hook;
    c->kernel.hle_after_sysop = nc_hle_after_sysop;
    nc_hle_boot(&c->kernel, &c->vm, c->cpu, &g_boot->ctx);
    if (fail_at("hle")) {
        return boot_failed("The renderer couldn't start.");
    }
    nc_state_attach(c);
    if (g_loadstate[0]) {
        const char *err = nc_state_load(g_loadstate);
        if (err) {
            char why[384];
            snprintf(why, sizeof(why), "The savestate couldn't be loaded: %s.", err);
            nc_hle_set_fatal(NC_FATAL_STATE, why);
            return false;
        }
        nc_kernel_loaded_fixups(&c->kernel);
        nc_mods_after_load(&c->kernel, c->kernel.image_path);
        g_loaded = true;
    }
    return true;
}

void nc_core_run(NcCore *c)
{
    /* The guest thread's short host waits (a Sleep(0) poll that went on, the
     * idle CPU) end when they should: Linux lets a normal thread's timed
     * wait run 50 us late by default. */
    prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
    /* a savestate: the thread it was running, or the scheduler first */
    const NcCpuContext *start = g_loaded ? nc_kstate_start_context(&c->kernel) : &g_boot->ctx;
    nc_cpu_run(c->cpu, start ? start : &g_boot->ctx);
    const NcKernel *k = &c->kernel;
    if (k->stop) {
        nc_log("hakuX-native-frontier", "stop: %s", k->stop_reason);
    } else {
        nc_log("hakuX-native-frontier", "stop: guest executed HLT");
    }
    unsigned threads = 0;
    for (unsigned i = 0; i < NC_MAX_THREADS; i++) {
        threads += k->threads[i].state != NC_T_FREE;
    }
    const NcKExport *first = nc_kernel_export(k->first_ordinal);
    nc_log("hakuX-native-frontier", "summary stage=%s kernel_calls=%llu first_call=%s threads=%u",
           k->first_ordinal ? "S2" : "S1", (unsigned long long)k->calls,
           first ? first->name : "-", threads);
}

/* The KVM backend's view of the core (#56; FEX ignores it): guest memory,
 * the fault path, the devices, CPUID and the hooked addresses. */
static NcCore *g_cpu_core;

static NcFrame *kvm_access_frame(void *opaque)
{
    return opaque;
}

static uint32_t kvm_access_eip(void *opaque)
{
    return nc_frame_eip(opaque);
}

static int kvm_fault(void *opaque, NcFrame *f, uint32_t va, bool write, const char **why)
{
    const NcGuestAccess a = {.va = va, .page_fault = true, .write = write,
                             .frame = kvm_access_frame, .eip = kvm_access_eip, .opaque = f};
    return (int)nc_core_guest_fault(g_cpu_core, &a, why);
}

/* A device store that asks to run again (the APU's voice locks, which FEX's
 * x86mmio sites re-run with interrupts taken in between) is made again
 * here: KVM has already completed the guest's instruction. */
static bool kvm_mmio(uint32_t addr, unsigned bytes, bool store, uint64_t *v)
{
    const uint64_t value = *v;
    for (unsigned tries = 0;; tries++) {
        if (!nc_mmio_access(addr, bytes, store, v)) {
            return false;
        }
        if (!store || !nc_mmio_take_retry()) {
            return true;
        }
        *v = value;
        if (tries > 16) {
            sched_yield();
        }
    }
}

void nc_core_cpu_backend(NcCore *c, NcCpuConfig *cfg)
{
    g_cpu_core = c;
    cfg->vm = &c->vm;
    cfg->fault = kvm_fault;
    cfg->mmio = kvm_mmio;
    cfg->mmio_window = nc_mmio_window;
    cfg->port_in = nc_port_in;
    cfg->port_out = nc_port_out;
    cfg->cpuid = nc_kernel_cpuid;
    cfg->hook_list = nc_hle_hook_list;
}

/* Hooked guest addresses: the HLE GPU's functions and device-access sites. */
static bool core_hook_query(void *opaque, uint32_t eip)
{
    return nc_x86mmio_is_site(eip) || nc_hle_hook_query(opaque, eip);
}

/* A fault in translated code the core can't take as the guest's: where the
 * guest was, and what it was doing, before the process goes down with it.
 * Faults elsewhere go on to ART's handlers unreported (its null checks). */
static bool fault_report(NcCore *c, uintptr_t pc, uint64_t va, const char *what)
{
    /* FEX maps a pc outside its code buffer to the vCPU's current EIP, not
     * 0: every fault in the process took this for the guest's, ART's null
     * checks on the UI thread too (a Galaxy Tab S9, Orta: the dumps, the UI
     * thread with them, then an ANR). Only translated code's. */
    uint32_t eip;
    if (!nc_cpu_sample_pc(c->cpu, pc, &eip) || !eip) {
        return false;
    }
    nc_log("hakuX-native-frontier", "host fault: %s at guest %08llx, eip %08x (host pc %p)", what,
           (unsigned long long)va, eip, (void *)pc);
    nc_kernel_dump_recent(-1);
    nc_kernel_dump_threads(&c->kernel, -1);
    return false;
}

/* bumped from the fault handlers of any thread (atomically), read by the stats */
uint64_t g_nc_faults_callret, g_nc_faults_dirty, g_nc_faults_mmio, g_nc_faults_unaligned;

static struct {
    uint32_t eip, n;
} g_mfs[16];

static void mmio_fault_sample(uint32_t eip)
{
    if (__atomic_load_n(&g_nc_faults_mmio, __ATOMIC_RELAXED) & 63) {
        return;
    }
    unsigned k = 0;
    while (k < 16 && g_mfs[k].n && g_mfs[k].eip != eip) {
        k++;
    }
    if (k < 16) {
        g_mfs[k].eip = eip;
        g_mfs[k].n++;
    }
}

unsigned nc_mmio_fault_top(uint32_t *eips, uint32_t *counts, unsigned max)
{
    unsigned n = 0;
    for (; n < max; n++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_mfs[k].n && (best < 0 || g_mfs[k].n > g_mfs[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        eips[n] = g_mfs[best].eip;
        counts[n] = g_mfs[best].n * 64;
        g_mfs[best].n = 0;
    }
    memset(g_mfs, 0, sizeof(g_mfs));
    return n;
}

void nc_mmio_fault_sites(char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = 0;
    for (unsigned r = 0; r < 6 && o + 24 < len; r++) {
        int best = -1;
        for (unsigned k = 0; k < 16; k++) {
            if (g_mfs[k].n && (best < 0 || g_mfs[k].n > g_mfs[best].n)) {
                best = (int)k;
            }
        }
        if (best < 0) {
            break;
        }
        o += (size_t)snprintf(buf + o, len - o, " %08x:%u", g_mfs[best].eip, g_mfs[best].n * 64);
        g_mfs[best].n = 0;
    }
    memset(g_mfs, 0, sizeof(g_mfs));
}

enum NcGuestFault nc_core_guest_fault(NcCore *c, const NcGuestAccess *a, const char **why)
{
    *why = NULL;
    if (!a->wrapped) {
        /* NC_WATCHPA: a guest store to the watched RAM, with its EIP */
        const uint32_t gpa = nc_vm_phys(&c->vm, a->va);
        if (gpa != UINT32_MAX && nc_hle_watch_pa(gpa)) {
            nc_hle_watch_store(a->va, gpa, a->eip(a->opaque), 0);
            static unsigned framed;
            NcFrame *wf = framed < 20 ? a->frame(a->opaque) : NULL;
            if (wf) {
                /* the stack's words that look like the title's code: return addresses */
                framed++;
                char fr[400];
                int n = 0;
                const uint32_t esp = nc_frame_gpr(wf, NC_ESP);
                for (uint32_t i = 0; i < 160 && n < (int)sizeof(fr) - 12; i++) {
                    const uint32_t w_at = esp + 4 * i;
                    if (nc_vm_phys(&c->vm, w_at) == UINT32_MAX) {
                        break;
                    }
                    const uint32_t w = nc_vm_rd32(&c->vm, w_at);
                    if (w >= 0x11000 && w < 0x160000) {
                        n += snprintf(fr + n, sizeof(fr) - (size_t)n, " %x:%08x", i * 4, w);
                    }
                }
                nc_log("hakuX-native-watch", "  esp %08x stack%s", esp, fr);
            }
        }
        /* With a decoder (FEX's backend), an access it can't decode counts
         * as a store; without one, the CPU's write bit is the answer. */
        if (nc_hle_dirty_guest_fault(a->va, a->info ? true : a->write, a->info, a->ucontext)) {
            __atomic_fetch_add(&g_nc_faults_dirty, 1, __ATOMIC_RELAXED);
            return NC_GF_RETRY;
        }
        if (a->info && nc_mmio_fault(&c->vm, a->info, a->ucontext)) {
            __atomic_fetch_add(&g_nc_faults_mmio, 1, __ATOMIC_RELAXED);
            const uint32_t eip = a->eip(a->opaque);
            mmio_fault_sample(eip);
            /* From translated code: that instruction is hooked from now on. */
            nc_x86mmio_note(eip);
            return NC_GF_RETRY;
        }
    }
    if (!a->page_fault) {
        *why = "outside the guest";
        return NC_GF_DECLINED;
    }
    /* A guest page the guest doesn't have: the title's page tables may map
     * it since the last flush (the access runs again), or else from
     * translated code the title's page-fault handler, if it has one, maps it.
     * An access past the window's ends is the guest's at the wrapped address
     * (Forza after a race: [0xFFFFFFC4 + 0x60], a NULL list entry's field):
     * the title's fault when that page is missing, as on the Xbox. */
    if (nc_vm_phys(&c->vm, a->va) != UINT32_MAX) {
        *why = a->wrapped ? "wrapped, to a mapped page" : "mapped page";
        return NC_GF_DECLINED;
    }
    if (nc_paging_fault(&c->kernel, a->va)) {
        /* mapped now; the wrapped access would only fault again */
        if (a->wrapped) {
            *why = "wrapped, to a page the tables map";
            return NC_GF_DECLINED;
        }
        return NC_GF_RETRY;
    }
    NcFrame *f = a->frame(a->opaque);
    if (!f) {
        nc_kernel_call_fault(&c->kernel, a->va, a->write);
        return NC_GF_DECLINED;
    }
    if (a->wrapped) {
        nc_log("hakuX-native-frontier", "guest fault at %08x (wrapped address), eip %08x", a->va,
               nc_frame_eip(f));
    }
    /* As the Xbox kernel: the title's page-fault handler, else the access
     * violation raised to the thread's SEH handlers (Serious Sam's streams
     * commit their pages there) */
    if (!nc_kernel_page_fault(&c->kernel, f, a->va, a->write) &&
        !nc_seh_access_violation(&c->kernel, f, a->va, a->write)) {
        *why = "the title doesn't handle it";
        return NC_GF_DECLINED;
    }
    return NC_GF_FRAME;
}

/* FEX's backend: a fault's guest frame and EIP, from its signal */
typedef struct FexFault {
    NcCpu *cpu;
    void *ucontext;
    uintptr_t pc;
} FexFault;

static NcFrame *fex_fault_frame(void *opaque)
{
    const FexFault *x = opaque;
    return nc_cpu_fault_frame(x->cpu, x->ucontext);
}

static uint32_t fex_fault_eip(void *opaque)
{
    const FexFault *x = opaque;
    return nc_cpu_guest_eip_at(x->cpu, x->pc);
}

bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext)
{
    if (sig == SIGILL) {
        return false;
    }
    const uintptr_t pc = nc_host_pc(ucontext);
    if (sig == SIGBUS && nc_cpu_unaligned_fault(c->cpu, info->si_code, ucontext)) {
        /* Doom 3 keeps a critical section at 0x00BFD88E: RtlEnterCriticalSection's
         * lock inc of its LockCount (0x00BFD89E) took the core down */
        if (__atomic_fetch_add(&g_nc_faults_unaligned, 1, __ATOMIC_RELAXED) < 4) {
            nc_log("hakuX-native", "unaligned atomic at guest %08llx, eip %08x",
                   (unsigned long long)nc_vm_guest_of(&c->vm, info->si_addr), nc_cpu_guest_eip_at(c->cpu, pc));
        }
        return true;
    }
    if (nc_cpu_callret_fault(c->cpu, (uintptr_t)info->si_addr, ucontext)) {
        __atomic_fetch_add(&g_nc_faults_callret, 1, __ATOMIC_RELAXED);
        return true;
    }
    const uint64_t guest = nc_vm_guest_of(&c->vm, info->si_addr);
    const uint64_t wrapped = guest == UINT64_MAX ? nc_vm_guest_of_wrapped(&c->vm, info->si_addr) : UINT64_MAX;
    if (guest == UINT64_MAX && wrapped == UINT64_MAX) {
        return fault_report(c, pc, (uint64_t)(uintptr_t)info->si_addr, "outside the guest");
    }
    FexFault x = {c->cpu, ucontext, pc};
    const NcGuestAccess a = {
        .va = (uint32_t)(wrapped != UINT64_MAX ? wrapped : guest),
        .wrapped = wrapped != UINT64_MAX,
        .page_fault = sig == SIGSEGV,
        .write = nc_cpu_fault_is_write(ucontext),
        .info = info,
        .ucontext = ucontext,
        .frame = fex_fault_frame,
        .eip = fex_fault_eip,
        .opaque = &x,
    };
    const char *why;
    switch (nc_core_guest_fault(c, &a, &why)) {
    case NC_GF_RETRY:
        return true;
    case NC_GF_FRAME:
        nc_cpu_fault_resume(c->cpu, ucontext);
        return true;
    case NC_GF_DECLINED:
        break;
    }
    if (!why) {
        return false;
    }
    return fault_report(c, pc, a.page_fault ? a.va : (uint64_t)(uintptr_t)info->si_addr, why);
}

/* ---------------------------------------------------------------- stop */

void nc_core_request_stop(NcCore *c)
{
    nc_kernel_host_stop(&c->kernel);
}

void nc_core_halt(NcCore *c)
{
    /* The CPU has left (nc_core_run returned): devices paused with the
     * emulation leave their pause, see their stop and end. */
    nc_pause_set(false);
    nc_dev_nv2a_stop();
    nc_hle_native_stop();
    nc_dev_apu_stop();
    nc_dev_ohci_stop();
    nc_dev_aci_stop();
    nc_dev_nvnet_stop();
    nc_net_stop();
    nc_kernel_clock_stop();
    nc_io_stop();
}

void nc_core_destroy(NcCore *c)
{
    nc_dev_nv2a_free();
    nc_dev_apu_free();
    nc_dev_ohci_free();
    nc_dev_aci_free();
    nc_dev_nvnet_free();
    nc_cpu_destroy(c->cpu);
    c->cpu = NULL;
    nc_kernel_destroy(&c->kernel);
    if (c->xbe.fd >= 0) {
        close(c->xbe.fd);               /* hard-disk XBE descriptor opened by nc_xbe_load */
    }
    nc_disc_close(&c->disc);
    nc_vm_destroy(&c->vm);
    nc_mmio_reset();
    nc_irq_reset();
    nc_mods_reset();
    nc_hle_native_reset();
    memset(c, 0, sizeof(*c));
    c->disc.fd = -1;
    c->xbe.fd = -1;
    c->vm.ram_fd = -1;
    g_boot = NULL;
    g_loaded = false;
    g_loadstate[0] = 0;
    memset(g_mfs, 0, sizeof(g_mfs));
    __atomic_store_n(&g_nc_faults_callret, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_nc_faults_dirty, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_nc_faults_mmio, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_nc_faults_unaligned, 0, __ATOMIC_RELAXED);
}
