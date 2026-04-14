/* tests/fuzz/seed_blobs.c -- Phase 68 Plan 01 skeleton.
 *
 * Build-time helper invoked by tests/fuzz/CMakeLists.txt POST_BUILD.
 * Arguments:
 *   argv[1] = path to tests/integration/
 *   argv[2] = path to corpus/ output directory (has parser/, typecheck/,
 *             hir_to_lir/ subdirs pre-created by CMake)
 *
 * Plan 01 responsibility:
 *   - Copy every tests/integration/ file ending in .iron into corpus/parser/.
 *     Binary-safe byte-for-byte copy via fread/fwrite. File name is
 *     preserved so libFuzzer can reference inputs by basename.
 *   - Leave corpus/typecheck/ and corpus/hir_to_lir/ empty (Plan 02
 *     will populate them with binary blobs).
 *
 * Plan 02 will extend this file to pre-flight each .iron file through
 * iron_lex_all + iron_parse and serialize the token array for the
 * post-parser corpora. That's why this file links iron_compiler even
 * though Plan 01 only needs dirent + stdio -- keeps the diff in Plan
 * 02 additive.
 *
 * NOTE: This helper is NOT a fuzz target. No libFuzzer runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "seed_blobs: cannot open %s\n", src); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fprintf(stderr, "seed_blobs: cannot open %s\n", dst); fclose(in); return -1; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "seed_blobs: write failed to %s\n", dst);
            fclose(in); fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: iron_seed_blobs <integration-dir> <corpus-dir>\n");
        return 2;
    }
    const char *integ_dir = argv[1];
    const char *corpus_dir = argv[2];

    char parser_dir[4096];
    snprintf(parser_dir, sizeof parser_dir, "%s/parser", corpus_dir);

    DIR *d = opendir(integ_dir);
    if (!d) {
        fprintf(stderr, "seed_blobs: cannot open %s\n", integ_dir);
        return 1;
    }

    int copied = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcmp(name + len - 5, ".iron") != 0) continue;
        /* Skip the fuzz_crash_*.iron fixtures that Plan 06 will land —
         * they're already minimized inputs, not seed material. This
         * guard is harmless on Plan 01 where no such files exist. */
        if (strncmp(name, "fuzz_crash_", 11) == 0) continue;

        char src[4096], dst[4096];
        snprintf(src, sizeof src, "%s/%s", integ_dir, name);
        snprintf(dst, sizeof dst, "%s/%s", parser_dir, name);
        if (copy_file(src, dst) == 0) copied++;
    }
    closedir(d);

    fprintf(stderr, "seed_blobs: copied %d .iron files into %s\n",
            copied, parser_dir);
    /* Plan 02 will also populate corpus/typecheck/ and corpus/hir_to_lir/
     * with binary blobs derived from the same .iron sources. */
    return 0;
}
