/* Phase 97 HELP-04 / HELP-05 / HELP-06: central CLI help registry.
 *
 * See src/cli/help_registry.h for the design rationale. This file holds:
 *   1. The static IRON_CLI_FLAGS table (~28 entries).
 *   2. iron_help_print_subcommand: per-subcommand help text.
 *   3. iron_help_print_all: top-level help with all flags grouped.
 *
 * Plan 97-02 will swap the inline print_usage / print_help blocks in
 * src/cli/main.c and src/pkg/main.c for calls into this file. Plan
 * 97-01 lands only the registry + Unity tests; the CLI dispatchers
 * still print their v3.1 inline text.
 */

#include "cli/help_registry.h"
#include "cli/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Registry ─────────────────────────────────────────────────────────────
 *
 * Subcommand grouping convention:
 *   ""        — global flags (apply to every subcommand): --help, --version, --verbose
 *   "build"   — flags accepted by `iron build` (most build-time flags live here)
 *   "run"     — flags accepted by `iron run`. Most build-time flags are mirrored
 *               from "build" so iron run --help shows the full surface; entries
 *               unique to run (currently just --keep-binary) live here only.
 *   "check"   — `iron check` accepts no subcommand-specific flags in v3.2.
 *   "fmt"     — `iron fmt` accepts no subcommand-specific flags in v3.2.
 *   "test"    — `iron test` accepts no subcommand-specific flags in v3.2.
 *   "init"    — `iron init` (--lib).
 *   "migrate" — ironc migrate (--from, --to).
 *
 * Adding a new flag = one new row below. Ordering inside a subcommand block
 * does not need to be alphabetic in this table; the printers sort on the
 * fly via qsort + strcmp before emitting (CONTEXT.md "Within each subcommand
 * block, flags appear alphabetically (locked).").
 */
const IronCliFlag IRON_CLI_FLAGS[] = {
    /* ── Global ─────────────────────────────────────────────────────────── */
    { "", "--help",    "-h", NULL,  "Print help and exit" },
    { "", "--version", "-v", NULL,  "Print version and exit" },
    { "", "--verbose", NULL, "off", "Verbose output (show generated C, link line, etc.)" },

    /* ── iron build ─────────────────────────────────────────────────────── */
    { "build", "--release",            NULL, "off",    "Optimized release build (native -O2, web -Oz -flto)" },
    { "build", "--debug-build",        NULL, "off",    "Keep .iron-build/ directory after compile for inspection" },
    { "build", "--no-optimize",        NULL, "off",    "Skip optimization passes (for A/B comparison)" },
    { "build", "--target",             NULL, "native", "Build target: native or web (use --target=web)" },
    { "build", "--strict-v3",          NULL, "on",     "Enable v3.0 breaking-change rejections (default ON)" },
    { "build", "--no-strict-v3",       NULL, "off",    "Disable v3.0 breaking-change rejections (debug v2 syntax)" },
    { "build", "--force-comptime",     NULL, "off",    "Skip comptime evaluation cache" },
    { "build", "--dump-ir-passes",     NULL, "off",    "Print IR after each optimization pass" },
    { "build", "--report-compression", NULL, "off",    "Show fields narrowed for value range compression" },
    { "build", "--warn-fusion-break",  NULL, "off",    "Show where fusion chains are broken by non-fusible calls" },
    { "build", "--output",             "-o", NULL,     "Output binary path" },

    /* ── iron run (mirrors most of build's surface; --keep-binary is unique) ── */
    { "run", "--release",            NULL, "off",    "Optimized release build (native -O2, web -Oz -flto)" },
    { "run", "--debug-build",        NULL, "off",    "Keep .iron-build/ directory after compile for inspection" },
    { "run", "--no-optimize",        NULL, "off",    "Skip optimization passes (for A/B comparison)" },
    { "run", "--target",             NULL, "native", "Build target: native or web (use --target=web)" },
    { "run", "--strict-v3",          NULL, "on",     "Enable v3.0 breaking-change rejections (default ON)" },
    { "run", "--no-strict-v3",       NULL, "off",    "Disable v3.0 breaking-change rejections (debug v2 syntax)" },
    { "run", "--force-comptime",     NULL, "off",    "Skip comptime evaluation cache" },
    { "run", "--dump-ir-passes",     NULL, "off",    "Print IR after each optimization pass" },
    { "run", "--report-compression", NULL, "off",    "Show fields narrowed for value range compression" },
    { "run", "--warn-fusion-break",  NULL, "off",    "Show where fusion chains are broken by non-fusible calls" },
    { "run", "--keep-binary",        NULL, "off",    "(reserved, not yet implemented in v3.2) Suppress atexit unlink of run-mode tempfile" },

    /* ── iron init ──────────────────────────────────────────────────────── */
    { "init", "--lib", NULL, "off", "Scaffold a library package (type = \"lib\") instead of a binary" },

    /* ── ironc migrate ──────────────────────────────────────────────────── */
    { "migrate", "--from", NULL, NULL, "Source grammar version (e.g. v2)" },
    { "migrate", "--to",   NULL, NULL, "Target grammar version (e.g. v3)" },
};

const int IRON_CLI_FLAGS_COUNT = (int)(sizeof(IRON_CLI_FLAGS) / sizeof(IRON_CLI_FLAGS[0]));

/* ── Subcommand one-line summaries (used in iron_help_print_all subcommand
 *    list and in iron_help_print_subcommand banner). Hard-coded here
 *    because subcommands are commands, not flags, and don't belong in the
 *    flag registry. ───────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *summary;
} IronSubSummary;

static const IronSubSummary IRON_SUB_SUMMARIES[] = {
    { "build",   "Compile the package" },
    { "run",     "Compile and execute" },
    { "check",   "Type-check without emitting code" },
    { "fmt",     "Format source files" },
    { "test",    "Run package tests" },
    { "init",    "Scaffold a new package" },
    { "migrate", "Run codemod migration" },
};

static const int IRON_SUB_SUMMARIES_COUNT =
    (int)(sizeof(IRON_SUB_SUMMARIES) / sizeof(IRON_SUB_SUMMARIES[0]));

static const char *sub_summary(const char *sub) {
    if (sub == NULL) return "";
    for (int i = 0; i < IRON_SUB_SUMMARIES_COUNT; i++) {
        if (strcmp(IRON_SUB_SUMMARIES[i].name, sub) == 0) {
            return IRON_SUB_SUMMARIES[i].summary;
        }
    }
    return "";
}

/* ── Sorting helpers ─────────────────────────────────────────────────── */

static int cmp_flag_ptr(const void *a, const void *b) {
    const IronCliFlag *fa = *(const IronCliFlag *const *)a;
    const IronCliFlag *fb = *(const IronCliFlag *const *)b;
    return strcmp(fa->flag, fb->flag);
}

/* Collect entries for `sub` into `out` (caller-provided buffer of length
 * `cap`), then sort the slice ASCII-ascending by flag name. Returns the
 * number of entries collected (always <= cap; if the registry ever grows
 * past cap entries per subcommand the excess is silently ignored, but
 * cap=64 is comfortably larger than the current ~13-entry maximum block). */
static int collect_sorted(const char *sub, const IronCliFlag **out, int cap) {
    int n = 0;
    for (int i = 0; i < IRON_CLI_FLAGS_COUNT && n < cap; i++) {
        if (strcmp(IRON_CLI_FLAGS[i].subcommand, sub) == 0) {
            out[n++] = &IRON_CLI_FLAGS[i];
        }
    }
    qsort(out, (size_t)n, sizeof(*out), cmp_flag_ptr);
    return n;
}

/* Emit one flag line at the standard 2-space indent. Format:
 *   --flag-name           description [default: val]
 *   -s                    (alias for --flag-name)
 * Default segment is omitted when default_val is NULL. */
static void emit_flag_line(const IronCliFlag *f, FILE *out) {
    if (f->default_val != NULL) {
        fprintf(out, "  %-22s %s [default: %s]\n",
                f->flag, f->description, f->default_val);
    } else {
        fprintf(out, "  %-22s %s\n", f->flag, f->description);
    }
    if (f->short_flag != NULL) {
        fprintf(out, "  %-22s (alias for %s)\n", f->short_flag, f->flag);
    }
}

/* ── iron_help_print_subcommand ──────────────────────────────────────── */

void iron_help_print_subcommand(const char *sub, FILE *out) {
    if (sub == NULL || sub[0] == '\0') {
        iron_help_print_all(out);
        return;
    }

    const char *summary = sub_summary(sub);
    if (summary[0] != '\0') {
        fprintf(out, "iron %s, %s\n\n", sub, summary);
    } else {
        fprintf(out, "iron %s\n\n", sub);
    }
    fprintf(out, "Usage:\n  iron %s [flags] [args]\n\n", sub);

    const IronCliFlag *sorted[64];
    int n = collect_sorted(sub, sorted, 64);

    if (n == 0) {
        fprintf(out, "Flags:\n  (no subcommand-specific flags)\n");
    } else {
        fprintf(out, "Flags:\n");
        for (int i = 0; i < n; i++) {
            emit_flag_line(sorted[i], out);
        }
    }

    fprintf(out, "\nFor more information, see https://ironlang.org/docs/cli\n");
}

/* ── iron_help_print_all ─────────────────────────────────────────────── */

void iron_help_print_all(FILE *out) {
    fprintf(out, "iron %s, Iron compiler\n\n", IRON_VERSION_STRING);
    fprintf(out, "Usage:\n  iron <subcommand> [flags] [args]\n\n");

    fprintf(out, "Subcommands:\n");
    for (int i = 0; i < IRON_SUB_SUMMARIES_COUNT; i++) {
        fprintf(out, "  %-10s %s\n",
                IRON_SUB_SUMMARIES[i].name, IRON_SUB_SUMMARIES[i].summary);
    }
    fprintf(out, "\n");

    /* Global flags */
    const IronCliFlag *sorted[64];
    int n = collect_sorted("", sorted, 64);
    fprintf(out, "Global flags:\n");
    if (n == 0) {
        fprintf(out, "  (none)\n");
    } else {
        for (int i = 0; i < n; i++) {
            emit_flag_line(sorted[i], out);
        }
    }

    /* Per-subcommand sections in fixed display order. */
    for (int s = 0; s < IRON_SUB_SUMMARIES_COUNT; s++) {
        const char *sub = IRON_SUB_SUMMARIES[s].name;
        n = collect_sorted(sub, sorted, 64);
        fprintf(out, "\niron %s:\n", sub);
        if (n == 0) {
            fprintf(out, "  (no subcommand-specific flags)\n");
        } else {
            for (int i = 0; i < n; i++) {
                emit_flag_line(sorted[i], out);
            }
        }
    }

    fprintf(out, "\nFor more information, see https://ironlang.org/docs/cli\n");
}
