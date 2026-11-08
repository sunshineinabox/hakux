/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hakuX native core — a signal's host context (a ucontext_t *): the PC and
 * SP the fault handlers and reports read, on either host.
 */
#ifndef NATIVE_CORE_HOSTCTX_H
#define NATIVE_CORE_HOSTCTX_H

#include <stdint.h>
#include <ucontext.h>

static inline uintptr_t nc_host_pc(const void *ucontext)
{
#if defined(__aarch64__)
    return (uintptr_t)((const ucontext_t *)ucontext)->uc_mcontext.pc;
#else
    return (uintptr_t)((const ucontext_t *)ucontext)->uc_mcontext.gregs[REG_RIP];
#endif
}

static inline uintptr_t nc_host_sp(const void *ucontext)
{
#if defined(__aarch64__)
    return (uintptr_t)((const ucontext_t *)ucontext)->uc_mcontext.sp;
#else
    return (uintptr_t)((const ucontext_t *)ucontext)->uc_mcontext.gregs[REG_RSP];
#endif
}

#endif
