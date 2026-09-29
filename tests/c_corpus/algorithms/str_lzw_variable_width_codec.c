/*
 * title: LZW codec with variable-width codes
 * topic: algorithms
 * covers: LZW, hash dictionary, variable-width bit packing, KwKwK case, dictionary saturation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* LZW with a dictionary stored as a hash table over (prefix code, byte) pairs,
 * variable-width codes packed LSB first, and a decoder handling the KwKwK case. */
enum { MAXCODES = 4096, HSIZE = 8209 };

typedef struct {
    int key; /* prefix << 8 | byte, or -1 if empty */
    int code;
} Slot;

typedef struct {
    unsigned char *b;
    size_t nbits, cap;
} Bits;

static void put_bits(Bits *w, unsigned v, int width) {
    for (int i = 0; i < width; i++) {
        if (w->nbits / 8 >= w->cap) {
            w->cap = w->cap ? w->cap * 2 : 64;
            w->b = realloc(w->b, w->cap);
        }
        if (w->nbits % 8 == 0)
            w->b[w->nbits / 8] = 0;
        w->b[w->nbits / 8] |= (unsigned char)(((v >> i) & 1u) << (w->nbits % 8));
        w->nbits++;
    }
}

static int width_for(int next_code) {
    int w = 9;
    while ((1 << w) < next_code + 1 && w < 12)
        w++;
    return w;
}

static void lzw_encode(const unsigned char *s, int n, Bits *w, int *ncodes) {
    Slot *tab = malloc(sizeof(Slot) * HSIZE);
    for (int i = 0; i < HSIZE; i++)
        tab[i].key = -1;
    int next = 256, cur = -1;
    *ncodes = 0;
    for (int i = 0; i < n; i++) {
        if (cur < 0) {
            cur = s[i];
            continue;
        }
        int key = cur << 8 | s[i];
        unsigned h = (unsigned)key * 2654435761u % HSIZE;
        while (tab[h].key != -1 && tab[h].key != key)
            h = (h + 1) % HSIZE;
        if (tab[h].key == key) {
            cur = tab[h].code;
        } else {
            put_bits(w, (unsigned)cur, width_for(next));
            (*ncodes)++;
            if (next < MAXCODES) {
                tab[h].key = key;
                tab[h].code = next++;
            }
            cur = s[i];
        }
    }
    if (cur >= 0) {
        put_bits(w, (unsigned)cur, width_for(next));
        (*ncodes)++;
    }
    free(tab);
}

static unsigned get_bits(const Bits *r, size_t *pos, int width) {
    unsigned v = 0;
    for (int i = 0; i < width; i++, (*pos)++)
        v |= (unsigned)((r->b[*pos / 8] >> (*pos % 8)) & 1) << i;
    return v;
}

static int lzw_decode(const Bits *r, int ncodes, unsigned char *out, int cap) {
    static int prefix[MAXCODES];
    static unsigned char last[MAXCODES];
    static unsigned char stack[MAXCODES + 1];
    int next = 256, n = 0, prev = -1;
    unsigned char first = 0;
    size_t pos = 0;
    for (int c = 0; c < ncodes; c++) {
        /* the decoder is one entry behind the encoder */
        int w = width_for(prev < 0 ? next : (next < MAXCODES ? next + 1 : next));
        unsigned code = get_bits(r, &pos, w);
        int sp = 0;
        int cur = (int)code;
        if (prev >= 0 && cur >= next) {
            if (cur != next)
                return -1;
            stack[sp++] = first; /* KwKwK */
            cur = prev;
        }
        while (cur >= 256) {
            stack[sp++] = last[cur];
            cur = prefix[cur];
        }
        stack[sp++] = (unsigned char)cur;
        first = (unsigned char)cur;
        if (n + sp > cap)
            return -1;
        while (sp)
            out[n++] = stack[--sp];
        if (prev >= 0 && next < MAXCODES) {
            prefix[next] = prev;
            last[next] = first;
            next++;
        }
        prev = (int)code;
    }
    return n;
}

int main(void) {
    const char *demo = "TOBEORNOTTOBEORTOBEORNOT";
    Bits w = {NULL, 0, 0};
    int nc;
    static unsigned char dec[8192];
    lzw_encode((const unsigned char *)demo, (int)strlen(demo), &w, &nc);
    int dn = lzw_decode(&w, nc, dec, sizeof dec);
    check(dn == (int)strlen(demo) && memcmp(dec, demo, (size_t)dn) == 0, "demo roundtrip");
    printf("%s: %d codes, %zu bits (raw %zu bits)\n", demo, nc, w.nbits, strlen(demo) * 8);
    free(w.b);

    /* KwKwK case: aaaaaaa */
    Bits w2 = {NULL, 0, 0};
    lzw_encode((const unsigned char *)"aaaaaaaaaaaaaaaa", 16, &w2, &nc);
    dn = lzw_decode(&w2, nc, dec, sizeof dec);
    check(dn == 16 && memcmp(dec, "aaaaaaaaaaaaaaaa", 16) == 0, "kwkwk");
    printf("a^16: %d codes\n", nc);
    free(w2.b);

    static unsigned char src[7000];
    const char *names[] = {"2-letter random", "4-letter random", "byte noise", "period 7"};
    int alph[] = {2, 4, 256, 0};
    for (int m = 0; m < 4; m++) {
        int n = 6000;
        for (int i = 0; i < n; i++)
            src[i] = m == 3 ? (unsigned char)("lzwlzw!"[i % 7]) : (unsigned char)(rnd() % (unsigned)alph[m]);
        Bits ww = {NULL, 0, 0};
        lzw_encode(src, n, &ww, &nc);
        int d = lzw_decode(&ww, nc, dec, sizeof dec);
        check(d == n && memcmp(dec, src, (size_t)n) == 0, "roundtrip");
        printf("%-16s n=%d codes=%4d bytes=%5zu\n", names[m], n, nc, (ww.nbits + 7) / 8);
        free(ww.b);
    }
    return 0;
}
