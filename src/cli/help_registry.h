#ifndef IRON_CLI_HELP_REGISTRY_H
#define IRON_CLI_HELP_REGISTRY_H

/* Phase 97 HELP-04 / HELP-05 / HELP-06: central CLI flag/help registry.
 *
 * Single source of truth for every flag the iron and ironc CLIs accept.
 * Plan 97-02 will rewrite the inline print_usage() / print_help() text in
 * src/cli/main.c and src/pkg/main.c to call the printer functions declared
 * here, so adding a new flag becomes a one-line edit in
 * src/cli/help_registry.c instead of a triple-touch across two
 * dispatcher source files plus a hand-maintained banner.
 *
 * The registry is a static C array of IronCliFlag structs, one entry per
 * flag, grouped by subcommand. Linear iteration is fine at this scale
 * (~25 entries); no hash table, no bsearch.
 *
 * Both iron (src/pkg/main.c) and ironc (src/cli/main.c) link the
 * companion help_registry.c so either dispatcher can call
 * iron_help_print_subcommand / iron_help_print_all without further
 * wiring.
 */

#include <stdio.h>

typedef struct {
    /* "build" / "run" / "check" / "fmt" / "test" / "init" / "migrate",
     * or "" for global flags that apply regardless of subcommand. */
    const char *subcommand;
    /* "--release", "--target", etc. Always non-NULL. */
    const char *flag;
    /* "-o" or NULL. May be NULL. */
    const char *short_flag;
    /* "off" / "native" / etc. NULL for path/string flags whose default
     * value is meaningless ("--from", "--output", ...). */
    const char *default_val;
    /* Single-line description. Always non-NULL. */
    const char *description;
} IronCliFlag;

extern const IronCliFlag IRON_CLI_FLAGS[];
extern const int IRON_CLI_FLAGS_COUNT;

/* Print help for one subcommand to `out`. Pass "" to mean top-level help
 * (forwards to iron_help_print_all). Recognized subcommand strings are
 * those listed in the IronCliFlag.subcommand field comment above.
 *
 * `prog` is the binary name that should appear in the banner and usage
 * line ("iron" when called from src/pkg/main.c, "ironc" when called from
 * src/cli/main.c). NULL falls back to "iron". */
void iron_help_print_subcommand(const char *prog, const char *sub, FILE *out);

/* Print top-level help (banner + subcommand list + every flag grouped by
 * subcommand) to `out`. Within each subcommand block, flags emit in
 * ASCII-alphabetical order, this is locked by tests/unit/test_help_registry.c.
 *
 * `prog` is the binary name that should appear in the banner and usage
 * line. NULL falls back to "iron". */
void iron_help_print_all(const char *prog, FILE *out);

#endif /* IRON_CLI_HELP_REGISTRY_H */
