/*
 * title: Atomic O_APPEND records from many processes
 * topic: io_files
 * covers: O_APPEND, fork, single write per record, length-prefixed records, integrity check, per-writer ordering
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)


static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned rnd_below(unsigned n) {
    unsigned v = (unsigned)(xs() >> 33);
    return v % n;
}

/* read until n bytes or EOF; returns count or -1 */
static inline long rd_full(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (long)got;
}
static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
#define KIDS 6
#define PER_KID 150

/* record: u8 writer, u16 seq (LE), u8 len, payload[len], u32 crc-ish (LE) */
static unsigned check32(const unsigned char *p, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void child(int id) {
    unsigned char rec[4 + 255 + 4];
    xs_state = 1000003ULL * (unsigned long long)(id + 1);
    int fd = open("shared.log", O_WRONLY | O_APPEND);
    if (fd < 0) _exit(2);
    for (int s = 0; s < PER_KID; s++) {
        unsigned len = 1 + rnd_below(120);
        rec[0] = (unsigned char)id;
        rec[1] = (unsigned char)(s & 0xff);
        rec[2] = (unsigned char)(s >> 8);
        rec[3] = (unsigned char)len;
        for (unsigned i = 0; i < len; i++) rec[4 + i] = (unsigned char)('a' + (id + s + (int)i) % 26);
        unsigned c = check32(rec, 4 + len);
        for (int k = 0; k < 4; k++) rec[4 + len + (unsigned)k] = (unsigned char)(c >> (8 * k));
        /* the whole record goes out in ONE write so O_APPEND keeps it contiguous */
        if (write(fd, rec, 8 + len) != (ssize_t)(8 + len)) _exit(3);
    }
    close(fd);
    _exit(0);
}

int main(void) {
    int fd = open("shared.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    close(fd);

    pid_t pids[KIDS];
    for (int i = 0; i < KIDS; i++) {
        pids[i] = fork();
        CHECK(pids[i] >= 0);
        if (pids[i] == 0) child(i);
    }
    for (int i = 0; i < KIDS; i++) CHECK(wait_exit(pids[i]) == 0);

    fd = open("shared.log", O_RDONLY);
    CHECK(fd >= 0);
    long size = fsize(fd);
    unsigned char *all = malloc((size_t)size);
    CHECK(all != NULL);
    CHECK(rd_full(fd, all, (size_t)size) == size);
    close(fd);

    int next_seq[KIDS] = {0};
    long bytes_by[KIDS] = {0};
    long pos = 0, records = 0, bad_crc = 0, bad_order = 0, bad_payload = 0;
    while (pos < size) {
        CHECK(pos + 8 <= size);
        int id = all[pos];
        int seq = all[pos + 1] | (all[pos + 2] << 8);
        unsigned len = all[pos + 3];
        CHECK(id < KIDS && len >= 1 && pos + 8 + (long)len <= size);
        unsigned want = check32(all + pos, 4 + len);
        unsigned got = 0;
        for (int k = 0; k < 4; k++) got |= (unsigned)all[pos + 4 + (long)len + k] << (8 * k);
        if (want != got) bad_crc++;
        if (seq != next_seq[id]) bad_order++;
        next_seq[id] = seq + 1;
        for (unsigned i = 0; i < len; i++)
            if (all[pos + 4 + (long)i] != (unsigned char)('a' + (id + seq + (int)i) % 26)) bad_payload++;
        bytes_by[id] += 8 + len;
        pos += 8 + len;
        records++;
    }
    free(all);
    printf("records=%ld crc_failures=%ld order_failures=%ld payload_failures=%ld\n", records, bad_crc, bad_order, bad_payload);
    CHECK(records == KIDS * PER_KID && bad_crc == 0 && bad_order == 0 && bad_payload == 0);
    long sum = 0;
    for (int i = 0; i < KIDS; i++) {
        printf("writer %d: records=%d bytes=%ld\n", i, next_seq[i], bytes_by[i]);
        CHECK(next_seq[i] == PER_KID);
        sum += bytes_by[i];
    }
    CHECK(sum == size);
    printf("file size equals sum of records: %ld\n", size);
    unlink("shared.log");
    return 0;
}
