/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — XBE patches from the frontend's cheats (xbe_patch.h).
 */
#include "native/core/xbe_patch.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                                        : -1;
}

/* eight hex digits at *s */
static bool hex8(const char **s, uint32_t *v)
{
    uint32_t x = 0;
    for (int i = 0; i < 8; i++) {
        const int d = hex_digit((*s)[i]);
        if (d < 0) {
            return false;
        }
        x = x << 4 | (uint32_t)d;
    }
    *s += 8;
    *v = x;
    return true;
}

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool nc_xbe_patch_is(const char *code)
{
    while (code && blank(*code)) {
        code++;
    }
    return code && !strncmp(code, NC_XBE_PATCH_TAG, sizeof(NC_XBE_PATCH_TAG) - 1) &&
           (blank(code[sizeof(NC_XBE_PATCH_TAG) - 1]) || !code[sizeof(NC_XBE_PATCH_TAG) - 1]);
}

bool nc_xbe_patch_parse(const char *code, NcXbePatch *out, char *err, size_t errlen)
{
    if (!nc_xbe_patch_is(code)) {
        return fail(err, errlen, "not an " NC_XBE_PATCH_TAG " code");
    }
    const char *s = strstr(code, NC_XBE_PATCH_TAG) + sizeof(NC_XBE_PATCH_TAG) - 1;
    while (blank(*s)) {
        s++;
    }
    if (!hex8(&s, &out->crc) || *s++ != ':' || !hex8(&s, &out->size)) {
        return fail(err, errlen, "no CRC32:SIZE (eight hex digits each)");
    }
    out->nruns = 0;
    uint32_t end = 4;                   /* the magic stays */
    while (*s == '+') {
        s++;
        const unsigned k = out->nruns + 1;
        if (out->nruns == NC_XBE_PATCH_RUNS_MAX) {
            return fail(err, errlen, "more than %d runs", NC_XBE_PATCH_RUNS_MAX);
        }
        NcXbePatchRun *r = &out->runs[out->nruns];
        if (!hex8(&s, &r->off) || *s++ != ':') {
            return fail(err, errlen, "run %u: no OFFSET: (eight hex digits)", k);
        }
        unsigned n = 0;
        for (; hex_digit(s[0]) >= 0; s += 2) {
            if (hex_digit(s[1]) < 0) {
                return fail(err, errlen, "run %u: an odd hex digit", k);
            }
            if (n == NC_XBE_PATCH_RUN_MAX) {
                return fail(err, errlen, "run %u: more than %d bytes", k, NC_XBE_PATCH_RUN_MAX);
            }
            r->bytes[n++] = (uint8_t)(hex_digit(s[0]) << 4 | hex_digit(s[1]));
        }
        if (!n) {
            return fail(err, errlen, "run %u: no bytes", k);
        }
        r->len = (uint8_t)n;
        if (r->off < 4) {
            return fail(err, errlen, "run %u: below offset 4 (the XBE's magic)", k);
        }
        if (r->off < end && out->nruns) {
            return fail(err, errlen, "run %u: not after run %u (sorted, not overlapping)", k, k - 1);
        }
        if ((uint64_t)r->off + n > out->size) {
            return fail(err, errlen, "run %u: past the XBE's size", k);
        }
        end = r->off + n;
        out->nruns++;
    }
    while (blank(*s)) {
        s++;
    }
    if (*s) {
        return fail(err, errlen, "\"%.12s\" after run %u", s, out->nruns);
    }
    if (!out->nruns) {
        return fail(err, errlen, "no runs");
    }
    return true;
}

/* ---- the frontend's ---- */

typedef struct Front {
    NcXbePatch *p;              /* NULL: off */
    uint64_t hash;              /* its code's */
} Front;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Front g_front[NC_XBE_PATCH_FRONT_MAX];

static uint64_t code_hash(const char *code)
{
    uint64_t h = 1469598103934665603ull;        /* FNV-1a */
    for (const char *p = code; *p; p++) {
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    }
    return h ? h : 1;
}

bool nc_xbe_patch_front_set(unsigned index, bool on, const char *code, char *err, size_t errlen)
{
    if (index >= NC_XBE_PATCH_FRONT_MAX) {
        return fail(err, errlen, "too many cheats");
    }
    NcXbePatch *p = NULL;
    bool ok = true;
    if (on) {
        p = malloc(sizeof(*p));
        ok = p ? nc_xbe_patch_parse(code, p, err, errlen) : fail(err, errlen, "out of memory");
        if (!ok) {
            free(p);
            p = NULL;
        }
    }
    pthread_mutex_lock(&g_mu);
    free(g_front[index].p);
    g_front[index].p = p;
    /* not the index's: RetroArch numbers the cheats that are on, so turning
     * an earlier one off moves the same patch to another index */
    g_front[index].hash = p ? code_hash(code) : 0;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void nc_xbe_patch_front_reset(void)
{
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < NC_XBE_PATCH_FRONT_MAX; i++) {
        free(g_front[i].p);
        g_front[i] = (Front){0};
    }
    pthread_mutex_unlock(&g_mu);
}

static uint64_t hash_locked(void)
{
    uint64_t h = 0;
    for (unsigned i = 0; i < NC_XBE_PATCH_FRONT_MAX; i++) {
        h += g_front[i].hash;
    }
    return h;
}

uint64_t nc_xbe_patch_front_hash(void)
{
    pthread_mutex_lock(&g_mu);
    const uint64_t h = hash_locked();
    pthread_mutex_unlock(&g_mu);
    return h;
}

int nc_xbe_patch_front_match(uint32_t crc, uint32_t size, NcXbePatch *out, unsigned *index, unsigned *other,
                             uint64_t *hash)
{
    int found = 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < NC_XBE_PATCH_FRONT_MAX && found >= 0; i++) {
        const NcXbePatch *p = g_front[i].p;
        if (!p || p->crc != crc || p->size != size) {
            continue;
        }
        if (found) {
            *other = i;
            found = -1;
        } else {
            *out = *p;
            *index = i;
            found = 1;
        }
    }
    if (hash) {
        *hash = hash_locked();
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}

unsigned nc_xbe_patch_front_list(char *buf, size_t len)
{
    unsigned n = 0;
    size_t at = 0;
    if (len) {
        buf[0] = 0;
    }
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < NC_XBE_PATCH_FRONT_MAX; i++) {
        const NcXbePatch *p = g_front[i].p;
        if (p && at < len) {
            const int w = snprintf(buf + at, len - at, "%s%08X:%08X", n ? ", " : "", p->crc, p->size);
            at += w > 0 ? (size_t)w : 0;
        }
        n += p != NULL;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
