/*
 * title: Allocation ledger with tags and leak report
 * topic: memory
 * covers: tracked allocator wrapper, per-tag statistics, peak usage, leak listing by allocation id, double free detection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ALLOCS 64

typedef struct {
    void *p;
    size_t size;
    int tag;
    unsigned id;
    int live;
} Entry;

enum { TAG_PARSER, TAG_CACHE, TAG_TEMP, NTAGS };
static const char *tag_names[NTAGS] = {"parser", "cache", "temp"};

static Entry ledger[MAX_ALLOCS];
static int nentries;
static unsigned next_id = 1;
static size_t tag_live[NTAGS], tag_peak[NTAGS], tag_total[NTAGS];
static size_t cur_bytes, peak_bytes;
static int bad_frees;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *tracked_alloc(size_t n, int tag) {
    check(nentries < MAX_ALLOCS, "ledger full");
    void *p = malloc(n);
    check(p != NULL, "alloc");
    Entry *e = &ledger[nentries++];
    e->p = p;
    e->size = n;
    e->tag = tag;
    e->id = next_id++;
    e->live = 1;
    tag_live[tag] += n;
    tag_total[tag] += n;
    if (tag_live[tag] > tag_peak[tag])
        tag_peak[tag] = tag_live[tag];
    cur_bytes += n;
    if (cur_bytes > peak_bytes)
        peak_bytes = cur_bytes;
    return p;
}

static Entry *find(void *p) {
    for (int i = nentries - 1; i >= 0; i--)
        if (ledger[i].p == p)
            return &ledger[i];
    return NULL;
}

static void tracked_free(void *p) {
    Entry *e = find(p);
    if (!e || !e->live) {
        bad_frees++; /* unknown pointer or double free: report, do not touch the heap */
        return;
    }
    e->live = 0;
    tag_live[e->tag] -= e->size;
    cur_bytes -= e->size;
    free(p);
}

static void report(const char *when) {
    printf("%s: cur=%zu peak=%zu |", when, cur_bytes, peak_bytes);
    for (int t = 0; t < NTAGS; t++)
        printf(" %s live=%zu peak=%zu total=%zu%s", tag_names[t], tag_live[t], tag_peak[t],
               tag_total[t], t + 1 < NTAGS ? "," : "");
    printf("\n");
}

int main(void) {
    char *tokens[5];
    for (int i = 0; i < 5; i++) {
        tokens[i] = tracked_alloc(16u + (unsigned)i * 8u, TAG_PARSER);
        memset(tokens[i], 'a' + i, 16u + (unsigned)i * 8u);
    }
    char *cache = tracked_alloc(256, TAG_CACHE);
    memset(cache, 0, 256);
    report("after build");

    char *tmp1 = tracked_alloc(100, TAG_TEMP);
    char *tmp2 = tracked_alloc(60, TAG_TEMP);
    memset(tmp1, 1, 100);
    memset(tmp2, 2, 60);
    report("with temps");
    tracked_free(tmp1);
    tracked_free(tmp2);
    tracked_free(tmp1); /* double free is caught */
    int local = 0;
    tracked_free(&local); /* foreign pointer is caught */
    report("temps freed");
    printf("bad frees: %d\n", bad_frees);

    /* parser finishes: free tokens 0, 2, 4 but "forget" 1 and 3 */
    tracked_free(tokens[0]);
    tracked_free(tokens[2]);
    tracked_free(tokens[4]);
    report("partial release");

    printf("leak report:\n");
    int leaks = 0;
    size_t leaked_bytes = 0;
    for (int i = 0; i < nentries; i++)
        if (ledger[i].live) {
            printf("  id=%u tag=%s size=%zu\n", ledger[i].id, tag_names[ledger[i].tag],
                   ledger[i].size);
            leaks++;
            leaked_bytes += ledger[i].size;
        }
    printf("leaks: %d blocks, %zu bytes\n", leaks, leaked_bytes);
    check(leaks == 3 && leaked_bytes == 24 + 40 + 256, "expected leaks");
    check(cur_bytes == leaked_bytes, "cur bytes matches");

    /* the test harness cleans up what the "program" leaked */
    for (int i = 0; i < nentries; i++)
        if (ledger[i].live)
            tracked_free(ledger[i].p);
    check(cur_bytes == 0 && bad_frees == 2, "clean end");
    report("cleanup");
    return 0;
}
