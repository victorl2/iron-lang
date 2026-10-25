/*
 * title: VLSM subnet allocation and equal splitting
 * topic: networking
 * covers: variable length subnet masks, host count to prefix length, aligned allocation, equal split into 2^k subnets, overlap invariants, utilisation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { const char *name; uint32_t need; int len; uint32_t addr; } Req;

static uint32_t mask_of(int len) { return len == 0 ? 0u : (0xffffffffu << (32 - len)); }

static int prefix_for_hosts(uint32_t hosts) {
    /* smallest block with hosts + network + broadcast addresses; /31 and /32 not used here */
    uint32_t need = hosts + 2;
    int bits = 0;
    while (((uint64_t)1 << bits) < need) bits++;
    if (bits < 2) bits = 2;
    return 32 - bits;
}

static void ip(uint32_t a, char *b) {
    snprintf(b, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

static int by_need_desc(const void *a, const void *b) {
    const Req *x = a, *y = b;
    if (x->need != y->need) return x->need > y->need ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int by_addr(const void *a, const void *b) {
    const Req *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

int main(void) {
    uint32_t pool_base = 0x0a0a0000u; /* 10.10.0.0/16 shrunk to the first /20 for the demo */
    int pool_len = 20;
    Req reqs[] = {
        { "servers", 500, 0, 0 }, { "office-a", 120, 0, 0 }, { "office-b", 60, 0, 0 }, { "wifi", 1000, 0, 0 },
        { "dmz", 25, 0, 0 }, { "mgmt", 10, 0, 0 }, { "lab", 250, 0, 0 }, { "p2p-1", 2, 0, 0 }, { "p2p-2", 2, 0, 0 }, { "iot", 300, 0, 0 },
    };
    int n = (int)(sizeof reqs / sizeof reqs[0]);
    qsort(reqs, (size_t)n, sizeof reqs[0], by_need_desc);
    uint32_t cursor = pool_base;
    uint64_t used = 0;
    for (int i = 0; i < n; i++) {
        reqs[i].len = prefix_for_hosts(reqs[i].need);
        uint32_t size = 1u << (32 - reqs[i].len);
        cursor = (cursor + size - 1) & ~(size - 1); /* align */
        reqs[i].addr = cursor;
        cursor += size;
        used += size;
        CHECK(cursor - pool_base <= (1u << (32 - pool_len)));
    }
    qsort(reqs, (size_t)n, sizeof reqs[0], by_addr);
    for (int i = 0; i < n; i++) {
        char a[16], b[16];
        uint32_t last = reqs[i].addr | ~mask_of(reqs[i].len);
        ip(reqs[i].addr, a);
        ip(last, b);
        uint32_t cap = (1u << (32 - reqs[i].len)) - 2;
        printf("%-8s need %-5u -> %s/%d (%u usable) %s-%s\n", reqs[i].name, reqs[i].need, a, reqs[i].len, cap, a, b);
        CHECK(cap >= reqs[i].need);
        CHECK((reqs[i].addr & ~mask_of(reqs[i].len)) == 0);
        if (i > 0) CHECK(reqs[i].addr > (reqs[i - 1].addr | ~mask_of(reqs[i - 1].len)));
    }
    printf("pool /%d: %llu of %u addresses allocated (%.1f%%)\n", pool_len, (unsigned long long)used, 1u << (32 - pool_len),
           100.0 * (double)used / (double)(1u << (32 - pool_len)));

    /* Equal splits of 192.168.0.0/22 into 2^k subnets. */
    for (int k = 1; k <= 4; k++) {
        int len = 22 + k;
        uint32_t size = 1u << (32 - len);
        printf("split /22 into %d x /%d:", 1 << k, len);
        for (int i = 0; i < (1 << k); i++) {
            uint32_t a = 0xc0a80000u + (uint32_t)i * size;
            char b[16];
            ip(a, b);
            if (i < 3 || i == (1 << k) - 1) printf(" %s", b);
            else if (i == 3) printf(" ...");
        }
        printf("\n");
    }
    /* Prefix length table. */
    printf("hosts->prefix:");
    static const uint32_t hs[] = { 1, 2, 6, 7, 14, 30, 62, 126, 254, 255, 510, 1022, 65534 };
    for (size_t i = 0; i < sizeof hs / sizeof hs[0]; i++) printf(" %u:/%d", hs[i], prefix_for_hosts(hs[i]));
    printf("\n");
    return 0;
}
