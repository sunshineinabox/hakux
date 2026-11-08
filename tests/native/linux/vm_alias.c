/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Alias overflow must reject new mappings without changing physical-page
 * ownership or protection. */
#include "native/core/vm.h"

#include <stdio.h>
#include <sys/mman.h>

#define OWNER  0x00100000u
#define VIEWS  0x10000000u
#define TABLE  1024u            /* vm.c's MAX_ALIASES */

static int g_fail;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

int main(void)
{
    NcVm vm;
    if (!nc_vm_init(&vm, NC_RAM_RETAIL)) {
        printf("FAIL vm init\n");
        return 1;
    }
    uint32_t pa;
    check(nc_vm_alloc_at(&vm, OWNER, NC_PAGE, &pa), "owner page mapped");

    unsigned mapped = 0;
    while (mapped < TABLE + 1 && nc_vm_map_alias(&vm, VIEWS + mapped * NC_PAGE, pa, NC_PAGE)) {
        mapped++;
    }
    check(mapped == TABLE, "the table holds MAX_ALIASES views, the next is refused");
    check(nc_vm_phys(&vm, VIEWS + TABLE * NC_PAGE) == UINT32_MAX, "the refused view is left unmapped");
    check(vm.rmap[pa / NC_PAGE] == OWNER, "the owner keeps its rmap entry");

    nc_vm_protect_page(&vm, pa, true);
    check(vm.vprot[OWNER / NC_PAGE] == PROT_READ, "protection reaches the owner");
    check(vm.vprot[VIEWS / NC_PAGE] == PROT_READ, "protection reaches a view");
    nc_vm_protect_page(&vm, pa, false);
    check(vm.vprot[OWNER / NC_PAGE] == (PROT_READ | PROT_WRITE), "unprotected again");

    check(nc_vm_unmap(&vm, VIEWS, NC_PAGE) && nc_vm_map_alias(&vm, VIEWS + TABLE * NC_PAGE, pa, NC_PAGE),
          "an unmapped view makes room for another");
    nc_vm_destroy(&vm);
    return g_fail ? 1 : 0;
}
