/*
 * title: Double-array trie with dynamic relocation
 * topic: data_structures
 * covers: double-array trie, base/check arrays, child relocation, common prefix search, erase, brute force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define N 40000
#define TERM 27
static int base_[N];
static int check_[N];
static int high_water = 1;

static int code_of(char c) { return c - 'a' + 1; }

static int is_free(int t) { return t > 1 && t < N && check_[t] == 0; }

static void da_init(void) {
    memset(base_, 0, sizeof base_);
    memset(check_, 0, sizeof check_);
    base_[1] = 1;
    check_[1] = -1; /* root is occupied */
}

static int children(int s, int *codes) {
    int n = 0;
    if (base_[s] <= 0)
        return 0;
    for (int c = 1; c <= TERM; c++) {
        int t = base_[s] + c;
        if (t < N && check_[t] == s)
            codes[n++] = c;
    }
    return n;
}

/* move all children of s so that they start at a base that also fits `extra` */
static void relocate(int s, int extra) {
    int codes[TERM], n = children(s, codes);
    codes[n++] = extra;
    int b = 1;
    for (;; b++) {
        check(b + TERM < N, "double array capacity");
        int ok = 1;
        for (int i = 0; i < n && ok; i++)
            ok = is_free(b + codes[i]);
        if (ok)
            break;
    }
    int old = base_[s];
    for (int i = 0; i < n - 1; i++) {
        int from = old + codes[i], to = b + codes[i];
        check_[to] = s;
        base_[to] = base_[from];
        if (base_[from] > 0)
            for (int c = 1; c <= TERM; c++) {
                int u = base_[from] + c;
                if (u < N && check_[u] == from)
                    check_[u] = to;
            }
        base_[from] = 0;
        check_[from] = 0;
    }
    base_[s] = b;
}

static int new_child(int s, int c) {
    if (base_[s] <= 0) {
        int b = 1;
        while (!is_free(b + c))
            b++;
        base_[s] = b;
    } else if (!is_free(base_[s] + c))
        relocate(s, c);
    int t = base_[s] + c;
    check(t < N, "capacity");
    check_[t] = s;
    if (t > high_water)
        high_water = t;
    return t;
}

static int da_step(int s, int c) {
    if (s <= 0 || base_[s] <= 0)
        return 0;
    int t = base_[s] + c;
    return (t < N && check_[t] == s) ? t : 0;
}

static void da_insert(const char *w, int id) {
    int s = 1;
    for (; *w; w++) {
        int t = da_step(s, code_of(*w));
        s = t ? t : new_child(s, code_of(*w));
    }
    int t = da_step(s, TERM);
    if (!t)
        t = new_child(s, TERM);
    base_[t] = -(id + 1);
}

static int da_lookup(const char *w) {
    int s = 1;
    for (; *w; w++) {
        s = da_step(s, code_of(*w));
        if (!s)
            return -1;
    }
    int t = da_step(s, TERM);
    return t ? -base_[t] - 1 : -1;
}

static int da_erase(const char *w) {
    int s = 1;
    for (; *w; w++) {
        s = da_step(s, code_of(*w));
        if (!s)
            return 0;
    }
    int t = da_step(s, TERM);
    if (!t)
        return 0;
    base_[t] = 0;
    check_[t] = 0;
    return 1;
}

/* ids of all stored words that are prefixes of text[from..] */
static int da_prefixes(const char *text, int *ids) {
    int s = 1, n = 0;
    for (;; text++) {
        int t = da_step(s, TERM);
        if (t)
            ids[n++] = -base_[t] - 1;
        if (!*text)
            break;
        s = da_step(s, code_of(*text));
        if (!s)
            break;
    }
    return n;
}

static int count_below(int s) {
    int total = 0;
    for (int c = 1; c <= TERM; c++) {
        int t = da_step(s, c);
        if (!t)
            continue;
        total += c == TERM ? 1 : count_below(t);
    }
    return total;
}

static int da_count_prefix(const char *p) {
    int s = 1;
    for (; *p; p++) {
        s = da_step(s, code_of(*p));
        if (!s)
            return 0;
    }
    return count_below(s);
}

static char rand_letter(void) {
    unsigned a = rnd(6);
    unsigned b = rnd(3);
    return (char)('a' + a * (1 + b) % 26);
}

#define MAXW 1000
static char words[MAXW][10];
static int alive[MAXW];
static int nw;

int main(void) {
    da_init();
    int inserted = 0, erased = 0;
    for (int step = 0; step < 3000; step++) {
        char w[10];
        int len = 1 + (int)rnd(6);
        for (int i = 0; i < len; i++)
            w[i] = rand_letter();
        w[len] = 0;
        int idx = -1;
        for (int i = 0; i < nw; i++)
            if (alive[i] && strcmp(words[i], w) == 0)
                idx = i;
        unsigned op = rnd(10);
        if (op < 5) {
            if (idx < 0 && nw < MAXW) {
                strcpy(words[nw], w);
                alive[nw] = 1;
                da_insert(w, nw++);
                inserted++;
            }
        } else if (op < 7) {
            int got = da_erase(w);
            check(got == (idx >= 0), "erase result");
            if (got) {
                alive[idx] = 0;
                erased++;
            }
        } else {
            check(da_lookup(w) == idx, "lookup");
        }
    }
    int live = 0;
    for (int i = 0; i < nw; i++) {
        live += alive[i];
        if (alive[i])
            check(da_lookup(words[i]) == i, "final lookup");
    }
    printf("inserted=%d erased=%d live=%d high_water=%d\n", inserted, erased, live, high_water);
    /* common prefix search over generated texts */
    long psum = 0;
    for (int q = 0; q < 200; q++) {
        char text[12];
        int len = 3 + (int)rnd(7);
        for (int i = 0; i < len; i++)
            text[i] = rand_letter();
        text[len] = 0;
        int ids[16];
        int n = da_prefixes(text, ids);
        int want = 0;
        for (int i = 0; i < nw; i++)
            if (alive[i] && strncmp(text, words[i], strlen(words[i])) == 0)
                want++;
        check(n == want, "prefix search count");
        for (int i = 0; i < n; i++)
            check(alive[ids[i]] && strncmp(text, words[ids[i]], strlen(words[ids[i]])) == 0,
                  "prefix search ids");
        psum += n;
    }
    printf("common prefix matches=%ld\n", psum);
    const char *pre[] = {"a", "b", "ab", "ce", "gg"};
    for (int p = 0; p < 5; p++) {
        int want = 0;
        for (int i = 0; i < nw; i++)
            want += alive[i] && strncmp(words[i], pre[p], strlen(pre[p])) == 0;
        check(da_count_prefix(pre[p]) == want, "count prefix");
        printf("prefix %s -> %d\n", pre[p], want);
    }
    return 0;
}
