/*
 * hakuX native core — guest-managed page tables.
 *
 * Seed CR3 with the contiguous window's 4 MiB PDEs. Guest PTEs map
 * individual pages; large PDEs create RAM aliases. Apply changed entries
 * on INVLPG or CR3 reload, and resolve missing mappings on page faults.
 * This also supports edits used before a flush (Fable and Hitman 2).
 */
#include "native/kernel/kernel.h"

#include <stdlib.h>

#define PTE_PRESENT 0x001u
#define PDE_LARGE   0x080u
#define LARGE_PAGE  0x400000u
/* Window PDE flags: present, writable, accessed, dirty, large, global. */
#define PDE_WINDOW  0x1E3u

static uint32_t g_pd_pa = UINT32_MAX;
static uint32_t g_pd_applied[1024];
static uint32_t *g_pt_applied[1024]; /* per present PDE: the PTEs applied */

uint32_t nc_paging_cr3(NcKernel *k)
{
    if (g_pd_pa == UINT32_MAX) {
        const uint32_t pa = nc_vm_alloc_phys(k->vm, NC_PAGE, 0, k->vm->ram_size);
        if (pa == UINT32_MAX) {
            nc_log("hakuX-native", "paging: no page for the page directory");
            return 0;
        }
        memset(k->vm->ram + pa, 0, NC_PAGE);
        for (uint32_t i = 0; i < k->vm->ram_size / LARGE_PAGE; i++) {
            const uint32_t pde = i * LARGE_PAGE | PDE_WINDOW;
            nc_wr32(k->vm->ram + pa + 4 * ((NC_CONTIG_WINDOW >> 22) + i), pde);
            g_pd_applied[(NC_CONTIG_WINDOW >> 22) + i] = pde;
        }
        g_pd_pa = pa;
        nc_log("hakuX-native", "paging: the title reads CR3; page directory at pa %08x", pa);
    }
    return g_pd_pa;
}

/* Code translated from what was mapped at va goes with the mapping. */
static void unmap(NcKernel *k, uint32_t va, uint32_t len)
{
    nc_vm_unmap(k->vm, va, len);
    nc_cpu_invalidate(k->cpu, va, len);
}

static void map_page(NcKernel *k, uint32_t va, uint32_t pa, uint32_t len)
{
    if ((uint64_t)pa + len > k->vm->ram_size) {
        static unsigned s_warned;
        if (s_warned++ < 8) {
            nc_log("hakuX-native", "paging: %08x -> pa %08x is outside RAM: left unmapped", va, pa);
        }
        return;
    }
    nc_vm_map(k->vm, va, pa, len);
    for (uint32_t off = 0; off < len; off += NC_PAGE) {
        nc_vm_pin(k->vm, pa + off, true, 0); /* in the title's tables: it stays put */
    }
}

/* Map a large PDE as a RAM alias without pinning its physical pages. */
static void map_large(NcKernel *k, uint32_t va, uint32_t pa)
{
    if ((uint64_t)pa + LARGE_PAGE > k->vm->ram_size) {
        static unsigned s_warned;
        if (s_warned++ < 8) {
            nc_log("hakuX-native", "paging: %08x -> pa %08x (4 MiB) is outside RAM: left unmapped", va, pa);
        }
        return;
    }
    nc_vm_map_alias(k->vm, va, pa, LARGE_PAGE);
}

/* PDE i covers one of the core's windows of RAM, which stay as they are. */
static bool core_window(const NcKernel *k, unsigned i)
{
    const uint32_t va = i << 22;
    return (va >= NC_CONTIG_WINDOW && va - NC_CONTIG_WINDOW < k->vm->ram_size) ||
           (va >= NC_GPU_WINDOW && va - NC_GPU_WINDOW < k->vm->ram_size);
}

/* A page table no longer applied: its mapped entries go. */
static void drop_table(NcKernel *k, unsigned i)
{
    uint32_t *applied = g_pt_applied[i];
    for (unsigned j = 0; applied && j < 1024; j++) {
        if (applied[j] & PTE_PRESENT) {
            unmap(k, (i << 22) | (j << 12), NC_PAGE);
        }
        if (applied) {
            applied[j] = 0;
        }
    }
}

/* Applies PDE i and the page table it names. */
static void sync_pde(NcKernel *k, unsigned i)
{
    const uint8_t *ram = k->vm->ram;
    {
        const uint32_t pde = nc_rd32(ram + g_pd_pa + 4 * i), old = g_pd_applied[i];
        if (!(pde & PTE_PRESENT) && !(old & PTE_PRESENT)) {
            return;
        }
        if (core_window(k, i)) {
            if (pde != old) {
                static unsigned s_warned;
                if (s_warned++ < 8) {
                    nc_log("hakuX-native", "paging: PDE %03x of a window of RAM changed to %08x: not applied", i, pde);
                }
                g_pd_applied[i] = pde;
            }
            return;
        }
        const uint32_t va4m = i << 22;
        /* The PDE changed what it points at: undo the old one first. */
        if ((old & PTE_PRESENT) && pde != old) {
            if (old & PDE_LARGE) {
                unmap(k, va4m, LARGE_PAGE);
            } else if (!(pde & PTE_PRESENT) || (pde & PDE_LARGE) || ((pde ^ old) & ~0xFFFu)) {
                drop_table(k, i);
            }
        }
        g_pd_applied[i] = pde;
        if (!(pde & PTE_PRESENT)) {
            return;
        }
        if (pde & PDE_LARGE) {
            if (pde != old) {
                map_large(k, va4m, pde & 0xFFC00000u);
            }
            return;
        }
        const uint32_t pt_pa = pde & ~0xFFFu;
        if ((uint64_t)pt_pa + NC_PAGE > k->vm->ram_size) {
            return;
        }
        if (!g_pt_applied[i]) {
            g_pt_applied[i] = calloc(1024, sizeof(uint32_t));
            if (!g_pt_applied[i]) {
                return;
            }
        }
        uint32_t *applied = g_pt_applied[i];
        for (unsigned j = 0; j < 1024; j++) {
            const uint32_t pte = nc_rd32(ram + pt_pa + 4 * j);
            if (pte == applied[j]) {
                continue;
            }
            const uint32_t va = va4m | (j << 12);
            if (pte & PTE_PRESENT) {
                if (!(applied[j] & PTE_PRESENT)) {
                    map_page(k, va, pte & ~0xFFFu, NC_PAGE);
                } else if ((pte ^ applied[j]) & ~0xFFFu) {
                    unmap(k, va, NC_PAGE);
                    map_page(k, va, pte & ~0xFFFu, NC_PAGE);
                }
            } else if (applied[j] & PTE_PRESENT) {
                unmap(k, va, NC_PAGE);
            }
            applied[j] = pte;
        }
    }
}

void nc_paging_flush(NcKernel *k)
{
    if (g_pd_pa == UINT32_MAX) {
        return;
    }
    for (unsigned i = 0; i < 1024; i++) {
        sync_pde(k, i);
    }
}

bool nc_paging_fault(NcKernel *k, uint32_t va)
{
    if (g_pd_pa == UINT32_MAX) {
        return false;
    }
    sync_pde(k, va >> 22);
    return nc_vm_phys(k->vm, va) != UINT32_MAX;
}

/* ---------------------------------------------------------- savestates */

typedef struct NcPagingState {
    uint32_t pd_pa;
    uint32_t pd_applied[1024];
    uint32_t ntables;
} NcPagingState;

typedef struct NcPagingTable {
    uint32_t pde;
    uint32_t pte[1024];
} NcPagingTable;

#define NC_DEF_SECTION PAGE
#define NC_DEF_FILE "native/state/paging.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_page_state_save(NcBuf *b)
{
    static NcPagingState st;
    st.pd_pa = g_pd_pa;
    memcpy(st.pd_applied, g_pd_applied, sizeof(st.pd_applied));
    st.ntables = 0;
    for (unsigned i = 0; i < 1024; i++) {
        st.ntables += g_pt_applied[i] != NULL;
    }
    nc_rec_put(b, &nc_rec_NcPagingState, &st);
    for (unsigned i = 0; i < 1024; i++) {
        if (g_pt_applied[i]) {
            static NcPagingTable t;
            t.pde = i;
            memcpy(t.pte, g_pt_applied[i], sizeof(t.pte));
            nc_rec_put(b, &nc_rec_NcPagingTable, &t);
        }
    }
    return true;
}

bool nc_page_state_load(NcRd *r, unsigned v)
{
    static NcPagingState st;
    memset(&st, 0, sizeof(st));
    st.pd_pa = UINT32_MAX;
    nc_rec_get(r, &nc_rec_NcPagingState, &st, v);
    if (st.ntables > 1024) {
        return false;
    }
    for (unsigned i = 0; i < 1024; i++) {
        free(g_pt_applied[i]);
        g_pt_applied[i] = NULL;
    }
    g_pd_pa = st.pd_pa;
    memcpy(g_pd_applied, st.pd_applied, sizeof(g_pd_applied));
    for (unsigned n = 0; n < st.ntables; n++) {
        static NcPagingTable t;
        memset(&t, 0, sizeof(t));
        nc_rec_get(r, &nc_rec_NcPagingTable, &t, v);
        if (t.pde >= 1024 || g_pt_applied[t.pde]) {
            return false;
        }
        g_pt_applied[t.pde] = malloc(sizeof(t.pte));
        if (!g_pt_applied[t.pde]) {
            return false;
        }
        memcpy(g_pt_applied[t.pde], t.pte, sizeof(t.pte));
    }
    /* Restore alias tracking after loading the VM mappings. */
    NcKernel *k = nc_state_kernel();
    for (unsigned i = 0; k && i < 1024; i++) {
        const uint32_t pde = g_pd_applied[i];
        if ((pde & (PTE_PRESENT | PDE_LARGE)) == (PTE_PRESENT | PDE_LARGE) && !core_window(k, i)) {
            map_large(k, i << 22, pde & 0xFFC00000u);
        }
    }
    return !r->fail;
}

void nc_page_reset(void)
{
    g_pd_pa = UINT32_MAX;
    memset(g_pd_applied, 0, sizeof(g_pd_applied));
    for (unsigned i = 0; i < 1024; i++) {
        free(g_pt_applied[i]);
        g_pt_applied[i] = NULL;
    }
}
