#include "hir/stdlib_origin.h"

#include <stdlib.h>
#include <string.h>

#define IRON_STDLIB_ORIGIN_MAX 64

static char *g_paths[IRON_STDLIB_ORIGIN_MAX];
static int   g_count;
static bool  g_unknown;

void iron_stdlib_origin_reset(void) {
    for (int i = 0; i < g_count; i++) free(g_paths[i]);
    g_count = 0;
    g_unknown = false;
}

void iron_stdlib_origin_add(const char *path) {
    if (!path) return;
    if (g_count >= IRON_STDLIB_ORIGIN_MAX) { g_unknown = true; return; }
    char *copy = strdup(path);
    if (!copy) { g_unknown = true; return; }
    g_paths[g_count++] = copy;
}

void iron_stdlib_origin_mark_unknown(void) {
    g_unknown = true;
}

bool iron_stdlib_origin_is_stub_file(const char *filename) {
    if (g_unknown) return true;
    if (!filename) return false;
    /* `.iron-stub` companions of `type = "lib"` packages are signature-only:
     * their bodies live in the package's compiled archive. */
    size_t n = strlen(filename);
    static const char k_stub_suffix[] = ".iron-stub";
    size_t sn = sizeof(k_stub_suffix) - 1;
    if (n >= sn && strcmp(filename + n - sn, k_stub_suffix) == 0) return true;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_paths[i], filename) == 0) return true;
    return false;
}
