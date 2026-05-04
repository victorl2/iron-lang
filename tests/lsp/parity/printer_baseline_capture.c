/* Phase 9 Plan 09-02 Task 1 — Wave 0 baseline capture.
 *
 * Walks tests/integration .iron fixtures, calls iron_format_source on every
 * fixture whose basename does NOT start with "v3_", and writes the formatted
 * bytes to tests/lsp/fixtures/printer_v2_baseline/<basename>.printed.
 *
 * Captured BEFORE any printer change so that Tasks 2 + 3 can byte-diff the
 * post-edit printer output against this snapshot to enforce the Pitfall 3
 * zero-v2-regression invariant locked by RESEARCH §3.
 *
 * Fixtures whose source has lexer or parser errors (ok=false) write an empty
 * .printed file — the post-edit run must produce the same empty result for
 * those fixtures, which still proves zero regression for the refusal path. */

#include "diagnostics/diagnostics.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "util/arena.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static int write_all(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int    e = fflush(f);
    fclose(f);
    return (w == len && e == 0) ? 0 : -1;
}

static int starts_with_v3(const char *name) {
    return name[0] == 'v' && name[1] == '3' && name[2] == '_';
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <fixtures-dir> <baseline-out-dir>\n", argv[0]);
        return 2;
    }
    const char *fixtures_dir = argv[1];
    const char *out_dir      = argv[2];

    DIR *d = opendir(fixtures_dir);
    if (!d) { perror(fixtures_dir); return 2; }

    int written = 0;
    int skipped_v3 = 0;
    int refused = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5) continue;
        if (strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;
        if (starts_with_v3(ent->d_name)) { skipped_v3++; continue; }

        char in_path[4096];
        snprintf(in_path, sizeof(in_path), "%s/%s", fixtures_dir, ent->d_name);

        struct stat st;
        if (stat(in_path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        size_t src_len = 0;
        char *src = slurp(in_path, &src_len);
        if (!src) continue;

        Iron_Arena    arena = iron_arena_create(64 * 1024);
        Iron_DiagList diags = iron_diaglist_create();
        IronFmtOptions opts = iron_fmt_options_default();
        IronFmtResult  r = iron_format_source(src, in_path, &opts, &arena, &diags);

        /* Compose output path: <out_dir>/<basename>.printed */
        char out_path[4096];
        snprintf(out_path, sizeof(out_path), "%s/%.*s.printed",
                 out_dir, (int)(nlen - 5), ent->d_name);

        int rc;
        if (!r.ok) {
            /* Refusal path: write empty file to record that this fixture
             * was refused at baseline time. Post-edit refusal must match. */
            rc = write_all(out_path, "", 0);
            refused++;
        } else {
            rc = write_all(out_path, r.formatted, r.formatted_len);
        }
        if (rc != 0) {
            fprintf(stderr, "WRITE-FAIL %s\n", out_path);
        } else {
            written++;
        }

        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(src);
    }
    closedir(d);

    fprintf(stdout, "baseline-capture: written=%d (refused=%d) skipped_v3=%d\n",
            written, refused, skipped_v3);
    return 0;
}
