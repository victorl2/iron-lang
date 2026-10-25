/*
 * title: Definedness bit tracking to catch reads of uninitialized memory
 * topic: memory
 * covers: valid-bit shadow, propagation through copies and arithmetic, uninitialized use reports, calloc versus malloc
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A tiny tracked memory: every byte has a parallel definedness flag (Memcheck-style). */
#define MEM 256
static unsigned char mem[MEM], def[MEM];
static int reports;
static char lastmsg[80];

typedef struct { int v; int d; } Val; /* value with a definedness bit */

static void t_store(int addr, Val v) { mem[addr] = (unsigned char)v.v; def[addr] = (unsigned char)v.d; }
static Val t_load(int addr) { Val v = {mem[addr], def[addr]}; return v; }
static void t_memset(int addr, int val, int n) { for (int i = 0; i < n; i++) { mem[addr + i] = (unsigned char)val; def[addr + i] = 1; } }
static void t_memcpy(int dst, int src, int n) { for (int i = 0; i < n; i++) { mem[dst + i] = mem[src + i]; def[dst + i] = def[src + i]; } }

/* arithmetic: result is defined only if both operands are */
static Val add(Val a, Val b) { Val r = {(a.v + b.v) & 0xFF, a.d && b.d}; return r; }
static Val mul(Val a, Val b) {
    Val r = {(a.v * b.v) & 0xFF, a.d && b.d};
    if ((a.d && a.v == 0) || (b.d && b.v == 0)) { r.v = 0; r.d = 1; } /* 0 * garbage is well defined */
    return r;
}
/* a branch on an undefined value is an error, like "conditional jump depends on uninitialised value" */
static int branch(Val c, const char *where) {
    if (!c.d) { reports++; snprintf(lastmsg, sizeof lastmsg, "uninitialised branch at %s", where); return 0; }
    return c.v != 0;
}
static void output(Val v, const char *where) {
    if (!v.d) { reports++; printf("  report: undefined value output at %s\n", where); return; }
    printf("  %s = %d\n", where, v.v);
}

typedef struct { int base; int size; } Buf;
static int next_free = 0;
static Buf t_malloc(int n) { Buf b = {next_free, n}; next_free += n; memset(&def[b.base], 0, (size_t)n); memset(&mem[b.base], 0xAB, (size_t)n); return b; }
static Buf t_calloc(int n) { Buf b = t_malloc(n); t_memset(b.base, 0, n); return b; }

/* count undefined bytes in a range */
static int undefined(int addr, int n) { int c = 0; for (int i = 0; i < n; i++) c += !def[addr + i]; return c; }

int main(void) {
    Buf a = t_malloc(8), z = t_calloc(8);
    printf("fresh malloc: %d/8 bytes undefined; calloc: %d/8\n", undefined(a.base, 8), undefined(z.base, 8));

    /* partially initialise a */
    for (int i = 0; i < 8; i += 2) { Val v = {i * 10, 1}; t_store(a.base + i, v); }
    printf("after init of even slots: undefined=%d\n", undefined(a.base, 8));

    printf("outputs:\n");
    output(t_load(a.base + 2), "a[2]");
    output(t_load(a.base + 3), "a[3]");
    output(add(t_load(a.base + 4), t_load(a.base + 5)), "a[4]+a[5]");
    output(add(t_load(a.base + 4), t_load(a.base + 6)), "a[4]+a[6]");
    output(mul(t_load(a.base + 0), t_load(a.base + 1)), "a[0]*a[1] (0 * garbage)");
    output(mul(t_load(a.base + 2), t_load(a.base + 1)), "a[2]*a[1]");

    /* undefinedness travels through memcpy */
    Buf c = t_malloc(8);
    t_memcpy(c.base, a.base, 8);
    printf("copy of a has %d undefined bytes (same positions: %d)\n", undefined(c.base, 8),
           memcmp(&def[a.base], &def[c.base], 8) == 0);

    /* branch on data: sum of calloc'd buffer is fine, sum of malloc'd is not */
    int acc_def = 1, acc = 0;
    for (int i = 0; i < 8; i++) { Val v = t_load(z.base + i); acc += v.v; acc_def &= v.d; }
    Val sz = {acc, acc_def};
    printf("branch on calloc sum: %d\n", branch(sz, "calloc sum"));
    Val u = t_load(a.base + 1);
    printf("branch on undefined byte: %d (message: %s)\n", branch(u, "a[1]"), lastmsg);
    /* memset repairs it */
    t_memset(a.base, 7, 8);
    Val ok = t_load(a.base + 1);
    printf("after memset: branch=%d undefined=%d\n", branch(ok, "a[1] again"), undefined(a.base, 8));
    printf("total reports: %d\n", reports);
    return reports == 4 ? 0 : 1;
}
