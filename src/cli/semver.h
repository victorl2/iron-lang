#ifndef IRON_CLI_SEMVER_H
#define IRON_CLI_SEMVER_H

/* Phase 95 PIN: minimal semver constraint parser + comparator.
 *
 * Supports a single-string constraint with comma-separated AND clauses.
 * Operators: >=, >, <=, <, = (or no operator = exact), ^, ~.
 * Whitespace tolerant around operators and commas.
 *
 * v3.2 simplifications (documented and locked in 95-CONTEXT.md):
 *   (a) Versions compare by semver precedence: a pre-release (`-alpha`)
 *       sorts below the release it precedes (4.2.0-alpha < 4.2.0), so
 *       `= 4.2.0` rejects 4.2.0-alpha and `< 4.2.0` accepts it. Build
 *       metadata (`+build.123`) is ignored.
 *   (b) `^0.X.Y` follows Cargo's 0.x pre-1.0 special case: compatible-within
 *       same minor (e.g. `^0.1.2` matches `0.1.*` but not `0.2.0`).
 *   (c) `*` (wildcard) constraints are explicitly REJECTED as malformed.
 */

#include <stdbool.h>

typedef struct IronSemverConstraint IronSemverConstraint;

/* Parse a single-string semver constraint. Whitespace tolerant.
 * Operators supported: >=, >, <=, <, = (or none = exact), ^, ~.
 * Comma-separated AND clauses: ">= 3.0.0, < 4.0.0".
 * Returns NULL on malformed input. */
IronSemverConstraint *iron_semver_parse(const char *constraint_str);

/* Compare a version string ("X.Y.Z" or "X.Y.Z-suffix") to the constraint
 * using semver precedence.
 * Returns true iff every clause is satisfied. */
bool iron_semver_satisfies(const IronSemverConstraint *c, const char *version_str);

/* Returns the lower bound of the constraint as a static string owned by c.
 * For >=X.Y.Z / =X.Y.Z / ^X.Y.Z / ~X.Y.Z: returns "X.Y.Z".
 * For ranges with a >= clause: returns that clause's bound.
 * For pure </<= constraints with no lower bound: returns NULL. */
const char *iron_semver_suggest_version(const IronSemverConstraint *c);

/* True when version_str fails a lower-bound clause (>=, >, =, ^, ~), i.e.
 * installing the suggested lower bound would help. False when only upper
 * bounds fail (the installed compiler is too new). */
bool iron_semver_below_lower_bound(const IronSemverConstraint *c,
                                   const char *version_str);

void iron_semver_free(IronSemverConstraint *c);

#endif /* IRON_CLI_SEMVER_H */
