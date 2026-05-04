/* test_lsp_log_path_xdg -- Phase 2 Plan 06 Task 01 (CORE-21).
 *
 * Exercises src/lsp/obs/log.{h,c}:
 *   1. test_xdg_state_home_set     -- $XDG_STATE_HOME/iron-lsp/server-<pid>.log
 *   2. test_xdg_fallback_home      -- $HOME/.local/state/iron-lsp/server-<pid>.log
 *   3. test_xdg_last_ditch_tmp     -- both unset -> /tmp/iron-lsp/...
 *   4. test_log_filter_by_level    -- level threshold drops low-prio lines
 *   5. test_log_format_is_json_line-- line contains ts, pid, lvl, event, msg
 *                                     + JSON-escape of " \\ \n \r
 *   6. test_log_thread_safe        -- 4 threads * 100 lines = 400 valid lines
 */
#include "unity.h"
#include "lsp/obs/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {
    ilsp_log_close();
    ilsp_log_set_level(ILSP_LOG_WARN);
}

/* Helper: count '\n'-terminated lines in a file. */
static size_t count_lines_in(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') n++;
    }
    fclose(f);
    return n;
}

/* Helper: read whole file into a malloc'd string. NUL-terminated. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rn = fread(buf, 1, (size_t)sz, f);
    buf[rn] = '\0';
    if (out_len) *out_len = rn;
    fclose(f);
    return buf;
}

/* Helper: returns true iff `needle` appears in the first line of `text`. */
static bool first_line_contains(const char *text, const char *needle) {
    const char *nl = strchr(text, '\n');
    size_t first_len = nl ? (size_t)(nl - text) : strlen(text);
    const char *p = strstr(text, needle);
    return p != NULL && (size_t)(p - text) < first_len;
}

static void rmrf_file(const char *p) { unlink(p); }

/* ── Test 1: $XDG_STATE_HOME respected ──────────────────────────────────── */
static void test_xdg_state_home_set(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/gsd-xdg-test-%d", (int)getpid());
    rmrf_file(dir);
    (void)mkdir(dir, 0755);

    setenv("XDG_STATE_HOME", dir, 1);
    unsetenv("IRONLS_LOG");

    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(NULL));
    const char *path = ilsp_log_path();
    TEST_ASSERT_NOT_NULL(path);

    char expected[512];
    snprintf(expected, sizeof(expected),
             "%s/iron-lsp/server-%d.log", dir, (int)getpid());
    TEST_ASSERT_EQUAL_STRING(expected, path);

    /* Emit one line at WARN (default threshold). */
    ilsp_log(ILSP_LOG_WARN, "xdg-ok", "hello %d", 42);
    ilsp_log_close();

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(expected, &st));
    TEST_ASSERT_TRUE(st.st_size > 0);

    char *contents = read_file(expected, NULL);
    TEST_ASSERT_NOT_NULL(contents);
    TEST_ASSERT_TRUE(first_line_contains(contents, "\"event\":\"xdg-ok\""));
    TEST_ASSERT_TRUE(first_line_contains(contents, "\"msg\":\"hello 42\""));
    free(contents);

    unlink(expected);
    unsetenv("XDG_STATE_HOME");
}

/* ── Test 2: $HOME/.local/state fallback ────────────────────────────────── */
static void test_xdg_fallback_home(void) {
    char home[256];
    snprintf(home, sizeof(home), "/tmp/gsd-home-test-%d", (int)getpid());
    rmrf_file(home);
    (void)mkdir(home, 0755);

    unsetenv("XDG_STATE_HOME");
    setenv("HOME", home, 1);

    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(NULL));
    const char *path = ilsp_log_path();
    TEST_ASSERT_NOT_NULL(path);

    char expected[512];
    snprintf(expected, sizeof(expected),
             "%s/.local/state/iron-lsp/server-%d.log", home, (int)getpid());
    TEST_ASSERT_EQUAL_STRING(expected, path);

    ilsp_log_close();
    unlink(expected);
    unsetenv("HOME");
}

/* ── Test 3: /tmp/iron-lsp last-ditch fallback ──────────────────────────── */
static void test_xdg_last_ditch_tmp(void) {
    unsetenv("XDG_STATE_HOME");
    unsetenv("HOME");

    /* Silence the stderr warning by redirecting temporarily; not
     * strictly necessary for the assertion but keeps CT output clean. */
    fflush(stderr);
    int saved = dup(fileno(stderr));
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) dup2(fileno(devnull), fileno(stderr));

    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(NULL));
    const char *path = ilsp_log_path();

    if (devnull) { fflush(stderr); dup2(saved, fileno(stderr)); fclose(devnull); }
    close(saved);

    TEST_ASSERT_NOT_NULL(path);
    char expected[256];
    snprintf(expected, sizeof(expected),
             "/tmp/iron-lsp/server-%d.log", (int)getpid());
    TEST_ASSERT_EQUAL_STRING(expected, path);

    ilsp_log_close();
    unlink(expected);
    /* Restore HOME so later tests in the binary don't trip on its
     * absence if executed out-of-order. */
    setenv("HOME", "/tmp", 1);
}

/* ── Test 4: level threshold filters low-prio lines ─────────────────────── */
static void test_log_filter_by_level(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/gsd-log-level-%d", (int)getpid());
    rmrf_file(dir);
    (void)mkdir(dir, 0755);

    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(dir));
    const char *path_live = ilsp_log_path();
    TEST_ASSERT_NOT_NULL(path_live);
    /* Snapshot the path -- ilsp_log_close() zeroes the module-level
     * buffer that ilsp_log_path() aliases. */
    char path[512];
    snprintf(path, sizeof(path), "%s", path_live);

    /* Pin level to ERROR; INFO + DEBUG should be dropped. */
    ilsp_log_set_level(ILSP_LOG_ERROR);
    ilsp_log(ILSP_LOG_INFO,  "filter", "should drop");
    ilsp_log(ILSP_LOG_DEBUG, "filter", "should drop too");
    ilsp_log(ILSP_LOG_ERROR, "filter", "should keep");
    ilsp_log_close();

    TEST_ASSERT_EQUAL_size_t(1, count_lines_in(path));

    /* Reopen + widen to DEBUG; both INFO and ERROR arrive. */
    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(dir));
    ilsp_log_set_level(ILSP_LOG_DEBUG);
    ilsp_log(ILSP_LOG_INFO,  "filter", "now kept");
    ilsp_log(ILSP_LOG_DEBUG, "filter", "kept too");
    ilsp_log_close();

    TEST_ASSERT_EQUAL_size_t(3, count_lines_in(path));

    unlink(path);
}

/* ── Test 5: JSON-line format + escapes ─────────────────────────────────── */
static void test_log_format_is_json_line(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/gsd-log-fmt-%d", (int)getpid());
    rmrf_file(dir);
    (void)mkdir(dir, 0755);

    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(dir));
    ilsp_log_set_level(ILSP_LOG_DEBUG);

    /* Message contains every escapable byte. */
    ilsp_log(ILSP_LOG_INFO, "json-fmt",
             "double-quote:\" backslash:\\ newline:\n carriage:\r");
    ilsp_log_close();

    const char *path_saved = NULL;
    char path_fallback[512];
    snprintf(path_fallback, sizeof(path_fallback),
             "%s/server-%d.log", dir, (int)getpid());
    path_saved = path_fallback;

    char *text = read_file(path_saved, NULL);
    TEST_ASSERT_NOT_NULL(text);

    /* All required keys present. */
    TEST_ASSERT_NOT_NULL(strstr(text, "\"ts\":\""));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"pid\":"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"lvl\":\""));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"event\":\"json-fmt\""));
    TEST_ASSERT_NOT_NULL(strstr(text, "\"msg\":\""));

    /* Escape sequences visible. */
    TEST_ASSERT_NOT_NULL(strstr(text, "\\\""));
    TEST_ASSERT_NOT_NULL(strstr(text, "\\\\"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "\\r"));

    /* Exactly one line -- the raw newline inside the msg was escaped so
     * the line ends with a single '\n' from the log terminator. */
    TEST_ASSERT_EQUAL_size_t(1, count_lines_in(path_saved));

    free(text);
    unlink(path_saved);
}

/* ── Test 6: concurrency -- 4 threads * 100 lines = 400 valid lines ────── */
typedef struct {
    int thread_id;
    int iterations;
} WorkerArg;

static void *worker_main(void *arg) {
    WorkerArg *w = (WorkerArg *)arg;
    for (int i = 0; i < w->iterations; i++) {
        ilsp_log(ILSP_LOG_INFO, "conc",
                 "tid=%d iter=%d payload=qwerty-%d-%d",
                 w->thread_id, i, w->thread_id, i);
    }
    return NULL;
}

static void test_log_thread_safe(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/gsd-log-conc-%d", (int)getpid());
    rmrf_file(dir);
    (void)mkdir(dir, 0755);

    TEST_ASSERT_EQUAL_INT(0, ilsp_log_open(dir));
    ilsp_log_set_level(ILSP_LOG_DEBUG);

    enum { N = 4, PER = 100 };
    pthread_t tids[N];
    WorkerArg args[N];
    for (int i = 0; i < N; i++) {
        args[i].thread_id = i;
        args[i].iterations = PER;
        pthread_create(&tids[i], NULL, worker_main, &args[i]);
    }
    for (int i = 0; i < N; i++) pthread_join(tids[i], NULL);
    ilsp_log_close();

    char path[512];
    snprintf(path, sizeof(path), "%s/server-%d.log", dir, (int)getpid());

    /* 400 lines, each starting with '{' and ending with '}'. */
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(f);
    char line[8192];
    size_t line_count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        TEST_ASSERT_TRUE(len >= 3);
        /* Strip trailing '\n'. */
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        TEST_ASSERT_EQUAL_CHAR('{', line[0]);
        TEST_ASSERT_EQUAL_CHAR('}', line[len - 1]);
        line_count++;
    }
    fclose(f);
    TEST_ASSERT_EQUAL_size_t(N * PER, line_count);

    unlink(path);
}

/* ── Unity main ─────────────────────────────────────────────────────────── */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_xdg_state_home_set);
    RUN_TEST(test_xdg_fallback_home);
    RUN_TEST(test_xdg_last_ditch_tmp);
    RUN_TEST(test_log_filter_by_level);
    RUN_TEST(test_log_format_is_json_line);
    RUN_TEST(test_log_thread_safe);
    return UNITY_END();
}
