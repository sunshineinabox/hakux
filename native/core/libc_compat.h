/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compatibility functions for older glibc versions.
 * AMD64 releases target Ubuntu 22.04 (glibc 2.35).
 * Newer glibc and Android's bionic use their own implementations. */
#ifndef NATIVE_CORE_LIBC_COMPAT_H
#define NATIVE_CORE_LIBC_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Bionic has no __GLIBC_PREREQ, so guard the version checks. */
#ifdef __GLIBC__
#if !__GLIBC_PREREQ(2, 36)
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

/* Use getrandom, falling back to /dev/urandom. */
static inline void arc4random_buf(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            r = fd >= 0 ? read(fd, p, n) : -1;
            if (fd >= 0) {
                close(fd);
            }
            if (r <= 0) {
                abort();
            }
        }
        p += r;
        n -= (size_t)r;
    }
}

static inline uint32_t arc4random(void)
{
    uint32_t v;
    arc4random_buf(&v, sizeof(v));
    return v;
}

/* Rejection sampling avoids modulo bias. */
static inline uint32_t arc4random_uniform(uint32_t upper)
{
    if (upper < 2) {
        return 0;
    }
    const uint32_t min = (uint32_t)-upper % upper;
    for (;;) {
        uint32_t r = arc4random();
        if (r >= min) {
            return r % upper;
        }
    }
}
#endif

#if !__GLIBC_PREREQ(2, 38)
/* Return the source length and copy at most size - 1 bytes. */
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}
#endif
#endif

#endif
