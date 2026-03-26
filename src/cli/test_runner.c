#include "cli/test_runner.h"
#include "cli/build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <spawn.h>

extern char **environ;

/* ── ANSI color helpers (gated by isatty) ────────────────────────────────── */

#define COL_RED    "\033[31m"
#define COL_GREEN  "\033[32m"
#define COL_BLUE   "\033[34m"
#define COL_RESET  "\033[0m"

static int use_color = -1; /* -1 = uninitialised */

static void init_color(void) {
    if (use_color == -1) {
        use_color = isatty(STDOUT_FILENO) ? 1 : 0;
    }
}

static const char *color(const char *code) {
    return use_color ? code : "";
}

/* ── Simple dynamic array of strings ─────────────────────────────────────── */

typedef struct {
    char **items;
    int    count;
    int    capacity;
} StrVec;

static void strvec_push(StrVec *v, char *s) {
    if (v->count == v->capacity) {
        int new_cap = v->capacity == 0 ? 8 : v->capacity * 2;
        char **new_items = (char **)realloc(v->items,
                                            (size_t)new_cap * sizeof(char *));
        if (!new_items) return;
        v->items    = new_items;
        v->capacity = new_cap;
    }
    v->items[v->count++] = s;
}

static void strvec_free(StrVec *v) {
    for (int i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items    = NULL;
    v->count    = 0;
    v->capacity = 0;
}

/* ── Compare function for qsort (alphabetical) ───────────────────────────── */

static int str_compare(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── Execute a binary and return its exit code ───────────────────────────── */

static int run_binary(const char *path) {
    const char *argv[] = { path, NULL };
    pid_t pid;
    int status = posix_spawn(&pid, path, NULL, NULL,
                              (char *const *)argv, environ);
    if (status != 0) {
        return -1;
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    return -1;
}

/* ── iron_test ───────────────────────────────────────────────────────────── */

int iron_test(const char *dir_path) {
    init_color();

    if (!dir_path) dir_path = ".";

    /* 1. Open directory and collect test_*.iron files */
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "iron test: cannot open directory '%s': %s\n",
                dir_path, strerror(errno));
        return 1;
    }

    StrVec test_files = { NULL, 0, 0 };

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        /* Match files starting with "test_" and ending with ".iron" */
        if (strncmp(name, "test_", 5) != 0) continue;
        size_t nlen = strlen(name);
        if (nlen <= 5) continue;
        if (strcmp(name + nlen - 5, ".iron") != 0) continue;

        /* Build full path: dir_path + "/" + name */
        size_t dir_len  = strlen(dir_path);
        size_t name_len = nlen;
        char *full_path = (char *)malloc(dir_len + 1 + name_len + 1);
        if (!full_path) continue;
        memcpy(full_path, dir_path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, name, name_len + 1);

        strvec_push(&test_files, full_path);
    }
    closedir(d);

    if (test_files.count == 0) {
        fprintf(stdout, "iron test: no test_*.iron files found in '%s'\n",
                dir_path);
        strvec_free(&test_files);
        return 0;
    }

    /* 2. Sort alphabetically for deterministic order */
    qsort(test_files.items, (size_t)test_files.count,
          sizeof(char *), str_compare);

    /* 3. Run each test */
    int pass_count = 0;
    int fail_count = 0;

    for (int i = 0; i < test_files.count; i++) {
        const char *file_path = test_files.items[i];

        /* Extract filename for display */
        const char *display_name = strrchr(file_path, '/');
        display_name = display_name ? display_name + 1 : file_path;

        printf("%s[RUN ]%s %s\n",
               color(COL_BLUE), color(COL_RESET), display_name);
        fflush(stdout);

        /* Build a temp binary path */
        char tmp_binary[512];
        snprintf(tmp_binary, sizeof(tmp_binary), "/tmp/iron_test_%d_%d",
                 (int)getpid(), i);

        /* Compile the test file */
        IronBuildOpts opts = {
            .verbose      = false,
            .debug_build  = false,
            .run_after    = false,
            .run_args     = NULL,
            .run_arg_count = 0
        };
        int build_ret = iron_build(file_path, tmp_binary, opts);

        if (build_ret != 0) {
            printf("%s[FAIL]%s %s (compilation error)\n",
                   color(COL_RED), color(COL_RESET), display_name);
            fflush(stdout);
            fail_count++;
            continue;
        }

        /* Run the compiled binary */
        int exit_code = run_binary(tmp_binary);

        /* Remove temp binary */
        unlink(tmp_binary);

        if (exit_code == 0) {
            printf("%s[PASS]%s %s\n",
                   color(COL_GREEN), color(COL_RESET), display_name);
            fflush(stdout);
            pass_count++;
        } else {
            printf("%s[FAIL]%s %s (exit code %d)\n",
                   color(COL_RED), color(COL_RESET), display_name, exit_code);
            fflush(stdout);
            fail_count++;
        }
    }

    /* 4. Print summary */
    int total = pass_count + fail_count;
    printf("\nResults: %d passed, %d failed, %d total\n",
           pass_count, fail_count, total);

    strvec_free(&test_files);
    return (fail_count > 0) ? 1 : 0;
}
