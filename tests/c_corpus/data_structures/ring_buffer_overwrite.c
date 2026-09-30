/*
 * title: Overwriting ring buffer with free-running indices
 * topic: data_structures
 * covers: ring buffer, power-of-two mask, free-running counters, overwrite oldest, unsigned wraparound, moving sum
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x3141592653589793ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 13); }

#define SIZE 16u /* power of two */
#define MASK (SIZE - 1u)

typedef struct {
    int32_t buf[SIZE];
    uint32_t rd, wr;        /* free running, wrap at 2^32 */
    int64_t sum;            /* sum of stored elements */
    uint64_t overwritten;
} Ring;

static uint32_t count(const Ring *r) { return r->wr - r->rd; }
static void put(Ring *r, int32_t v) {
    if (count(r) == SIZE) { r->sum -= r->buf[r->rd & MASK]; r->rd++; r->overwritten++; }
    r->buf[r->wr & MASK] = v; r->wr++; r->sum += v;
}
static int get(Ring *r, int32_t *v) {
    if (count(r) == 0) return 0;
    *v = r->buf[r->rd & MASK]; r->rd++; r->sum -= *v; return 1;
}
static int32_t peek_newest(const Ring *r, uint32_t back) { CHECK(back < count(r)); return r->buf[(r->wr - 1 - back) & MASK]; }

int main(void) {
    Ring r; memset(&r, 0, sizeof r);
    /* start the counters near the 32-bit wrap so unsigned overflow is exercised */
    r.rd = r.wr = 0xFFFFFFF0u;
    int32_t model[SIZE]; uint32_t mn = 0;
    uint64_t total_put = 0, total_get = 0, mo = 0;
    int64_t peak_sum = 0, trough = 0;
    long fullness[SIZE + 1] = {0};
    for (int step = 0; step < 50000; step++) {
        int burst = (step / 40) % 3;
        int p = burst == 0 ? 75 : burst == 1 ? 40 : 55;
        if ((int)(rnd() % 100) < p) {
            int32_t v = (int32_t)(rnd() % 2001) - 1000;
            put(&r, v); total_put++;
            if (mn == SIZE) { memmove(model, model + 1, (SIZE - 1) * sizeof(int32_t)); model[SIZE - 1] = v; mo++; }
            else model[mn++] = v;
        } else {
            int32_t v = 0;
            int ok = get(&r, &v);
            CHECK(ok == (mn > 0));
            if (ok) { CHECK(v == model[0]); memmove(model, model + 1, (size_t)(--mn) * sizeof(int32_t)); total_get++; }
        }
        CHECK(count(&r) == mn && r.overwritten == mo);
        int64_t ms = 0; for (uint32_t i = 0; i < mn; i++) ms += model[i];
        CHECK(ms == r.sum);
        if (r.sum > peak_sum) peak_sum = r.sum;
        if (r.sum < trough) trough = r.sum;
        for (uint32_t i = 0; i < mn && i < 3; i++) CHECK(peek_newest(&r, i) == model[mn - 1 - i]);
        fullness[mn]++;
    }
    printf("put=%llu get=%llu overwritten=%llu\n", (unsigned long long)total_put, (unsigned long long)total_get, (unsigned long long)r.overwritten);
    printf("peak_sum=%lld lowest_sum=%lld final_count=%u\n", (long long)peak_sum, (long long)trough, count(&r));
    printf("wr wrapped past 2^32: %s\n", r.wr < 0xFFFFFFF0u ? "yes" : "no");
    printf("time at each fill level:");
    for (int i = 0; i <= (int)SIZE; i++) printf(" %ld", fullness[i]);
    printf("\n");
    return 0;
}
