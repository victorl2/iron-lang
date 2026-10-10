/*
 * title: Scope guards via for-loop macros
 * topic: memory
 * covers: scope guard macros, one-shot for-loop trick, acquire/release pairing, lock-like and buffer-like scopes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int depth;
static int max_depth;
static int live_buffers;
static char log_buf[512];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void logf_(const char *tag, int v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%s%d ", tag, v);
    check(strlen(log_buf) + strlen(tmp) < sizeof log_buf, "log overflow");
    strcat(log_buf, tmp);
}

/* WITH(acquire, release): runs body exactly once between acquire and release.
 * `continue` inside the body jumps to the increment expression, so release still runs.
 * A `break` directly in the body (or a `return`) skips it: the hazard of this macro style. */
#define WITH(acq, rel) for (int once_ = ((acq), 1); once_; once_ = ((rel), 0))

static void enter(int *d) { (*d)++; if (*d > max_depth) max_depth = *d; logf_("E", *d); }
static void leave(int *d) { logf_("L", *d); (*d)--; }

#define INDENT_SCOPE() WITH(enter(&depth), leave(&depth))

/* Scoped buffer: declares a pointer, frees it after the body. */
#define WITH_BUF(name, n)                                                     \
    for (char *name = malloc(n), *once_##name = (live_buffers++, name);       \
         once_##name; free(name), live_buffers--, once_##name = NULL)

static int find_first_negative(const int *a, int n) {
    int idx = -1;
    INDENT_SCOPE() {
        for (int i = 0; i < n; i++) {
            if (a[i] < 0) {
                idx = i;
                break; /* leaves the for(i) loop only; the guard still runs at the end */
            }
        }
    }
    return idx;
}

static long sum_with_skips(const int *a, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) {
        INDENT_SCOPE() {
            if (a[i] % 3 == 0)
                continue; /* binds to the guard loop, so release still runs */
            s += a[i];
        }
    }
    return s;
}

int main(void) {
    int a[] = {4, 9, -2, 7, 12};
    printf("first negative: %d\n", find_first_negative(a, 5));
    printf("log: %s\n", log_buf);
    check(depth == 0, "depth after break");

    log_buf[0] = 0;
    depth = 0;
    INDENT_SCOPE() {
        INDENT_SCOPE() {
            INDENT_SCOPE() {
                logf_("in", depth);
            }
        }
    }
    printf("nested: %s (max depth %d)\n", log_buf, max_depth);
    check(depth == 0, "depth after nested");

    log_buf[0] = 0;
    long s = sum_with_skips(a, 5);
    printf("sum with skips: %ld, depth after continue: %d\n", s, depth);
    printf("log: %s\n", log_buf);
    check(depth == 0 && s == 4 - 2 + 7, "continue still releases");

    /* the hazard: break directly inside the guard body skips release */
    INDENT_SCOPE() {
        logf_("body", depth);
        break;
    }
    printf("depth leaked by direct break: %d\n", depth);
    check(depth == 1, "break leaks the guard");
    depth = 0;

    WITH_BUF(tmp, 32) {
        strcpy(tmp, "scoped");
        printf("buf=%s live=%d\n", tmp, live_buffers);
    }
    printf("live after scope: %d\n", live_buffers);
    check(live_buffers == 0, "buffer released");

    WITH_BUF(x, 8) {
        WITH_BUF(y, 8) {
            printf("two buffers live=%d\n", live_buffers);
            check(x != y, "distinct buffers");
        }
        printf("inner released, live=%d\n", live_buffers);
    }
    check(live_buffers == 0, "both released");
    return 0;
}
