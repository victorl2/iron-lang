/*
 * title: Copy-on-write array with cheap snapshots
 * topic: memory
 * covers: chunked COW array, snapshot by refcount bump, copy only touched chunks, structural sharing statistics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 8
#define NCHUNKS 8
#define TOTAL (CHUNK * NCHUNKS)

typedef struct {
    int rc;
    int data[CHUNK];
} Chunk;

typedef struct {
    Chunk *chunks[NCHUNKS];
} CowArray;

static int live_chunks;
static int chunk_copies;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Chunk *chunk_new(void) {
    Chunk *c = calloc(1, sizeof *c);
    check(c != NULL, "alloc");
    c->rc = 1;
    live_chunks++;
    return c;
}

static void chunk_release(Chunk *c) {
    if (--c->rc == 0) {
        free(c);
        live_chunks--;
    }
}

static CowArray arr_new(int fill) {
    CowArray a;
    for (int i = 0; i < NCHUNKS; i++) {
        a.chunks[i] = chunk_new();
        for (int j = 0; j < CHUNK; j++)
            a.chunks[i]->data[j] = fill;
    }
    return a;
}

/* O(#chunks) snapshot, zero element copies. */
static CowArray arr_snapshot(const CowArray *a) {
    CowArray s = *a;
    for (int i = 0; i < NCHUNKS; i++)
        s.chunks[i]->rc++;
    return s;
}

static void arr_free(CowArray *a) {
    for (int i = 0; i < NCHUNKS; i++) {
        chunk_release(a->chunks[i]);
        a->chunks[i] = NULL;
    }
}

static int arr_get(const CowArray *a, int i) { return a->chunks[i / CHUNK]->data[i % CHUNK]; }

static void arr_set(CowArray *a, int i, int v) {
    Chunk **slot = &a->chunks[i / CHUNK];
    if ((*slot)->rc > 1) {
        Chunk *n = chunk_new();
        memcpy(n->data, (*slot)->data, sizeof n->data);
        (*slot)->rc--;
        *slot = n;
        chunk_copies++;
    }
    (*slot)->data[i % CHUNK] = v;
}

static int shared_chunks(const CowArray *a, const CowArray *b) {
    int n = 0;
    for (int i = 0; i < NCHUNKS; i++)
        n += a->chunks[i] == b->chunks[i];
    return n;
}

static unsigned checksum(const CowArray *a) {
    unsigned s = 0;
    for (int i = 0; i < TOTAL; i++)
        s = s * 31u + (unsigned)arr_get(a, i);
    return s % 1000003u;
}

int main(void) {
    CowArray v0 = arr_new(0);
    for (int i = 0; i < TOTAL; i++)
        arr_set(&v0, i, i);
    printf("v0 built: live chunks=%d copies=%d\n", live_chunks, chunk_copies);

    CowArray v1 = arr_snapshot(&v0);
    printf("snapshot: live chunks=%d shared=%d/%d\n", live_chunks, shared_chunks(&v0, &v1), NCHUNKS);

    arr_set(&v1, 3, -3);
    arr_set(&v1, 4, -4); /* same chunk: only one copy */
    arr_set(&v1, 60, -60);
    printf("v1 edited 3 cells in 2 chunks: live=%d copies=%d shared=%d\n", live_chunks, chunk_copies,
           shared_chunks(&v0, &v1));

    CowArray v2 = arr_snapshot(&v1);
    arr_set(&v0, 3, 333); /* v0's chunk 0 is already private (v1 copied it), so no new copy */
    printf("v0 write to chunk 0: copies=%d shared(v0,v1)=%d shared(v1,v2)=%d\n", chunk_copies,
           shared_chunks(&v0, &v1), shared_chunks(&v1, &v2));

    printf("cell 3: v0=%d v1=%d v2=%d\n", arr_get(&v0, 3), arr_get(&v1, 3), arr_get(&v2, 3));
    check(arr_get(&v0, 3) == 333 && arr_get(&v1, 3) == -3 && arr_get(&v2, 3) == -3, "isolation");
    check(arr_get(&v0, 60) == 60 && arr_get(&v1, 60) == -60, "isolation 60");

    /* many versions from a rolling window of snapshots */
    CowArray hist[6];
    for (int i = 0; i < 6; i++) {
        hist[i] = arr_snapshot(&v2);
        arr_set(&v2, i * 9, 1000 + i);
    }
    int total_shared = 0;
    for (int i = 0; i < 6; i++)
        total_shared += shared_chunks(&hist[i], &v2);
    printf("history of 6: live chunks=%d (a full copy each would be %d), shared chunk pairs with head=%d\n",
           live_chunks, 9 * NCHUNKS, total_shared);
    printf("checksums: v0=%u v1=%u v2=%u h0=%u h5=%u\n", checksum(&v0), checksum(&v1),
           checksum(&v2), checksum(&hist[0]), checksum(&hist[5]));

    for (int i = 0; i < 6; i++)
        arr_free(&hist[i]);
    arr_free(&v0);
    arr_free(&v1);
    arr_free(&v2);
    check(live_chunks == 0, "leak");
    printf("final live chunks=%d\n", live_chunks);
    return 0;
}
