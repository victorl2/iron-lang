/*
 * title: Word frequency counting with string-keyed table and top-k
 * topic: data_structures
 * covers: string-keyed hash map, counting, tokenizing, arena-free key ownership, top-k with total order, sort-based reference
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
typedef struct Ent {
    char *word;
    uint32_t hash;
    long count;
    struct Ent *next;
} Ent;
typedef struct {
    Ent **b;
    size_t nb, n;
    long lookups, cmps;
} Freq;

static uint32_t hash_word(const char *s) {
    uint32_t h = 5381u;
    for (; *s; s++)
        h = h * 33u ^ (unsigned char)*s;
    return mix32(h);
}

static void freq_grow(Freq *f) {
    size_t nn = f->nb * 2;
    Ent **nb = calloc(nn, sizeof(Ent *));
    for (size_t i = 0; i < f->nb; i++)
        for (Ent *e = f->b[i]; e;) {
            Ent *nx = e->next;
            e->next = nb[e->hash % nn];
            nb[e->hash % nn] = e;
            e = nx;
        }
    free(f->b);
    f->b = nb;
    f->nb = nn;
}

static void freq_add(Freq *f, const char *w) {
    uint32_t h = hash_word(w);
    f->lookups++;
    for (Ent *e = f->b[h % f->nb]; e; e = e->next) {
        f->cmps++;
        if (e->hash == h && strcmp(e->word, w) == 0) {
            e->count++;
            return;
        }
    }
    if (f->n >= f->nb * 2)
        freq_grow(f);
    Ent *e = malloc(sizeof *e);
    size_t l = strlen(w) + 1;
    e->word = malloc(l);
    memcpy(e->word, w, l);
    e->hash = h;
    e->count = 1;
    e->next = f->b[h % f->nb];
    f->b[h % f->nb] = e;
    f->n++;
}

typedef struct {
    const char *w;
    long c;
} WC;
static int cmp_wc(const void *a, const void *b) { /* count desc, word asc: total order */
    const WC *x = a, *y = b;
    if (x->c != y->c)
        return x->c > y->c ? -1 : 1;
    return strcmp(x->w, y->w);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void) {
    static const char *syl[] = {"ka", "to", "mi", "ren", "su", "lo", "vi", "an", "th", "ex"};
    enum { NW = 900, TOKENS = 30000 };
    static char vocab[NW][12];
    for (int i = 0; i < NW; i++) {
        int parts = 1 + (int)(rnd() % 3);
        vocab[i][0] = 0;
        for (int p = 0; p < parts; p++)
            strcat(vocab[i], syl[rnd() % 10]);
    }
    /* build text with skewed word choice, then tokenize it */
    static char text[TOKENS * 8];
    size_t len = 0;
    for (int i = 0; i < TOKENS; i++) {
        uint64_t a = rnd() % NW, b = rnd() % NW;
        const char *w = vocab[a * b / NW];
        len += (size_t)snprintf(text + len, sizeof text - len, "%s%s", w, rnd() % 9 == 0 ? ", " : " ");
    }
    Freq f = {calloc(16, sizeof(Ent *)), 16, 0, 0, 0};
    static char *tokens[TOKENS];
    int nt = 0;
    for (char *p = text; *p;) {
        while (*p == ' ' || *p == ',')
            *p++ = 0;
        if (!*p)
            break;
        char *s = p;
        while (*p && *p != ' ' && *p != ',')
            p++;
        char save = *p;
        *p = 0;
        freq_add(&f, s);
        check(nt < TOKENS, "token bound");
        tokens[nt++] = s;
        if (!save)
            break;
        p++;
    }
    check(nt == TOKENS, "token count");
    /* reference: sort all tokens and run-length encode */
    static char *sorted[TOKENS];
    memcpy(sorted, tokens, sizeof sorted);
    qsort(sorted, TOKENS, sizeof(char *), cmp_str);
    static WC ref[NW];
    int nref = 0;
    for (int i = 0; i < TOKENS;) {
        int j = i;
        while (j < TOKENS && strcmp(sorted[j], sorted[i]) == 0)
            j++;
        ref[nref].w = sorted[i];
        ref[nref].c = j - i;
        nref++;
        i = j;
    }
    check((size_t)nref == f.n, "distinct words");
    static WC got[NW];
    int ng = 0;
    for (size_t i = 0; i < f.nb; i++)
        for (Ent *e = f.b[i]; e; e = e->next) {
            got[ng].w = e->word;
            got[ng].c = e->count;
            ng++;
        }
    qsort(got, (size_t)ng, sizeof(WC), cmp_wc);
    qsort(ref, (size_t)nref, sizeof(WC), cmp_wc);
    long total = 0;
    for (int i = 0; i < ng; i++) {
        check(strcmp(got[i].w, ref[i].w) == 0 && got[i].c == ref[i].c, "table equals sorted reference");
        total += got[i].c;
    }
    check(total == TOKENS, "counts sum to token count");
    printf("tokens=%d distinct=%zu table buckets=%zu avg compares per lookup=%.4f\n", nt, f.n, f.nb, (double)f.cmps / (double)f.lookups);
    printf("top 10:");
    for (int i = 0; i < 10; i++)
        printf(" %s=%ld", got[i].w, got[i].c);
    long singles = 0;
    for (int i = 0; i < ng; i++)
        singles += got[i].c == 1;
    printf("\nsingletons=%ld\n", singles);
    for (size_t i = 0; i < f.nb; i++)
        for (Ent *e = f.b[i]; e;) {
            Ent *nx = e->next;
            free(e->word);
            free(e);
            e = nx;
        }
    free(f.b);
    return 0;
}
