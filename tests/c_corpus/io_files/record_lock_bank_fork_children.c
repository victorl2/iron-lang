/*
 * title: Record-level locking database updated by forked workers
 * topic: io_files
 * covers: fcntl byte-range record locks, ordered lock acquisition, fork workers, commutative transfers, invariant check, pipes for results
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rndn(uint32_t n) {
    uint64_t v = rnd();
    return (uint32_t)((v >> 16) % n);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write all bytes at an offset; abort the program on failure. */
static inline void pwrite_all(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, off);
        check(w > 0, "pwrite");
        p += w;
        off += w;
        n -= (size_t)w;
    }
}

/* Read up to n bytes at offset; returns the number of bytes read (short at EOF). */
static inline size_t pread_upto(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        check(r >= 0, "pread");
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got;
}

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer (caller frees). */
static inline unsigned char *slurp(const char *path, size_t *len) {
    long n = file_size(path);
    check(n >= 0, "slurp stat");
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    check(b != NULL, "malloc");
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "slurp open");
    size_t got = pread_upto(fd, b, (size_t)n, 0);
    close(fd);
    check(got == (size_t)n, "slurp short");
    *len = (size_t)n;
    return b;
}

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

#include <sys/wait.h>
#include <signal.h>

/* Record i lives at offset i*REC: balance u64 (two's complement) | ops u32 | pad u32. */
enum { REC = 16, NACC = 8, WORKERS = 4, TRANSFERS = 250 };

static int lock_rec(int fd, int idx, int type) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = (short)type;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)idx * REC;
    fl.l_len = REC;
    int r;
    do
        r = fcntl(fd, F_SETLKW, &fl);
    while (r != 0 && errno == EINTR);
    return r;
}

static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }
static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void add_to(int fd, int idx, uint64_t delta) {
    unsigned char r[REC];
    check(pread_upto(fd, r, REC, (off_t)idx * REC) == REC, "rec read");
    put64(r, get64(r) + delta);
    put32(r + 8, get32(r + 8) + 1);
    pwrite_all(fd, r, REC, (off_t)idx * REC);
}

typedef struct {
    int from, to;
    uint32_t amount;
} Xfer;

/* Deterministic transfer list for a worker, independent of database state. */
static void gen_transfers(int worker, Xfer *out) {
    uint64_t saved = rng_state;
    rng_state = 0x5EED0000ULL + (uint64_t)worker * 7919u;
    for (int i = 0; i < TRANSFERS; i++) {
        int a = (int)rndn(NACC);
        int b = (int)rndn(NACC - 1);
        if (b >= a)
            b++;
        out[i].from = a;
        out[i].to = b;
        out[i].amount = 1u + rndn(100);
    }
    rng_state = saved;
}

static void worker_main(int worker, int wfd) {
    alarm(20);
    int fd = open("bank.db", O_RDWR);
    check(fd >= 0, "worker open");
    Xfer xs[TRANSFERS];
    gen_transfers(worker, xs);
    uint64_t total = 0;
    for (int i = 0; i < TRANSFERS; i++) {
        int lo = xs[i].from < xs[i].to ? xs[i].from : xs[i].to;
        int hi = xs[i].from < xs[i].to ? xs[i].to : xs[i].from;
        check(lock_rec(fd, lo, F_WRLCK) == 0, "lock lo");
        check(lock_rec(fd, hi, F_WRLCK) == 0, "lock hi");
        add_to(fd, xs[i].from, (uint64_t)0 - xs[i].amount);
        add_to(fd, xs[i].to, xs[i].amount);
        check(lock_rec(fd, hi, F_UNLCK) == 0, "unlock hi");
        check(lock_rec(fd, lo, F_UNLCK) == 0, "unlock lo");
        total += xs[i].amount;
    }
    close(fd);
    unsigned char rep[8];
    put64(rep, total);
    write_all(wfd, rep, 8);
    close(wfd);
    _exit(0);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unsigned char db[NACC * REC];
    memset(db, 0, sizeof db);
    for (int i = 0; i < NACC; i++)
        put64(db + i * REC, 1000u);
    spit("bank.db", db, sizeof db);
    int pfd[WORKERS][2];
    pid_t pid[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        check(pipe(pfd[w]) == 0, "pipe");
        pid[w] = fork();
        check(pid[w] >= 0, "fork");
        if (pid[w] == 0) {
            for (int k = 0; k < w; k++)
                close(pfd[k][0]);
            close(pfd[w][0]);
            worker_main(w, pfd[w][1]);
        }
        close(pfd[w][1]);
    }
    uint64_t moved[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        unsigned char rep[8];
        size_t got = 0;
        while (got < 8) {
            ssize_t r = read(pfd[w][0], rep + got, 8 - got);
            check(r > 0, "worker report");
            got += (size_t)r;
        }
        moved[w] = get64(rep);
        close(pfd[w][0]);
        int status = 0;
        check(waitpid(pid[w], &status, 0) == pid[w], "waitpid");
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "worker exit clean");
    }
    /* Replay every worker's transfers serially: additions commute, so balances must match exactly. */
    uint64_t exp_bal[NACC];
    uint32_t exp_ops[NACC];
    for (int i = 0; i < NACC; i++) {
        exp_bal[i] = 1000u;
        exp_ops[i] = 0;
    }
    uint64_t exp_moved[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        Xfer xs[TRANSFERS];
        gen_transfers(w, xs);
        exp_moved[w] = 0;
        for (int i = 0; i < TRANSFERS; i++) {
            exp_bal[xs[i].from] -= xs[i].amount;
            exp_bal[xs[i].to] += xs[i].amount;
            exp_ops[xs[i].from]++;
            exp_ops[xs[i].to]++;
            exp_moved[w] += xs[i].amount;
        }
        check(moved[w] == exp_moved[w], "worker total moved");
    }
    size_t n;
    unsigned char *fin = slurp("bank.db", &n);
    check(n == sizeof db, "db size");
    uint64_t sum = 0;
    long ops = 0;
    for (int i = 0; i < NACC; i++) {
        uint64_t bal = get64(fin + i * REC);
        check(bal == exp_bal[i], "final balance");
        check(get32(fin + i * REC + 8) == exp_ops[i], "final op count");
        sum += bal;
        ops += get32(fin + i * REC + 8);
        printf("account %d: balance=%lld ops=%u\n", i, (long long)(int64_t)bal, get32(fin + i * REC + 8));
    }
    free(fin);
    check(sum == 1000u * NACC, "money conserved");
    printf("sum=%llu total record updates=%ld workers=%d transfers each=%d\n", (unsigned long long)sum, ops, WORKERS,
           TRANSFERS);
    for (int w = 0; w < WORKERS; w++)
        printf("worker %d moved %llu\n", w, (unsigned long long)moved[w]);
    unlink("bank.db");
    return 0;
}
