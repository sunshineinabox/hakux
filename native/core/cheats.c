/*
 * hakuX native core — cheats (cheats.h; the format in patches/mods/README.md).
 *
 * Each code line compiles to an op in one pool: a write, an if, else or end.
 * An address is a base and up to four pointer steps ([A]+N reads the 32-bit
 * pointer at A and adds N). The frontend's cheats have ops of their own.
 */
#include "native/core/cheats.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPS_MAX   8192
#define DEPTH_MAX 8
#define PTR_MAX   4

enum { OP_W, OP_IF, OP_ELSE, OP_END };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE, CMP_AND };

typedef struct Addr {
    uint32_t base;
    uint8_t steps;              /* pointers read */
    int32_t off[PTR_MAX];       /* added after each */
} Addr;

typedef struct Op {
    uint8_t kind;
    uint8_t width;              /* bytes: 1, 2, 4 */
    uint8_t isf;                /* a float (wf, iff) */
    uint8_t cmp;
    Addr a;
    uint32_t v;                 /* the value's bits */
} Op;

typedef struct Cheat {
    char name[64];
    int on;                     /* atomic */
    unsigned gen;               /* atomic: bumped each time it is turned on */
    unsigned done;              /* a once cheat: the gen it ran for (the run's thread) */
    bool once;
    unsigned first, n;          /* its ops */
    int depth;                  /* ifs open while its lines come in */
    uint64_t misses;
} Cheat;

static Op g_ops[OPS_MAX];
static unsigned g_nops;
static Cheat g_cheats[NC_CHEATS_MAX];
static unsigned g_n;

/* The frontend's cheats, by its index: changed in place (freed by a reset),
 * so the lock is held to change them and by the run, which skips them for
 * that vblank while it is taken. */
typedef struct Front {
    Op *ops;                    /* NULL: off */
    unsigned n;
    uint64_t misses;
    bool told;                  /* its misses reported (nc_cheat_front_missed) */
} Front;

static pthread_mutex_t g_front_mu = PTHREAD_MUTEX_INITIALIZER;
static Front g_front[NC_CHEATS_MAX];
static unsigned g_nfront;       /* the highest index on + 1 */

void nc_cheats_reset(void)
{
    g_nops = 0;
    g_n = 0;
    pthread_mutex_lock(&g_front_mu);
    for (unsigned i = 0; i < g_nfront; i++) {
        g_front[i].misses = 0;          /* the next boot's own */
        g_front[i].told = false;
    }
    pthread_mutex_unlock(&g_front_mu);
}

int nc_cheat_add(const char *name, bool on, bool once)
{
    if (g_n >= NC_CHEATS_MAX) {
        return -1;
    }
    Cheat *c = &g_cheats[g_n];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->on = on;
    c->gen = on;
    c->once = once;
    c->first = g_nops;
    return (int)g_n++;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* A number: 0x hex or decimal, an optional sign. */
static bool number(const char **s, int64_t *out)
{
    const char *p = skip_ws(*s);
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p = skip_ws(p + 1);
    }
    char *end;
    const bool hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    if (!(hex ? isxdigit((unsigned char)p[2]) : isdigit((unsigned char)*p))) {
        return false;
    }
    const unsigned long long v = strtoull(hex ? p + 2 : p, &end, hex ? 16 : 10);
    if (v > 0xFFFFFFFFull) {
        return false;
    }
    *out = neg ? -(int64_t)v : (int64_t)v;
    *s = end;
    return true;
}

/* A, [A]+N, [[A]+N]-M ... */
static bool address(const char **s, Addr *a)
{
    const char *p = skip_ws(*s);
    unsigned open = 0;
    while (*p == '[') {
        open++;
        p = skip_ws(p + 1);
    }
    if (open > PTR_MAX) {
        return false;
    }
    int64_t v;
    if (!number(&p, &v) || v < 0) {
        return false;
    }
    memset(a, 0, sizeof(*a));
    a->base = (uint32_t)v;
    a->steps = (uint8_t)open;
    for (unsigned i = 0; i < open; i++) {
        p = skip_ws(p);
        if (*p != ']') {
            return false;
        }
        p = skip_ws(p + 1);
        if (*p == '+' || *p == '-') {
            const bool neg = *p == '-';
            p++;
            if (!number(&p, &v) || v < 0) {
                return false;
            }
            a->off[i] = (int32_t)(neg ? -v : v);
        }
    }
    *s = p;
    return true;
}

/* An integer value that fits width bytes (negative: two's complement). */
static bool int_value(const char **s, unsigned width, uint32_t *out)
{
    int64_t v;
    if (!number(s, &v)) {
        return false;
    }
    const int64_t max = width == 4 ? 0xFFFFFFFFll : (1ll << (8 * width)) - 1;
    if (v > max || v < -(max / 2 + 1)) {
        return false;
    }
    *out = (uint32_t)v & (uint32_t)max;
    return true;
}

static bool float_value(const char **s, uint32_t *out)
{
    const char *p = skip_ws(*s);
    char *end;
    const float f = strtof(p, &end);
    if (end == p) {
        return false;
    }
    memcpy(out, &f, 4);
    *s = end;
    return true;
}

static bool comparison(const char **s, uint8_t *cmp)
{
    static const struct { const char *t; uint8_t c; } k[] = {
        {"==", CMP_EQ}, {"!=", CMP_NE}, {"<=", CMP_LE}, {">=", CMP_GE},
        {"<", CMP_LT}, {">", CMP_GT}, {"&", CMP_AND},
    };
    const char *p = skip_ws(*s);
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        const size_t n = strlen(k[i].t);
        if (!strncmp(p, k[i].t, n)) {
            *cmp = k[i].c;
            *s = p + n;
            return true;
        }
    }
    return false;
}

static bool fail(char *err, size_t errlen, const char *why)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s", why);
    }
    return false;
}

static bool line_into(Cheat *c, const char *line, char *err, size_t errlen);

bool nc_cheat_line(const char *line, char *err, size_t errlen)
{
    if (!g_n) {
        return fail(err, errlen, "no cheat");
    }
    return line_into(&g_cheats[g_n - 1], line, err, errlen);
}

int nc_cheat_compile(const char *name, bool on, bool once, const char *text, bool dry, char *err,
                     size_t errlen)
{
    const unsigned n = g_n, nops = g_nops;
    if (n >= NC_CHEATS_MAX) {
        fail(err, errlen, "too many cheats");
        return -1;
    }
    Cheat *c = &g_cheats[n];             /* not counted yet: the guest's thread doesn't look */
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->on = on;
    c->gen = on;
    c->once = once;
    c->first = nops;
    unsigned lineno = 0, ops = 0;
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        size_t keep = len;
        while (keep && p[keep - 1] == '\r') {
            keep--;                             /* a file from Windows */
        }
        char line[256], why[96];
        snprintf(line, sizeof(line), "%.*s", (int)(keep < sizeof(line) ? keep : sizeof(line) - 1), p);
        lineno++;
        const unsigned before = c->n;
        if (!line_into(c, line, why, sizeof(why))) {
            g_nops = nops;
            if (err && errlen) {
                snprintf(err, errlen, "line %u: %s", lineno, why);
            }
            return -1;
        }
        ops += c->n - before;
        p = eol ? eol + 1 : p + len;
    }
    if (c->depth) {
        g_nops = nops;
        fail(err, errlen, "an if without its end");
        return -1;
    }
    if (!ops) {
        g_nops = nops;
        fail(err, errlen, "no code lines");
        return -1;
    }
    if (dry) {
        g_nops = nops;
        return 0;
    }
    __atomic_store_n(&g_n, n + 1, __ATOMIC_RELEASE);
    return (int)n;
}

/* One code at *s, into op (depth: the ifs open before it); *s past it, the
 * rest of its line left. NULL, or why it isn't one. */
static const char *parse_code(const char **s, Op *op, int depth)
{
    const char *p = skip_ws(*s);
    char word[8];
    size_t n = 0;
    while (isalnum((unsigned char)*p) && n < sizeof(word) - 1) {
        word[n++] = (char)tolower((unsigned char)*p++);
    }
    word[n] = 0;
    memset(op, 0, sizeof(*op));
    if (!strcmp(word, "else") || !strcmp(word, "end")) {
        if (!depth) {
            return "no if to close";
        }
        op->kind = word[1] == 'l' ? OP_ELSE : OP_END;
    } else {
        const bool is_if = !strncmp(word, "if", 2);
        const char *t = word + (is_if ? 2 : 1);
        if (!is_if && word[0] != 'w') {
            return "unknown code";
        }
        op->kind = is_if ? OP_IF : OP_W;
        if (!strcmp(t, "8") || !strcmp(t, "16") || !strcmp(t, "32")) {
            op->width = (uint8_t)(atoi(t) / 8);
        } else if (!strcmp(t, "f")) {
            op->width = 4;
            op->isf = 1;
        } else {
            return "unknown code";
        }
        if (!address(&p, &op->a)) {
            return "bad address";
        }
        if (is_if && !comparison(&p, &op->cmp)) {
            return "bad comparison";
        }
        if (is_if && op->isf && op->cmp == CMP_AND) {
            return "& compares integers";
        }
        if (!(op->isf ? float_value(&p, &op->v) : int_value(&p, op->width, &op->v))) {
            return "bad value";
        }
        if (is_if && depth >= DEPTH_MAX) {
            return "ifs nested too deep";
        }
    }
    *s = p;
    return NULL;
}

static int depth_change(const Op *op)
{
    return op->kind == OP_IF ? 1 : op->kind == OP_END ? -1 : 0;
}

static bool line_into(Cheat *c, const char *line, char *err, size_t errlen)
{
    const char *p = skip_ws(line);
    if (!*p || *p == '#' || isspace((unsigned char)*p)) {
        return true;                            /* blank or a comment */
    }
    if (g_nops >= OPS_MAX) {
        return fail(err, errlen, "too many code lines");
    }
    Op op;
    const char *why = parse_code(&p, &op, c->depth);
    p = skip_ws(p);
    if (!why && *p && *p != '#') {
        why = "extra text";
    }
    if (why) {
        return fail(err, errlen, why);
    }
    c->depth += depth_change(&op);
    g_ops[g_nops++] = op;
    c->n++;
    return true;
}

/* ---- the frontend's cheats ---- */

/* Between a frontend's codes: blanks, newlines, + */
static bool is_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '+';
}

/* Exactly 8 hex digits. */
static bool hex8(const char **s, uint32_t *v)
{
    const char *p = *s;
    for (unsigned i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)p[i])) {
            return false;
        }
    }
    if (isalnum((unsigned char)p[8])) {
        return false;
    }
    *v = (uint32_t)strtoul(p, NULL, 16);
    *s = p + 8;
    return true;
}

/* AAAAAAAA VVVVVVVV, cheat files' plain form: V's 32 bits written at A. */
static bool raw_code(const char **s, Op *op)
{
    const char *p = *s;
    uint32_t a, v;
    if (!hex8(&p, &a) || (*p != ' ' && *p != '\t')) {
        return false;
    }
    p = skip_ws(p);
    if (!hex8(&p, &v)) {
        return false;
    }
    memset(op, 0, sizeof(*op));
    op->kind = OP_W;
    op->width = 4;
    op->a.base = a;
    op->v = v;
    *s = p;
    return true;
}

/* A frontend's code text into ops of its own (malloc'd, *nops of them). */
static bool front_compile(const char *text, Op **ops, unsigned *nops, char *err, size_t errlen)
{
    Op *o = NULL;
    unsigned n = 0, cap = 0, k = 0;
    int depth = 0;
    const char *why = NULL;
    for (const char *p = text; !why;) {
        while (is_sep(*p) || *p == '#') {
            p = *p == '#' ? p + strcspn(p, "\n") : p + 1;   /* a comment: to the line's end */
        }
        if (!*p) {
            break;
        }
        k++;
        Op op;
        why = raw_code(&p, &op) ? NULL : parse_code(&p, &op, depth);
        if (!why && *p && !is_sep(*p) && *p != '#') {
            why = "extra text";
        }
        if (!why && n == cap) {
            Op *more = n < OPS_MAX ? realloc(o, (cap ? 2 * cap : 8) * sizeof(*o)) : NULL;
            if (more) {
                o = more;
                cap = cap ? 2 * cap : 8;
            } else {
                why = n < OPS_MAX ? "out of memory" : "too many codes";
            }
        }
        if (!why) {
            depth += depth_change(&op);
            o[n++] = op;
        }
    }
    if (why) {
        free(o);
        if (err && errlen) {
            snprintf(err, errlen, "code %u: %s", k, why);
        }
        return false;
    }
    if (depth || !n) {
        free(o);
        return fail(err, errlen, depth ? "an if without its end" : "no codes");
    }
    *ops = o;
    *nops = n;
    return true;
}

bool nc_cheat_front_set(unsigned index, bool on, const char *text, char *err, size_t errlen)
{
    if (index >= NC_CHEATS_MAX) {
        return fail(err, errlen, "too many cheats");
    }
    Op *ops = NULL;
    unsigned n = 0;
    const bool ok = !on || front_compile(text ? text : "", &ops, &n, err, errlen);
    pthread_mutex_lock(&g_front_mu);
    Front *f = &g_front[index];
    Op *old = f->ops;
    f->ops = ops;
    f->n = n;
    f->misses = 0;
    f->told = false;
    if (ops && index >= g_nfront) {
        g_nfront = index + 1;
    }
    while (g_nfront && !g_front[g_nfront - 1].ops) {
        g_nfront--;
    }
    pthread_mutex_unlock(&g_front_mu);
    free(old);
    return ok;
}

void nc_cheat_front_reset(void)
{
    pthread_mutex_lock(&g_front_mu);
    for (unsigned i = 0; i < g_nfront; i++) {
        free(g_front[i].ops);
    }
    memset(g_front, 0, sizeof(g_front));
    g_nfront = 0;
    pthread_mutex_unlock(&g_front_mu);
}

int nc_cheat_front_missed(void)
{
    int found = -1;
    if (pthread_mutex_trylock(&g_front_mu)) {
        return -1;                              /* being changed: the next vblank */
    }
    for (unsigned i = 0; i < g_nfront && found < 0; i++) {
        if (g_front[i].misses && !g_front[i].told) {
            g_front[i].told = true;
            found = (int)i;
        }
    }
    pthread_mutex_unlock(&g_front_mu);
    return found;
}

void nc_cheat_set(int index, bool on)
{
    if (index < 0 || (unsigned)index >= nc_cheat_count()) {
        return;
    }
    Cheat *c = &g_cheats[index];
    if (on && !__atomic_load_n(&c->on, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&c->gen, 1, __ATOMIC_ACQ_REL);   /* a once cheat writes again */
    }
    __atomic_store_n(&c->on, on, __ATOMIC_RELEASE);
}

bool nc_cheat_on(int index)
{
    return index >= 0 && (unsigned)index < g_n && __atomic_load_n(&g_cheats[index].on, __ATOMIC_ACQUIRE);
}

unsigned nc_cheat_count(void)
{
    return __atomic_load_n(&g_n, __ATOMIC_ACQUIRE);
}

const char *nc_cheat_name(int index)
{
    return index >= 0 && (unsigned)index < g_n ? g_cheats[index].name : "";
}

uint64_t nc_cheat_misses(int index)
{
    return index >= 0 && (unsigned)index < g_n ? g_cheats[index].misses : 0;
}

static bool resolve(const NcCheatMem *m, const Addr *a, uint32_t *va)
{
    uint32_t x = a->base;
    for (unsigned i = 0; i < a->steps; i++) {
        uint8_t b[4];
        if (!m->read(m->opaque, x, b, 4)) {
            return false;
        }
        const uint32_t ptr = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        if (!ptr) {
            return false;                       /* a NULL pointer: nothing there yet */
        }
        x = ptr + (uint32_t)a->off[i];
    }
    *va = x;
    return true;
}

static uint32_t load(const uint8_t *b, unsigned width)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++) {
        v |= (uint32_t)b[i] << (8 * i);
    }
    return v;
}

static bool compare(const Op *op, uint32_t cur)
{
    if (op->isf) {
        float a, b;
        memcpy(&a, &cur, 4);
        memcpy(&b, &op->v, 4);
        switch (op->cmp) {
        case CMP_EQ: return a == b;
        case CMP_NE: return a != b;
        case CMP_LT: return a < b;
        case CMP_GT: return a > b;
        case CMP_LE: return a <= b;
        default: return a >= b;
        }
    }
    switch (op->cmp) {
    case CMP_EQ: return cur == op->v;
    case CMP_NE: return cur != op->v;
    case CMP_LT: return cur < op->v;
    case CMP_GT: return cur > op->v;
    case CMP_LE: return cur <= op->v;
    case CMP_GE: return cur >= op->v;
    default: return (cur & op->v) == op->v;
    }
}

static unsigned run_ops(const Op *ops, unsigned n, uint64_t *misses, const NcCheatMem *m)
{
    struct { bool outer, taken; } st[DEPTH_MAX];
    unsigned sp = 0, wrote = 0;
    bool active = true;
    for (unsigned i = 0; i < n; i++) {
        const Op *op = &ops[i];
        switch (op->kind) {
        case OP_IF: {
            bool taken = false;
            uint32_t va;
            uint8_t b[4];
            if (active) {
                if (resolve(m, &op->a, &va) && m->read(m->opaque, va, b, op->width)) {
                    taken = compare(op, load(b, op->width));
                } else {
                    (*misses)++;
                }
            }
            st[sp].outer = active;
            st[sp].taken = taken;
            sp++;
            active = active && taken;
            break;
        }
        case OP_ELSE:
            if (sp) {
                active = st[sp - 1].outer && !st[sp - 1].taken;
            }
            break;
        case OP_END:
            if (sp) {
                active = st[--sp].outer;
            }
            break;
        default: {
            if (!active) {
                break;
            }
            uint32_t va;
            uint8_t cur[4], b[4];
            for (unsigned k = 0; k < op->width; k++) {
                b[k] = (uint8_t)(op->v >> (8 * k));
            }
            if (!resolve(m, &op->a, &va) || !m->read(m->opaque, va, cur, op->width)) {
                (*misses)++;
                break;
            }
            if (memcmp(cur, b, op->width)) {
                if (m->write(m->opaque, va, b, op->width)) {
                    wrote += op->width;
                } else {
                    (*misses)++;
                }
            }
            break;
        }
        }
    }
    return wrote;
}

unsigned nc_cheats_run(const NcCheatMem *m)
{
    unsigned wrote = 0;
    const unsigned n = nc_cheat_count();     /* a cheat the app adds now is whole when counted */
    for (unsigned i = 0; i < n; i++) {
        Cheat *c = &g_cheats[i];
        if (!__atomic_load_n(&c->on, __ATOMIC_ACQUIRE)) {
            continue;
        }
        const unsigned gen = __atomic_load_n(&c->gen, __ATOMIC_ACQUIRE);
        if (c->once && c->done == gen) {
            continue;
        }
        const uint64_t misses = c->misses;
        wrote += run_ops(&g_ops[c->first], c->n, &c->misses, m);
        if (c->misses == misses) {
            c->done = gen;              /* a once cheat retries while its memory isn't there */
        }
    }
    if (!pthread_mutex_trylock(&g_front_mu)) {     /* taken: being changed, the next vblank */
        for (unsigned i = 0; i < g_nfront; i++) {
            Front *f = &g_front[i];
            if (f->ops) {
                wrote += run_ops(f->ops, f->n, &f->misses, m);
            }
        }
        pthread_mutex_unlock(&g_front_mu);
    }
    return wrote;
}
