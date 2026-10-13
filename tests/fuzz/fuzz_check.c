/* tests/fuzz/fuzz_check.c -- libFuzzer target for the front end as the
 * tools run it: `ironc check`, the language server and `ironc fmt`.
 *
 * fuzz_parser drives the lexer and parser alone, fuzz_typecheck and
 * fuzz_hir_to_lir only see token blobs that parse cleanly and analyze them
 * without the stdlib. Editors and `ironc check` analyze every buffer,
 * broken ones included, with the stdlib prelude. Per input this target:
 *
 *   1. analyzes the source as `ironc check` does: stdlib prelude
 *      (iron_stdlib_prepend), strict CLI mode, error-recovery
 *      suppression on;
 *   2. analyzes it as the language server does: the same prelude, LSP
 *      mode (every recovery diagnostic, the partial tree kept and
 *      analyzed);
 *   3. formats it (iron_format_source); when the formatter accepts the
 *      file, formatting the result must give it back unchanged, and the
 *      formatter must accept only files the parser accepts. A violation
 *      aborts, so libFuzzer keeps the input.
 *
 * Inputs are Iron source (the integration corpus is a good seed; see
 * docs/dev/fuzzing.md). The stdlib is found as `ironc` finds it
 * (IRON_LIB_DIR, or the source tree next to the build).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "analyzer/analyzer.h"
#include "analyzer/stdlib_prepend.h"
#include "diagnostics/diagnostics.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "util/arena.h"

#define IRON_FUZZ_CHECK_MAX_INPUT 8192

static char *g_lib_dir;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    g_lib_dir = iron_stdlib_lib_dir();
    if (!g_lib_dir) {
        fprintf(stderr, "fuzz_check: cannot locate the stdlib (set IRON_LIB_DIR)\n");
        abort();
    }
    return 0;
}

static bool has_syntax_error(const Iron_DiagList *d) {
    for (int i = 0; i < d->count; i++)
        if (d->items[i].level == IRON_DIAG_ERROR && d->items[i].code < 200 &&
            d->items[i].code != 175 && d->items[i].code != 176) return true;   /* 175, 176: also resolve */
    return false;
}

/* One analysis of `src` with the stdlib prelude, in `mode`. Sets
 * *syntax_error when the user's file has a parse error. */
static void analyze(const char *src, size_t len, IronAnalysisMode mode,
                    bool *syntax_error) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return;
    memcpy(buf, src, len);
    buf[len] = '\0';
    int prepended = iron_stdlib_prepend(&buf, "fuzz.iron", g_lib_dir);
    Iron_Arena    arena = iron_arena_create(IRON_ARENA_CHUNK_SIZE);
    Iron_DiagList diags = iron_diaglist_create();
    (void)iron_analyze_buffer(buf, strlen(buf), "fuzz.iron", mode, &arena, &diags,
                              NULL, prepended + 1);
    if (syntax_error) *syntax_error = has_syntax_error(&diags);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(buf);
}

typedef struct { const uint8_t *data; size_t size; } CheckJob;
static void check_one(const uint8_t *Data, size_t Size);

static void *check_thread(void *arg) {
    CheckJob *job = (CheckJob *)arg;
    check_one(job->data, job->size);
    return NULL;
}

/* The analysis runs on a thread with a 512 MB stack: under ASan plus the
 * fuzzer's coverage instrumentation a type checker frame is tens of KB, so
 * a hundred nested operators overflow the 8 MB main stack although release
 * builds handle the parser's limits with room to spare. */
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size > IRON_FUZZ_CHECK_MAX_INPUT) return -1;
    CheckJob job = { Data, Size };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)512 * 1024 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, check_thread, &job) != 0) {
        pthread_attr_destroy(&attr);
        check_one(Data, Size);
        return 0;
    }
    pthread_join(t, NULL);
    pthread_attr_destroy(&attr);
    return 0;
}

static void check_one(const uint8_t *Data, size_t Size) {
    char *src = (char *)malloc(Size + 1);
    if (!src) return;
    memcpy(src, Data, Size);
    src[Size] = '\0';
    size_t len = strlen(src);   /* the tools read up to the first NUL */

    bool syntax_error = false;
    analyze(src, len, IRON_ANALYSIS_MODE_CLI, &syntax_error);
    analyze(src, len, IRON_ANALYSIS_MODE_LSP, NULL);

    IronFmtOptions opts = iron_fmt_options_default();
    Iron_Arena    arena = iron_arena_create(IRON_ARENA_CHUNK_SIZE);
    Iron_DiagList diags = iron_diaglist_create();
    IronFmtResult r1 = iron_format_source(src, "fuzz.iron", &opts, &arena, &diags);
    if (r1.ok) {
        Iron_DiagList d2 = iron_diaglist_create();
        IronFmtResult r2 = iron_format_source(r1.formatted, "fuzz.iron", &opts,
                                              &arena, &d2);
        if (!r2.ok || r2.formatted_len != r1.formatted_len ||
            memcmp(r1.formatted, r2.formatted, r1.formatted_len) != 0) {
            fprintf(stderr, "fuzz_check: formatting is not idempotent\n"
                            "--- fmt(x)\n%s\n--- fmt(fmt(x))\n%s\n",
                    r1.formatted, r2.ok ? r2.formatted : "(refused)");
            abort();
        }
        iron_diaglist_free(&d2);
        if (syntax_error) {
            fprintf(stderr, "fuzz_check: the formatter accepted a file with a "
                            "syntax error\n");
            abort();
        }
    } else {
        for (int i = 0; i < diags.count; i++) {
            if (diags.items[i].message &&
                strstr(diags.items[i].message, "internal formatter error")) {
                fprintf(stderr, "fuzz_check: %s\n", diags.items[i].message);
                abort();
            }
        }
    }
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}
