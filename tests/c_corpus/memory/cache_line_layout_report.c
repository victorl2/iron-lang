/*
 * title: Cache-line aware layouts: hot/cold split and false-sharing padding
 * topic: memory
 * covers: 64-byte line model, struct splitting, padding to line size, line-crossing analysis, alignas
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE 64u

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Original record: hot fields scattered among cold ones. */
typedef struct {
    uint64_t key;
    char name[40];
    uint32_t hits;
    uint8_t flags;
    uint8_t tier;
    uint16_t shard;
    uint64_t created;
    uint64_t updated;
    char note[24];
    uint32_t score;
    uint32_t version;
} Record;

/* Split: fields touched together on lookups. */
typedef struct {
    uint64_t key;
    uint32_t hits;
    uint32_t score;
    uint8_t flags;
    uint8_t tier;
    uint16_t shard;
} Hot;

typedef struct {
    char name[40];
    char note[24];
    uint64_t created;
    uint64_t updated;
    uint32_t version;
} Cold;

/* Per-thread counters: unpadded vs one line each. */
typedef struct {
    uint64_t count;
} PackedCounter;

typedef struct {
    _Alignas(LINE) uint64_t count;
} LineCounter;

/* How many distinct cache lines does the byte range [off, off+len) touch, given the base address alignment? */
static unsigned lines_touched(size_t base_misalign, size_t off, size_t len) {
    size_t first = (base_misalign + off) / LINE;
    size_t last = (base_misalign + off + len - 1) / LINE;
    return (unsigned)(last - first + 1);
}

/* number of elements of an array whose members straddle a line boundary */
static unsigned straddlers(size_t elem, size_t count) {
    unsigned n = 0;
    for (size_t i = 0; i < count; i++) {
        size_t start = i * elem, end = start + elem - 1;
        if (start / LINE != end / LINE)
            n++;
    }
    return n;
}

static unsigned distinct_lines_for_array(size_t elem, size_t count) {
    return (unsigned)((elem * count + LINE - 1) / LINE);
}

int main(void) {
    printf("Record: size %zu align %zu lines %u\n", sizeof(Record), _Alignof(Record),
           (unsigned)((sizeof(Record) + LINE - 1) / LINE));
    printf("Hot:    size %zu align %zu\n", sizeof(Hot), _Alignof(Hot));
    printf("Cold:   size %zu align %zu\n", sizeof(Cold), _Alignof(Cold));
    check(sizeof(Hot) == 24, "hot size");
    check(sizeof(Record) == 104, "record size");

    /* hot fields of the original record span which lines? (record at line-aligned base) */
    size_t hot_offs[] = {offsetof(Record, key), offsetof(Record, hits), offsetof(Record, flags),
                         offsetof(Record, tier), offsetof(Record, shard), offsetof(Record, score)};
    size_t hot_lens[] = {8, 4, 1, 1, 2, 4};
    unsigned mask_lo = 0;
    for (size_t i = 0; i < 6; i++)
        for (size_t b = hot_offs[i]; b < hot_offs[i] + hot_lens[i]; b++)
            mask_lo |= 1u << (b / LINE);
    unsigned nlines = 0;
    for (unsigned m = mask_lo; m; m >>= 1)
        nlines += m & 1u;
    printf("hot fields of Record touch %u lines (mask 0x%x)\n", nlines, mask_lo);
    printf("hot struct touches %u line per record when aligned\n", lines_touched(0, 0, sizeof(Hot)));

    /* scanning N records: cache lines fetched for the hot fields only */
    enum { N = 1000 };
    unsigned long lines_aos = 0, lines_split = 0;
    for (size_t i = 0; i < N; i++) {
        size_t base = i * sizeof(Record);
        /* set of lines touched by this record's hot fields */
        unsigned long seen[4] = {0, 0, 0, 0};
        unsigned cnt = 0;
        for (size_t f = 0; f < 6; f++) {
            size_t a = (base + hot_offs[f]) / LINE, b = (base + hot_offs[f] + hot_lens[f] - 1) / LINE;
            for (size_t l = a; l <= b; l++) {
                int dup = 0;
                for (unsigned q = 0; q < cnt; q++)
                    if (seen[q] == l)
                        dup = 1;
                if (!dup)
                    seen[cnt++] = l;
            }
        }
        lines_aos += cnt;
        (void)seen;
    }
    /* split layout: contiguous Hot array, only new lines count */
    lines_split = distinct_lines_for_array(sizeof(Hot), N);
    printf("scan of %d records: AoS-lines(with duplicates across records)=%lu, split hot array lines=%lu\n", N,
           lines_aos, lines_split);
    check(lines_split < lines_aos, "split reads fewer lines");

    printf("Hot elements straddling lines in a %d-array: %u\n", N, straddlers(sizeof(Hot), N));
    printf("Record elements straddling lines: %u of %d\n", straddlers(sizeof(Record), N), N);
    /* an element size that divides the line never straddles */
    check(straddlers(16, 1000) == 0 && straddlers(32, 1000) == 0 && straddlers(64, 1000) == 0, "divisors");
    check(straddlers(24, 1000) > 0, "24 straddles");
    printf("straddlers for sizes 16/24/32/48/64: %u %u %u %u %u (of 1000)\n", straddlers(16, 1000),
           straddlers(24, 1000), straddlers(32, 1000), straddlers(48, 1000), straddlers(64, 1000));

    /* false sharing: adjacent counters share a line unless padded */
    PackedCounter pc[8];
    LineCounter lc[8];
    unsigned shared_pairs_packed = 0, shared_pairs_padded = 0;
    for (int i = 0; i + 1 < 8; i++) {
        size_t a = (size_t)i * sizeof pc[0] / LINE, b = (size_t)(i + 1) * sizeof pc[0] / LINE;
        if (a == b)
            shared_pairs_packed++;
        a = (size_t)i * sizeof lc[0] / LINE;
        b = (size_t)(i + 1) * sizeof lc[0] / LINE;
        if (a == b)
            shared_pairs_padded++;
    }
    printf("PackedCounter size %zu: neighbours sharing a line %u of 7\n", sizeof(PackedCounter), shared_pairs_packed);
    printf("LineCounter   size %zu align %zu: neighbours sharing a line %u of 7\n", sizeof(LineCounter),
           _Alignof(LineCounter), shared_pairs_padded);
    check(shared_pairs_padded == 0 && sizeof(LineCounter) == LINE, "padded counters");

    /* the same 8 counters padded by hand, for compilers without alignas on members */
    typedef struct {
        uint64_t count;
        unsigned char pad[LINE - sizeof(uint64_t)];
    } ManualPadded;
    check(sizeof(ManualPadded) == LINE, "manual padding");
    printf("manual padding size: %zu\n", sizeof(ManualPadded));

    /* an object placed at every misalignment: lines touched by a 24-byte Hot */
    unsigned by_mis[3] = {0, 0, 0};
    for (size_t mis = 0; mis < LINE; mis += 8) {
        unsigned lt = lines_touched(mis, 0, sizeof(Hot));
        by_mis[lt]++;
    }
    printf("Hot at 8 base offsets within a line: 1 line x%u, 2 lines x%u\n", by_mis[1], by_mis[2]);
    return 0;
}
