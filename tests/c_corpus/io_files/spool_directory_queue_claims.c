/*
 * title: Maildir-style spool queue with rename-based claims, retries and dead letters
 * topic: io_files
 * covers: tmp/new/cur/done/dead directories, atomic rename claim, competing consumers, retry counter in file name, stale claim recovery, sorted readdir
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

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

#include <dirent.h>

static const char *dirs[5] = {"tmp", "new", "cur", "done", "dead"};

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* Sorted names in a directory into a fixed table; returns count. */
static int list_dir(const char *dir, char names[][40], int cap) {
    DIR *d = opendir(dir);
    check(d != NULL, "opendir");
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        check(n < cap, "too many entries");
        snprintf(names[n++], 40, "%s", e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, 40, cmp_str);
    return n;
}

/* Name: <prio digit>-<seq 4 digits>.r<retries> */
static void job_name(char *o, size_t c, int prio, int seq, int retries) {
    snprintf(o, c, "%d-%04d.r%d", prio, seq, retries);
}

static void produce(int seq, int prio) {
    char tmp[64], fin[64], nm[40], body[64];
    job_name(nm, sizeof nm, prio, seq, 0);
    snprintf(tmp, sizeof tmp, "tmp/%s", nm);
    snprintf(fin, sizeof fin, "new/%s", nm);
    int l = snprintf(body, sizeof body, "job %d payload %u", seq, (unsigned)seq * 37u % 101u);
    spit(tmp, body, (size_t)l); /* written completely before it becomes visible */
    check(rename(tmp, fin) == 0, "publish");
}

/* Try to claim the first available job; returns 1 with the claimed name. */
static int claim(char *name_out, const char *snapshot_first) {
    char cand[64], dst[64];
    snprintf(cand, sizeof cand, "new/%s", snapshot_first);
    snprintf(dst, sizeof dst, "cur/%s", snapshot_first);
    if (rename(cand, dst) != 0) {
        check(errno == ENOENT, "claim failure must be ENOENT");
        return 0;
    }
    snprintf(name_out, 40, "%s", snapshot_first);
    return 1;
}

static int parse_retries(const char *name) { return atoi(strrchr(name, 'r') + 1); }

/* A job fails when its sequence number is a multiple of 7 for the first `seq % 4` attempts, and poison jobs
 * (multiples of 11) always fail. */
static int job_succeeds(int seq, int retries) {
    if (seq % 11 == 0)
        return 0;
    if (seq % 7 == 0)
        return retries >= seq % 4;
    return 1;
}

int main(void) {
    for (int i = 0; i < 5; i++)
        check(mkdir(dirs[i], 0755) == 0, "mkdir");
    int total = 30;
    for (int seq = 1; seq <= total; seq++)
        produce(seq, seq % 5 == 0 ? 1 : 5); /* priority 1 sorts before priority 5 */
    char names[64][40];
    int n = list_dir("new", names, 64);
    printf("queued=%d first=%s last=%s\n", n, names[0], names[n - 1]);

    /* Two consumers snapshot the same listing and race for each head job: exactly one wins. */
    int lost_races = 0, processed = 0, failed_attempts = 0, requeued = 0, dead = 0;
    int order[64], nord = 0;
    for (int round = 0; round < 12; round++) {
        n = list_dir("new", names, 64);
        if (n == 0)
            break;
        char mine[40];
        char snap[40];
        snprintf(snap, sizeof snap, "%s", names[0]);
        int a = claim(mine, snap);   /* consumer A */
        char mine2[40];
        int b = claim(mine2, snap);  /* consumer B loses: the file is gone */
        check(a == 1 && b == 0, "exactly one consumer claims a job");
        lost_races++;
        /* consumer A works through claimed job and then keeps taking jobs from a fresh listing */
        int budget = 6;
        char cur_name[40];
        snprintf(cur_name, sizeof cur_name, "%s", mine);
        for (;;) {
            int seq = atoi(cur_name + 2), retries = parse_retries(cur_name);
            char src[64];
            snprintf(src, sizeof src, "cur/%s", cur_name);
            if (job_succeeds(seq, retries)) {
                char dst[64];
                snprintf(dst, sizeof dst, "done/%s", cur_name);
                check(rename(src, dst) == 0, "complete");
                order[nord++] = seq;
                processed++;
            } else {
                failed_attempts++;
                char nn[40], dst[64];
                job_name(nn, sizeof nn, cur_name[0] - '0', seq, retries + 1);
                if (retries + 1 >= 3) {
                    snprintf(dst, sizeof dst, "dead/%s", nn);
                    dead++;
                } else {
                    snprintf(dst, sizeof dst, "new/%s", nn);
                    requeued++;
                }
                check(rename(src, dst) == 0, "fail transition");
            }
            if (--budget == 0)
                break;
            n = list_dir("new", names, 64);
            if (n == 0 || !claim(cur_name, names[0]))
                break;
        }
        /* Crash simulation: consumer dies holding a claim, leaving a stale file in cur/. */
        if (round == 3) {
            n = list_dir("new", names, 64);
            char stuck[40];
            if (n > 0 && claim(stuck, names[0]))
                printf("consumer died holding %s\n", stuck);
        }
        /* Recovery sweep: anything in cur/ at the start of a round is stale and goes back to new/. */
        char cur[16][40];
        int nc = list_dir("cur", cur, 16);
        for (int i = 0; i < nc; i++) {
            char s[64], d[64];
            snprintf(s, sizeof s, "cur/%s", cur[i]);
            snprintf(d, sizeof d, "new/%s", cur[i]);
            check(rename(s, d) == 0, "requeue stale");
            printf("recovered stale claim %s\n", cur[i]);
        }
    }
    /* Drain whatever is left with a plain loop. */
    for (;;) {
        n = list_dir("new", names, 64);
        if (n == 0)
            break;
        char mine[40];
        check(claim(mine, names[0]), "drain claim");
        int seq = atoi(mine + 2), retries = parse_retries(mine);
        char src[64], dst[64];
        snprintf(src, sizeof src, "cur/%s", mine);
        if (job_succeeds(seq, retries)) {
            snprintf(dst, sizeof dst, "done/%s", mine);
            order[nord++] = seq;
            processed++;
        } else {
            char nn[40];
            failed_attempts++;
            job_name(nn, sizeof nn, mine[0] - '0', seq, retries + 1);
            if (retries + 1 >= 3) {
                snprintf(dst, sizeof dst, "dead/%s", nn);
                dead++;
            } else {
                snprintf(dst, sizeof dst, "new/%s", nn);
                requeued++;
            }
        }
        check(rename(src, dst) == 0, "drain transition");
    }
    char dn[64][40], dd[64][40];
    int ndone = list_dir("done", dn, 64), ndead = list_dir("dead", dd, 64);
    check(dead == ndead, "dead counter");
    check(ndone == processed && ndone + ndead == total, "every job ended in done or dead");
    check(list_dir("cur", dn, 64) == 0 && list_dir("tmp", dn, 64) == 0, "no leftovers");
    printf("done=%d dead=%d failed attempts=%d requeued=%d races lost by second consumer=%d\n", ndone, ndead,
           failed_attempts, requeued, lost_races);
    printf("first 10 completions:");
    for (int i = 0; i < 10 && i < nord; i++)
        printf(" %d", order[i]);
    printf("\ndead letters:");
    for (int i = 0; i < ndead; i++)
        printf(" %s", dd[i]);
    printf("\n");
    for (int i = 0; i < 5; i++) {
        char nm[64][40];
        int k = list_dir(dirs[i], nm, 64);
        for (int j = 0; j < k; j++) {
            char p[64];
            snprintf(p, sizeof p, "%s/%s", dirs[i], nm[j]);
            unlink(p);
        }
        rmdir(dirs[i]);
    }
    return 0;
}
