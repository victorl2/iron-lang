#ifndef IRON_CLI_TOML_H
#define IRON_CLI_TOML_H

#include <stdbool.h>
#include "cli/web_config.h"
#include "fmt/options.h"

/* Parsed representation of an iron.toml project file. */
typedef struct {
    /* [package] fields (was [project]) */
    char *name;        /* name = "..." (required) */
    char *version;     /* version = "..." (required) */
    char *entry;       /* entry = "..." (preserved for backward compat, ignored by iron) */
    char *type;        /* type = "bin" or "lib" (default "bin") */
    char *description; /* description = "..." (optional) */

    /* [package].iron — Phase 95 PIN-01: optional Cargo-style semver
     * constraint enforced by project_build.c's check_iron_version. NULL when
     * the field is absent (no constraint = no check). Heap-owned; freed
     * in iron_toml_free. */
    char *iron_constraint;

    /* Legacy [dependencies] table. It is no longer supported (no package
     * manager); the parser only records what it saw so the iron CLI can
     * print a migration message.
     *   legacy_deps_section: a [dependencies] header was present.
     *   legacy_dep_name:     first entry other than `raylib = true`
     *                        (heap-owned, NULL when the table was empty or
     *                        only held `raylib = true`). */
    bool   legacy_deps_section;
    char  *legacy_dep_name;
    /* [web] section (parsed in toml.c section==3 branch, Plan 02) */
    IronWebConfig web;

    /* Phase 5 Plan 05-01 (D-02, FMT-05): [fmt] section options.
     * Populated by iron_toml_parse section == 4 branch (added in Plan
     * 05-01 Task 2). Silently absent in iron.toml -> struct stays at
     * calloc-zeroed zero values, which iron_fmt_options_from_toml
     * interprets as "use defaults for each field whose value is <= 0". */
    IronFmtOptions fmt;

    /* Directory containing the iron.toml file passed to iron_toml_parse.
     * Populated by iron_toml_parse. Never NULL on a successful parse —
     * falls back to "." when path has no directory component (bare filename).
     * Freed by iron_toml_free.
     * Used by web builds to resolve [web].assets paths relative to iron.toml's
     * location rather than the shell's cwd (WEB-ASSET-04). */
    char *toml_dir;
} IronProject;

/* Parse iron.toml at the given path.
 * Returns a heap-allocated IronProject on success, NULL on error.
 * Caller must free with iron_toml_free(). */
IronProject *iron_toml_parse(const char *path);

/* Free an IronProject returned by iron_toml_parse(). */
void iron_toml_free(IronProject *proj);

#endif /* IRON_CLI_TOML_H */
