/* tests/fuzz/seed_blobs.c -- Phase 68 Plan 02 build-time helper.
 *
 * Invoked by tests/fuzz/CMakeLists.txt POST_BUILD. Arguments:
 *   argv[1] = path to tests/integration/
 *   argv[2] = path to corpus/ output directory (has parser/, typecheck/,
 *             hir_to_lir/ subdirs pre-created by CMake)
 *
 * Responsibilities:
 *   1. Copy every tests/integration/ file ending in .iron into
 *      corpus/parser/ (binary-safe byte-for-byte). Skips any
 *      fuzz_crash_*.iron (those are already-minimized inputs, not
 *      seed material).
 *   2. For every .iron file, pre-flight through iron_lex_all +
 *      iron_parse. If the parse succeeds with zero error diagnostics,
 *      serialize the token array into an IRTB binary blob and write
 *      it to corpus/typecheck/<name>.blob and corpus/hir_to_lir/<name>.blob.
 *      Plans 04-05's fuzz targets mutate these blobs via libFuzzer's
 *      custom mutator (iron_gen_mutate_blob).
 *   3. Print a final tally to stderr.
 *
 * NOTE: This helper is NOT a fuzz target. No libFuzzer runtime.
 *
 * Pitfall 4 (stb_ds diag/token leaks): this tool is short-lived; we let
 * the arena cover the lexer/parser allocations and accept a small
 * per-file leak in the stb_ds diag array, released when iron_diaglist_free
 * runs. Total process RSS over 381 fixtures stays <20 MB in practice.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "iron_gen.h"

/* Cap the per-blob serialized size at 256 KB. Anything larger is almost
 * certainly a pathological fixture; drop it from the post-parser corpora
 * but still keep the .iron file in corpus/parser/. */
#define SEED_BLOB_MAX_BYTES (256u * 1024u)

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

/* Read a whole file into an arena-allocated, null-terminated C string.
 * Returns NULL on any I/O failure. */
static char *read_file_into_arena(Iron_Arena *arena, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);

    char *buf = (char *)iron_arena_alloc(arena, (size_t)len + 1, 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) return NULL;
    buf[len] = '\0';
    return buf;
}

/* Write a blob file to <corpus_dir>/<subdir>/<iron_name>.blob. On any
 * failure, log to stderr and return -1 (caller continues). */
static int write_blob(const char *corpus_dir, const char *subdir,
                       const char *iron_name,
                       const uint8_t *bytes, size_t nbytes) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s/%s.blob",
                      corpus_dir, subdir, iron_name);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "seed_blobs: path too long: %s/%s/%s.blob\n",
                corpus_dir, subdir, iron_name);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "seed_blobs: cannot open %s for write\n", path);
        return -1;
    }
    size_t w = fwrite(bytes, 1, nbytes, f);
    fclose(f);
    if (w != nbytes) {
        fprintf(stderr, "seed_blobs: short write to %s (%zu/%zu)\n",
                path, w, nbytes);
        return -1;
    }
    return 0;
}

/* Pre-flight a single .iron file: lex + parse. If clean, serialize the
 * token array into both post-parser corpora. Returns 1 if a blob was
 * written, 0 if parse failed / output overflowed / any non-fatal issue. */
static int try_seed_blob(const char *integ_path, const char *iron_name,
                          const char *corpus_dir) {
    int result = 0;
    Iron_Arena    arena = iron_arena_create(IRON_ARENA_CHUNK_SIZE);
    Iron_DiagList diags = iron_diaglist_create();

    char *src = read_file_into_arena(&arena, integ_path);
    if (!src) goto cleanup;

    Iron_Lexer lex = iron_lexer_create(src, iron_name, &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lex);
    if (!tokens) goto cleanup;

    int n = 0;
    while (tokens[n].kind != IRON_TOK_EOF) n++;
    n++;  /* include EOF */

    Iron_Parser p = iron_parser_create(tokens, n, src, iron_name,
                                        &arena, &diags);
    Iron_Node *prog = iron_parse(&p);

    /* Parser-success filter: drop any file with an error diagnostic
     * (Pitfall 3 — iron_parse never returns NULL, uses ErrorNode on
     * failures, so we gate on diags.error_count + node kind). */
    if (diags.error_count == 0 && prog && prog->kind == IRON_NODE_PROGRAM) {
        static uint8_t scratch[SEED_BLOB_MAX_BYTES];
        size_t nbytes = iron_gen_blob_encode_from_tokens(
            tokens, n, scratch, sizeof scratch);
        if (nbytes > 0) {
            int ok1 = write_blob(corpus_dir, "typecheck",  iron_name,
                                  scratch, nbytes);
            int ok2 = write_blob(corpus_dir, "hir_to_lir", iron_name,
                                  scratch, nbytes);
            if (ok1 == 0 && ok2 == 0) result = 1;
        }
    }

cleanup:
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: iron_seed_blobs <integration-dir> <corpus-dir>\n");
        return 2;
    }
    const char *integ_dir  = argv[1];
    const char *corpus_dir = argv[2];

    char parser_dir[4096];
    snprintf(parser_dir, sizeof parser_dir, "%s/parser", corpus_dir);

    DIR *d = opendir(integ_dir);
    if (!d) {
        fprintf(stderr, "seed_blobs: cannot open %s\n", integ_dir);
        return 1;
    }

    int parser_copied    = 0;
    int typecheck_copied = 0;
    int hir_copied       = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcmp(name + len - 5, ".iron") != 0) continue;
        /* Skip fuzz_crash_*.iron fixtures (Plan 06 will land these) —
         * they're already-minimized crash inputs, not seed material. */
        if (strncmp(name, "fuzz_crash_", 11) == 0) continue;

        char src[4096], dst[4096];
        snprintf(src, sizeof src, "%s/%s", integ_dir, name);
        snprintf(dst, sizeof dst, "%s/%s", parser_dir, name);

        /* Part 1: byte-copy into corpus/parser/. */
        if (copy_file(src, dst) == 0) parser_copied++;

        /* Part 2: pre-flight + serialize into corpus/typecheck/ and
         * corpus/hir_to_lir/ (parser-success filter). */
        if (try_seed_blob(src, name, corpus_dir)) {
            typecheck_copied++;
            hir_copied++;
        }
    }
    closedir(d);

    fprintf(stderr,
            "seed_blobs: parser=%d typecheck=%d hir_to_lir=%d\n",
            parser_copied, typecheck_copied, hir_copied);
    return 0;
}
