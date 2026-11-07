/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test directory lookup and listing on deep and looping XDVDFS trees. The
 * listing walks run on a small thread stack, so recursion would overflow it. */
#include "native/core/xdvdfs.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR NC_XDVDFS_SECTOR
#define CHAIN  80

static int g_fail;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_fail += !ok;
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}

/* An XISO of 35 sectors: the volume descriptor at 32, the root at 33 (one
 * chain of CHAIN files and LOOP, each the right child of the one before),
 * LOOP's looping tree at 34. */
static int image(void)
{
    char path[] = "/tmp/xdvdfs-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    unlink(path);
    static uint8_t img[35 * SECTOR];
    memset(img, 0, sizeof(img));
    uint8_t *vd = img + 32 * SECTOR;
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    wr32(vd + 20, 33);
    wr32(vd + 24, SECTOR);
    uint8_t *root = img + 33 * SECTOR;
    memset(root, 0xFF, SECTOR);         /* padding after the entries */
    for (unsigned i = 0; i < CHAIN; i++) {
        uint8_t *e = root + i * 20;     /* 14 + a 3-byte name, 4-aligned */
        wr16(e, 0);
        wr16(e + 2, i + 1 < CHAIN ? (uint16_t)((i + 1) * 20 / 4) : 0);
        wr32(e + 4, 1000 + i);
        wr32(e + 8, i);
        e[12] = 0x20;
        e[13] = 3;
        char name[4];
        snprintf(name, sizeof(name), "F%02u", i);
        memcpy(e + 14, name, 3);
    }
    /* the chain's end: LOOP, a directory whose tree loops (AAA's right
     * child BBB is its own left child) */
    wr16(root + (CHAIN - 1) * 20 + 2, (uint16_t)(CHAIN * 20 / 4));
    uint8_t *e = root + CHAIN * 20;
    wr32(e + 4, 34);
    wr32(e + 8, SECTOR);
    e[12] = 0x10;
    e[13] = 4;
    memcpy(e + 14, "LOOP", 4);
    uint8_t *loop = img + 34 * SECTOR;
    memset(loop, 0xFF, SECTOR);
    for (unsigned k = 0; k < 2; k++) {
        uint8_t *x = loop + k * 20;
        wr16(x, k ? 20 / 4 : 0);
        wr16(x + 2, k ? 0 : 20 / 4);
        wr32(x + 4, 2000 + k);
        wr32(x + 8, 1);
        x[12] = 0x20;
        x[13] = 3;
        memcpy(x + 14, k ? "BBB" : "AAA", 3);
    }
    /* F79's 79 bytes at its sector (the overlay's patches go over them) */
    uint8_t data[79];
    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 1);
    }
    if (write(fd, img, sizeof(img)) != (ssize_t)sizeof(img) ||
        pwrite(fd, data, sizeof(data), (off_t)1079 * SECTOR) != (ssize_t)sizeof(data)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void wr_entry(uint8_t *e, uint16_t left, uint16_t right, const char *name)
{
    wr16(e, left);
    wr16(e + 2, right);
    wr32(e + 4, 0);
    wr32(e + 8, 0);
    e[12] = 0x20;
    e[13] = (uint8_t)strlen(name);
    memcpy(e + 14, name, strlen(name));
}

/* A listing: the names in visit order, joined by spaces */
typedef struct List {
    char text[1200];
    unsigned n;
} List;

static bool list_add(void *ctx, const uint8_t *e, uint32_t off)
{
    List *l = ctx;
    size_t used = strlen(l->text);
    (void)off;
    snprintf(l->text + used, sizeof(l->text) - used, "%s%.*s", l->n ? " " : "", e[13], (const char *)e + 14);
    l->n++;
    return true;
}

static bool count(void *ctx, const uint8_t *e, uint32_t off)
{
    (void)e;
    (void)off;
    ++*(unsigned *)ctx;
    return true;
}

/* The walks that would need a deep stack, on a small one */
static void *small_stack(void *arg)
{
    int *fails = arg;
    /* 16 MiB of padding but for a left-child cycle: 0 -> 20 -> 20 */
    const uint32_t big = 16u << 20;
    uint8_t *dir = malloc(big);
    if (!dir) {
        ++*fails;
        return NULL;
    }
    memset(dir, 0xFF, big);
    wr_entry(dir, 20 / 4, 0, "TOP");
    wr_entry(dir + 20, 20 / 4, 0, "SELF");
    unsigned n = 0;
    *fails += !(nc_xdvdfs_walk(dir, big, count, &n) && n == 2);
    printf("%s a 16 MiB directory with a left-child cycle: each entry once (%u)\n", n == 2 ? "PASS" : "FAIL", n);
    /* the longest chain of left children 16-bit offsets reach */
    const unsigned chain = 0x3FFFCu / 20;
    memset(dir, 0xFF, big);
    for (unsigned i = 0; i < chain; i++) {
        char name[8];
        snprintf(name, sizeof(name), "%05u", i);
        wr_entry(dir + i * 20, i + 1 < chain ? (uint16_t)((i + 1) * 20 / 4) : 0, 0, name);
    }
    n = 0;
    const bool walked = nc_xdvdfs_walk(dir, chain * 20, count, &n);
    *fails += !(walked && n == chain);
    printf("%s a %u-deep chain of left children: every entry (%u)\n", walked && n == chain ? "PASS" : "FAIL",
           chain, n);
    free(dir);
    return NULL;
}

int main(void)
{
    int fd = image();
    const int fd_listing = fd >= 0 ? dup(fd) : -1;
    const int fd_patch = fd >= 0 ? dup(fd) : -1;
    NcDisc d;
    if (fd < 0 || !nc_disc_open(&d, fd)) {
        printf("FAIL the image\n");
        return 1;
    }
    NcDiscFile f;
    check(nc_disc_find(&d, "F00", &f) && f.sector == 1000, "the chain's first entry");
    check(nc_disc_find(&d, "\\f40", &f) && f.sector == 1040, "its 41st, any case");
    check(nc_disc_find(&d, "F79", &f) && f.sector == 1079 && f.size == 79, "its 80th, 79 deep");
    check(nc_disc_find(&d, "LOOP\\BBB", &f) && f.sector == 2001, "a file in the looping tree");
    check(!nc_disc_find(&d, "LOOP\\CCC", &f), "a missing one there: not found, and the search ends");
    check(!nc_disc_find(&d, "F80", &f), "a missing one in the root");
    nc_disc_close(&d);

    /* listings, in the disc's order */
    static uint8_t img_root[SECTOR];
    lseek(fd_listing, 33 * SECTOR, SEEK_SET);
    List l = {{0}, 0};
    const bool root_read = read(fd_listing, img_root, SECTOR) == SECTOR;
    check(root_read && nc_xdvdfs_walk(img_root, SECTOR, list_add, &l) && l.n == CHAIN + 1 &&
              !strncmp(l.text, "F00 F01 F02", 11) && !strcmp(l.text + strlen(l.text) - 8, "F79 LOOP"),
          "the root's listing: the chain's 80 files, then LOOP");
    static uint8_t img_loop[SECTOR];
    List ll = {{0}, 0};
    const bool loop_read = read(fd_listing, img_loop, SECTOR) == SECTOR;
    check(loop_read && nc_xdvdfs_walk(img_loop, SECTOR, list_add, &ll) && !strcmp(ll.text, "AAA BBB"),
          "LOOP's listing: each entry once");
    close(fd_listing);

    /* patches all at once (an XBE patch's runs): one past the file's end
     * and none of them is applied; all in it and each is */
    NcDisc pd;
    check(fd_patch >= 0 && nc_disc_open(&pd, fd_patch), "the image again");
    NcDiscFile pf;
    uint8_t orig[79], got[79];
    check(nc_disc_find(&pd, "F79", &pf) && nc_disc_read(&pd, &pf, 0, orig, sizeof(orig)), "F79's 79 bytes");
    static const uint8_t ab[2] = {0xAA, 0xBB}, cde[3] = {0xCC, 0xDD, 0xEE};
    const NcDiscRun past[] = {{4, 2, ab}, {10, 3, cde}, {77, 3, cde}};
    check(!nc_disc_overlay_patch_all(&pd, "F79", past, 3) && nc_disc_read(&pd, &pf, 0, got, sizeof(got)) &&
              !memcmp(got, orig, sizeof(got)),
          "patches with one past the end: none applied");
    const NcDiscRun in[] = {{4, 2, ab}, {10, 3, cde}, {76, 3, cde}};
    check(nc_disc_overlay_patch_all(&pd, "F79", in, 3) && nc_disc_read(&pd, &pf, 0, got, sizeof(got)) &&
              !memcmp(got + 4, ab, 2) && !memcmp(got + 10, cde, 3) && !memcmp(got + 76, cde, 3) &&
              !memcmp(got, orig, 4) && !memcmp(got + 6, orig + 6, 4) && !memcmp(got + 13, orig + 13, 63),
          "patches within it: each applied, the rest as the disc has it");
    nc_disc_close(&pd);

    pthread_attr_t attr;
    pthread_t th;
    int fails = 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN > 0x20000 ? PTHREAD_STACK_MIN : 0x20000);
    if (pthread_create(&th, &attr, small_stack, &fails) != 0) {
        fails++;
    } else {
        pthread_join(th, NULL);
    }
    g_fail += fails;
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
