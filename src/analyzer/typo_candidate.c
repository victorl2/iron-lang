/* typo_candidate.c — Phase 4 Plan 04-01 (EDIT-07).
 *
 * 2-row DP Levenshtein + scope-walker for the best-matching symbol name.
 * See typo_candidate.h for the contract.
 *
 * Algorithm notes:
 *   - 2-row DP: we only keep the previous row (prev[]) and the current row
 *     (curr[]) rather than the full |a|+1 by |b|+1 matrix. This is the
 *     canonical O(min(|a|,|b|)) space version of Levenshtein.
 *   - Early-exit: after filling each row, the minimum cell value is
 *     compared against max_dist. When every cell in a row already exceeds
 *     max_dist, no cell in any subsequent row can drop below max_dist+1
 *     (the recurrence is monotone-increasing on the diagonal), so we
 *     return max_dist+1 as a sentinel.
 *   - Scope walk: Iron_Scope is parent-linked and each scope holds a
 *     stb_ds hashmap (symbols). We iterate via hmlenu + direct index so
 *     we do not depend on any particular stb_ds iteration macro.
 */

#include "analyzer/typo_candidate.h"
#include "analyzer/scope.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

/* Lowercase a single byte; ASCII-only (identifiers in Iron are ASCII). */
static inline int ch_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

/* min of three ints — avoids a branch heavy triple-min macro. */
static inline int min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

int iron_levenshtein(const char *a, const char *b, int max_dist) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);

    /* Fast path: trivially-over threshold by length difference. */
    if (max_dist >= 0) {
        size_t diff = la > lb ? (la - lb) : (lb - la);
        if ((int)diff > max_dist) return max_dist + 1;
    }

    /* Degenerate cases. */
    if (la == 0) return (int)lb;
    if (lb == 0) return (int)la;

    /* Allocate two rows of size lb+1. Heap always — stack is not worth
     * the branch since the typo walker only ever calls us with identifier-
     * sized inputs (well under a few KB). */
    int *prev = (int *)malloc(sizeof(int) * (lb + 1));
    int *curr = (int *)malloc(sizeof(int) * (lb + 1));
    if (!prev || !curr) {
        free(prev); free(curr);
        /* On alloc failure, return max_dist+1 so the caller treats this
         * input as "not a candidate". Diagnostic pipeline is not the
         * place for an OOM abort. */
        return max_dist >= 0 ? max_dist + 1 : INT_MAX;
    }

    /* Row 0: edit distance from "" to first j chars of b == j. */
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        curr[0] = (int)i;
        int row_min = curr[0];
        int ai = ch_lower((unsigned char)a[i - 1]);
        for (size_t j = 1; j <= lb; j++) {
            int bj = ch_lower((unsigned char)b[j - 1]);
            int cost = (ai == bj) ? 0 : 1;
            int del  = prev[j] + 1;
            int ins  = curr[j - 1] + 1;
            int sub  = prev[j - 1] + cost;
            int v    = min3(del, ins, sub);
            curr[j]  = v;
            if (v < row_min) row_min = v;
        }

        /* Early exit if this row's minimum already exceeds the threshold —
         * every subsequent row's minimum is guaranteed >= row_min (the
         * diagonal is monotone-non-decreasing in the Levenshtein DP). */
        if (max_dist >= 0 && row_min > max_dist) {
            free(prev); free(curr);
            return max_dist + 1;
        }

        /* Swap rows. */
        int *tmp = prev; prev = curr; curr = tmp;
    }

    int result = prev[lb];
    free(prev); free(curr);
    if (max_dist >= 0 && result > max_dist) return max_dist + 1;
    return result;
}

/* ── Scope walker ────────────────────────────────────────────────────────── */

/* Consider a candidate name against the best-so-far, updating both the best
 * name and the best-distance-so-far. Tie-breaks alphabetically via strcmp. */
static void consider(const char *candidate,
                      const char *target,
                      int max_dist,
                      const char **best_name,
                      int *best_dist) {
    if (!candidate) return;
    if (strcmp(candidate, target) == 0) return; /* skip exact match */
    int d = iron_levenshtein(target, candidate, max_dist);
    if (d > max_dist) return;
    if (d < *best_dist) {
        *best_dist = d;
        *best_name = candidate;
        return;
    }
    if (d == *best_dist && *best_name != NULL) {
        /* Tie-break: alphabetically earlier wins. */
        if (strcmp(candidate, *best_name) < 0) {
            *best_name = candidate;
        }
    }
}

const char *iron_best_typo_candidate(struct Iron_Scope *scope,
                                      Iron_Arena        *arena,
                                      const char        *name) {
    if (!name || !arena) return NULL;

    const int max_dist = 2;
    const char *best_name = NULL;
    int best_dist = max_dist + 1;

    /* Walk parent chain. */
    for (Iron_Scope *s = scope; s != NULL; s = s->parent) {
        if (!s->symbols) continue;
        size_t n = hmlenu(s->symbols);
        for (size_t i = 0; i < n; i++) {
            Iron_SymbolEntry *e = &s->symbols[i];
            if (!e->value || !e->value->name) continue;
            consider(e->value->name, name, max_dist, &best_name, &best_dist);
        }
    }

    if (!best_name) return NULL;

    /* arena-strdup so the result survives the scope lifetime. */
    size_t len = strlen(best_name);
    char *copy = iron_arena_strdup(arena, best_name, len);
    return copy; /* NULL on arena OOM, which the caller treats as "no suggestion" */
}
