/*
 * title: Deferred destruction queue with frame delay
 * topic: memory
 * covers: deferred free, retire queue, lifetime tied to frames in flight, ring buffer of pending frees, poison on free
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_IN_FLIGHT 3
#define MAX_PENDING 64

typedef struct {
    int id;
    int size;
    unsigned char *mem;
    int last_used_frame;
} Resource;

typedef struct {
    Resource *res;
    int retire_frame;
} Pending;

static Pending queue[MAX_PENDING];
static int qn;
static int live_bytes;
static int frame;
static int freed_ids[32];
static int nfreed;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Resource *res_new(int id, int size) {
    Resource *r = malloc(sizeof *r);
    check(r != NULL, "alloc");
    r->id = id;
    r->size = size;
    r->mem = malloc((size_t)size);
    check(r->mem != NULL, "alloc");
    memset(r->mem, id, (size_t)size);
    r->last_used_frame = -1;
    live_bytes += size;
    return r;
}

static void res_destroy_now(Resource *r) {
    check(frame - r->last_used_frame >= FRAMES_IN_FLIGHT, "destroyed too early");
    memset(r->mem, 0xDE, (size_t)r->size);
    live_bytes -= r->size;
    freed_ids[nfreed++] = r->id;
    free(r->mem);
    free(r);
}

/* Users call this instead of freeing: destruction waits until no frame in flight can see it. */
static void res_retire(Resource *r) {
    check(qn < MAX_PENDING, "queue full");
    queue[qn].res = r;
    queue[qn].retire_frame = frame;
    qn++;
}

static int flush_ready(void) {
    int w = 0, destroyed = 0;
    for (int i = 0; i < qn; i++) {
        if (frame - queue[i].retire_frame >= FRAMES_IN_FLIGHT) {
            res_destroy_now(queue[i].res);
            destroyed++;
        } else {
            queue[w++] = queue[i];
        }
    }
    qn = w;
    return destroyed;
}

static void use(Resource *r) {
    check(r->mem[0] == (unsigned char)r->id, "use after destroy");
    r->last_used_frame = frame;
}

int main(void) {
    Resource *live[8] = {0};
    int next_id = 1;
    unsigned s = 2463534242u;
    int max_pending = 0;
    for (frame = 0; frame < 20; frame++) {
        int ready = flush_ready();
        for (int i = 0; i < 8; i++)
            if (live[i])
                use(live[i]);
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int slot = (int)(s % 8);
        if (live[slot]) {
            res_retire(live[slot]); /* replace: old one is still visible to in-flight frames */
            live[slot] = NULL;
        }
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        live[slot] = res_new(next_id++, 16 + (int)(s % 4) * 16);
        if (qn > max_pending)
            max_pending = qn;
        printf("frame %2d: destroyed=%d pending=%d live_bytes=%d\n", frame, ready, qn, live_bytes);
    }
    printf("max pending %d\n", max_pending);
    /* shutdown: retire everything, then advance frames until the queue drains */
    for (int i = 0; i < 8; i++)
        if (live[i])
            res_retire(live[i]);
    int extra = 0;
    while (qn) {
        frame++;
        extra++;
        flush_ready();
    }
    printf("drained after %d frames, live_bytes=%d, total freed=%d\n", extra, live_bytes, nfreed);
    check(live_bytes == 0 && nfreed == next_id - 1, "all destroyed");
    check(extra == FRAMES_IN_FLIGHT, "drain time");
    return 0;
}
