/* Allocation counter for run_fusible_bench.py. The benchmark is linked with
 * these definitions of the runtime's allocation functions in place of the
 * ones in iron_os.c (which the script compiles with the three names
 * suffixed _real), so every allocation of the program and of the runtime
 * is counted. A realloc that grows a list counts as one allocation of the
 * new size. The totals are printed when the program exits. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

void *iron_mem_alloc_real(size_t size);
void *iron_mem_calloc_real(size_t count, size_t size);
void *iron_mem_realloc_real(void *ptr, size_t size);

static unsigned long long bench_calls;
static unsigned long long bench_bytes;
static int bench_registered;

static void bench_report(void) {
    fprintf(stderr, "allocations %llu bytes %llu\n", bench_calls, bench_bytes);
}

static void bench_note(size_t size) {
    if (!bench_registered) {
        bench_registered = 1;
        atexit(bench_report);
    }
    bench_calls++;
    bench_bytes += size;
}

void *iron_mem_alloc(size_t size) {
    bench_note(size);
    return iron_mem_alloc_real(size);
}

void *iron_mem_calloc(size_t count, size_t size) {
    bench_note(count * size);
    return iron_mem_calloc_real(count, size);
}

void *iron_mem_realloc(void *ptr, size_t size) {
    bench_note(size);
    return iron_mem_realloc_real(ptr, size);
}
