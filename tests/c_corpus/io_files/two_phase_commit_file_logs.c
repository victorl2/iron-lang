/*
 * title: Two-phase commit across file-backed participants with crash sweep
 * topic: io_files
 * covers: prepare/commit/abort logs, coordinator decision record, presumed abort, in-doubt resolution on recovery, crash at every protocol step, atomicity and conservation invariants
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

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
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

enum { NP = 3, NTX = 4 };

static long budget; /* protocol steps allowed before the crash; <0 = unlimited */
static int crashed;
static int steps;

static int step(void) {
    steps++;
    if (crashed)
        return 0;
    if (budget == 0) {
        crashed = 1;
        return 0;
    }
    if (budget > 0)
        budget--;
    return 1;
}

static void append_line(const char *path, const char *line) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    check(fd >= 0, "append open");
    write_all(fd, line, strlen(line));
    write_all(fd, "\n", 1);
    close(fd);
}

static void pname(char *o, size_t c, int p, const char *ext) { snprintf(o, c, "part%d.%s", p, ext); }

/* State file "<balance> <mask>": the mask records which transactions were already applied, so that the
 * balance change and its "done" marker are made durable by one atomic rename. */
static void read_state(int p, long *bal, unsigned *mask) {
    char nm[32];
    pname(nm, sizeof nm, p, "bal");
    size_t n;
    unsigned char *b = slurp(nm, &n);
    b[n] = 0;
    unsigned m = 0;
    check(sscanf((char *)b, "%ld %u", bal, &m) == 2, "state parse");
    *mask = m;
    free(b);
}
static void write_state(int p, long bal, unsigned mask) {
    char nm[32], buf[48];
    pname(nm, sizeof nm, p, "bal");
    int l = snprintf(buf, sizeof buf, "%ld %u", bal, mask);
    spit("tmp.bal", buf, (size_t)l);
    check(rename("tmp.bal", nm) == 0, "state rename");
}
static long read_balance(int p) {
    long b;
    unsigned m;
    read_state(p, &b, &m);
    return b;
}

static int log_has(int p, const char *line) {
    char nm[32];
    pname(nm, sizeof nm, p, "log");
    if (file_size(nm) < 0)
        return 0;
    size_t n;
    unsigned char *b = slurp(nm, &n);
    b[n] = 0;
    size_t ll = strlen(line);
    int found = 0;
    for (char *s = (char *)b; *s;) {
        char *e = strchr(s, '\n');
        if (!e)
            break;
        if ((size_t)(e - s) == ll && !strncmp(s, line, ll))
            found = 1;
        s = e + 1;
    }
    free(b);
    return found;
}

/* Transaction ids stay below 10, so a substring test on single-digit ids is exact. */
static int coord_decision(int tx) { /* 1 commit; 0 abort, including "no record" (presumed abort) */
    char line[32];
    snprintf(line, sizeof line, "COMMIT %d\n", tx);
    if (file_size("coord.log") < 0)
        return 0;
    size_t n;
    unsigned char *b = slurp("coord.log", &n);
    b[n] = 0;
    int r = strstr((char *)b, line) != NULL;
    free(b);
    return r;
}

static long deltas[NTX][NP];

static void apply_decision(int p, int tx, int commit) {
    long bal;
    unsigned mask;
    read_state(p, &bal, &mask);
    if (mask & (1u << tx))
        return; /* already applied */
    write_state(p, commit ? bal + deltas[tx][p] : bal, mask | (1u << tx));
}

static void run_tx(int tx) {
    char line[64], nm[32];
    int vote_yes[NP];
    for (int p = 0; p < NP; p++) {
        if (deltas[tx][p] == 0) {
            vote_yes[p] = 1;
            continue;
        }
        if (!step())
            return;
        int ok = read_balance(p) + deltas[tx][p] >= 0;
        vote_yes[p] = ok;
        pname(nm, sizeof nm, p, "log");
        snprintf(line, sizeof line, ok ? "PREPARED %d" : "VOTENO %d", tx);
        append_line(nm, line);
    }
    int commit = 1;
    for (int p = 0; p < NP; p++)
        commit &= vote_yes[p];
    if (!step())
        return;
    snprintf(line, sizeof line, commit ? "COMMIT %d" : "ABORT %d", tx);
    append_line("coord.log", line); /* the decision point */
    for (int p = 0; p < NP; p++) {
        if (deltas[tx][p] == 0)
            continue;
        if (!step())
            return;
        apply_decision(p, tx, commit);
    }
}

/* Recovery: a participant that logged PREPARED but never applied a decision asks the coordinator. */
static int recover(void) {
    int resolved = 0;
    for (int tx = 0; tx < NTX; tx++)
        for (int p = 0; p < NP; p++) {
            char prep[32];
            snprintf(prep, sizeof prep, "PREPARED %d", tx);
            long bal;
            unsigned mask;
            read_state(p, &bal, &mask);
            if (log_has(p, prep) && !(mask & (1u << tx))) {
                apply_decision(p, tx, coord_decision(tx));
                resolved++;
            }
        }
    return resolved;
}

static void reset(void) {
    char nm[32];
    for (int p = 0; p < NP; p++) {
        pname(nm, sizeof nm, p, "log");
        unlink(nm);
        write_state(p, 100, 0);
    }
    unlink("coord.log");
}

int main(void) {
    /* Transfers between participants; tx 2 overdraws participant 1 and must abort. */
    static const long d[NTX][NP] = {{-30, 30, 0}, {0, -20, 20}, {10, -200, 190}, {-50, 25, 25}};
    memcpy(deltas, d, sizeof deltas);
    budget = -1;
    crashed = 0;
    steps = 0;
    reset();
    for (int tx = 0; tx < NTX; tx++)
        run_tx(tx);
    int total_steps = steps;
    long fin[NP];
    for (int p = 0; p < NP; p++)
        fin[p] = read_balance(p);
    printf("protocol steps for %d transactions=%d\n", NTX, total_steps);
    printf("crash-free result: %ld %ld %ld\n", fin[0], fin[1], fin[2]);
    check(fin[0] + fin[1] + fin[2] == 300, "conservation");

    int in_doubt_total = 0, worst = 0;
    int outcomes[2] = {0, 0};
    for (int k = 0; k <= total_steps; k++) {
        reset();
        budget = k;
        crashed = 0;
        steps = 0;
        for (int tx = 0; tx < NTX && !crashed; tx++)
            run_tx(tx);
        int r = recover();
        int r2 = recover(); /* recovery is idempotent: nothing left to resolve */
        check(r2 == 0, "second recovery finds nothing");
        in_doubt_total += r;
        if (r > worst)
            worst = r;
        long expect[NP] = {100, 100, 100};
        for (int tx = 0; tx < NTX; tx++)
            if (coord_decision(tx))
                for (int p = 0; p < NP; p++)
                    expect[p] += deltas[tx][p];
        long sum = 0;
        for (int p = 0; p < NP; p++) {
            long b = read_balance(p);
            check(b == expect[p], "participant equals coordinator-decided state");
            check(b >= 0, "no negative balance");
            sum += b;
        }
        check(sum == 300, "money conserved");
        check(!coord_decision(2), "overdraw transaction never commits");
        int full = expect[0] == fin[0] && expect[1] == fin[1] && expect[2] == fin[2];
        outcomes[full]++;
    }
    printf("crash points=%d in-doubt participants resolved=%d (max per crash %d)\n", total_steps + 1, in_doubt_total,
           worst);
    printf("recovered to full outcome=%d, to an earlier consistent prefix=%d\n", outcomes[1], outcomes[0]);
    for (int p = 0; p < NP; p++) {
        char nm[32];
        pname(nm, sizeof nm, p, "log");
        unlink(nm);
        pname(nm, sizeof nm, p, "bal");
        unlink(nm);
    }
    unlink("coord.log");
    return 0;
}
