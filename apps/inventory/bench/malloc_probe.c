/* Optional glibc allocator sampler for soak.sh. Loaded only into the service
 * with LD_PRELOAD; one diagnostic thread writes mallinfo2 totals and the
 * malloc_info arena count every ten seconds. */
#define _GNU_SOURCE
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int arena_count(void) {
    char *xml = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&xml, &size);
    if (!stream) return -1;
    if (malloc_info(0, stream) != 0) {
        fclose(stream);
        free(xml);
        return -1;
    }
    fclose(stream);
    int count = 0;
    for (char *p = xml; p && (p = strstr(p, "<heap nr=")); p += 9) count++;
    free(xml);
    return count;
}

static void *sample(void *unused) {
    (void)unused;
    const char *path = getenv("IRON_MALLOC_STATS_FILE");
    if (!path || !*path) return NULL;
    FILE *out = fopen(path, "w");
    if (!out) return NULL;
    setvbuf(out, NULL, _IOLBF, 0);
    fputs("seconds,arena_mb,in_use_mb,retained_free_mb,mmap_mb,arenas\n", out);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        sleep(10);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct mallinfo2 m = mallinfo2();
        double elapsed = now.tv_sec - start.tv_sec +
                         (now.tv_nsec - start.tv_nsec) / 1000000000.0;
        fprintf(out, "%.0f,%.3f,%.3f,%.3f,%.3f,%d\n", elapsed,
                m.arena / 1048576.0, m.uordblks / 1048576.0,
                m.fordblks / 1048576.0, m.hblkhd / 1048576.0,
                arena_count());
    }
}

__attribute__((constructor)) static void start_sampler(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, sample, NULL) == 0) pthread_detach(thread);
}
