/* Phase 2 Plan 04 Task 02 (CORE-13) -- Workspace root discovery.
 *
 * Minimal surface for Plan 04:
 *   - Parse a file:// URI into a filesystem path.
 *   - Walk up from that path looking for iron.toml.
 *   - Classify watched-files event paths (source / manifest / lockfile /
 *     unknown) so handlers_document.c can emit the right log event.
 *
 * No TOML parsing yet -- Phase 3 NAV-01 does that. Plan 04 just locates
 * the root so Plan 05+ have a handle. Directory-walk pattern adapted
 * from src/cli/check.c:get_iron_lib_dir. */
#include "lsp/store/workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* URL-decode: copies `src` into `dst`, converting %XX escapes. Returns
 * the number of bytes written. dst must be at least strlen(src) + 1. */
static size_t url_decode(const char *src, char *dst) {
    size_t j = 0;
    for (size_t i = 0; src[i]; i++) {
        if (src[i] == '%' && is_hex(src[i + 1]) && is_hex(src[i + 2])) {
            dst[j++] = (char)((hex_val(src[i + 1]) << 4) | hex_val(src[i + 2]));
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

char *ilsp_workspace_path_from_uri(const char *uri) {
    if (!uri) return NULL;
    const char *prefix = "file://";
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return NULL;
    const char *p = uri + plen;
    /* Skip optional host component (file:///path or file://host/path).
     * We accept both: any host is treated as localhost. */
    /* If there's a leading '/', that's the path root; otherwise the
     * next slash starts the path. */
    if (*p != '/') {
        const char *slash = strchr(p, '/');
        if (!slash) return NULL;
        p = slash;
    }
    size_t raw_len = strlen(p);
    char *out = (char *)malloc(raw_len + 1);
    if (!out) return NULL;
    url_decode(p, out);
    return out;
}

char *ilsp_workspace_find_root(const char *start_path) {
    if (!start_path) return NULL;
    size_t n = strlen(start_path);
    if (n == 0) return NULL;
    char *cur = (char *)malloc(n + 1);
    if (!cur) return NULL;
    memcpy(cur, start_path, n + 1);

    /* If start_path points at a file, start from its parent directory. */
    struct stat st;
    if (stat(cur, &st) == 0 && !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(cur, '/');
        if (slash && slash != cur) {
            *slash = '\0';
        }
    }

    /* Walk up until we find iron.toml or reach '/'. */
    char candidate[4096];
    while (1) {
        int m = snprintf(candidate, sizeof(candidate), "%s/iron.toml", cur);
        if (m > 0 && (size_t)m < sizeof(candidate) &&
            stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
            return cur;   /* caller frees. */
        }

        /* Ascend: strip the trailing component. */
        char *slash = strrchr(cur, '/');
        if (!slash) break;
        if (slash == cur) {
            /* Reached filesystem root "/" -- check once more then break. */
            cur[1] = '\0';
            int m2 = snprintf(candidate, sizeof(candidate), "/iron.toml");
            if (m2 > 0 && stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
                return cur;
            }
            break;
        }
        *slash = '\0';
    }
    free(cur);
    return NULL;
}

static int ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (xl > sl) return 0;
    return memcmp(s + sl - xl, suffix, xl) == 0;
}

IronLsp_WatchedKind ilsp_workspace_classify(const char *uri_or_path) {
    if (!uri_or_path) return ILSP_WATCHED_UNKNOWN;
    if (ends_with(uri_or_path, "/iron.toml") ||
        ends_with(uri_or_path, "iron.toml")) {
        return ILSP_WATCHED_MANIFEST;
    }
    if (ends_with(uri_or_path, "/iron.lock") ||
        ends_with(uri_or_path, "iron.lock")) {
        return ILSP_WATCHED_LOCKFILE;
    }
    if (ends_with(uri_or_path, ".iron")) {
        return ILSP_WATCHED_SOURCE;
    }
    return ILSP_WATCHED_UNKNOWN;
}
