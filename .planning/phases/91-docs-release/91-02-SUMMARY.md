---
phase: 91-docs-release
plan: "02"
subsystem: docs
tags: [migration-guide, site, language-spec, v3, documentation]
dependency_graph:
  requires: [91-01]
  provides:
    - docs/site/migration-v2-to-v3.md
    - docs/site/index.html v3 banner
    - docs/language_definition.md v3 spec
  affects: [release artifacts, user-facing docs, language reference]
tech_stack:
  added: []
  patterns: [step-by-step migration guide, surgical spec updates]
key_files:
  created:
    - docs/site/migration-v2-to-v3.md
  modified:
    - docs/site/index.html
    - docs/site/guide/index.html
    - docs/language_definition.md
decisions:
  - "Migration guide structured as checklist-driven how-to (not reference), targeting v2.2 developers with working codebases"
  - "guide/index.html is the package manager guide with no language syntax; added v3 callout and migrate command section rather than replacing non-existent receiver-method examples"
  - "language_definition.md receiver-method sections fully replaced rather than annotated deprecated -- v3 is a hard break and the spec should reflect that"
  - "Inheritance and interface examples updated to v3 in-block syntax in addition to the methods section"
  - "Full Example in language spec rewritten with pub, init, and in-block methods to serve as canonical v3 reference"
metrics:
  duration: "5m 7s"
  completed: "2026-04-23"
  tasks_completed: 2
  files_changed: 4
---

# Phase 91 Plan 02: Migration Guide, Site Updates, and Language Spec Summary

**One-liner:** v2-to-v3 migration guide (324 lines, 3 transform types, troubleshooting), v3 banner on landing page, and language spec fully updated to in-block methods, mutation tiers, init, patch, and default-private visibility.

## What Was Built

Four documentation artifacts completing the v3.0.0-alpha release surface:

1. **`docs/site/migration-v2-to-v3.md`** (324 lines) -- Step-by-step how-to guide for existing v2.2 users. Covers: install v3, preview codemod diff, apply codemod, verify build. Before/after code examples for all three mechanical transforms (receiver-method to in-block, mut-receiver to default-tier, inline defaults to init). Section on new v3 patterns (mutation tiers, patch, pub) that are not automated. Full troubleshooting section with E-code diagnostics. Summary checklist.

2. **`docs/site/index.html`** -- v3.0.0-alpha breaking-change banner inserted immediately below the nav bar (sticky, accent-colored). Banner links to migration-v2-to-v3.html. Hero code sample updated from v2 `impl` syntax to v3 `implements`, `init`, and `readonly func` in-block pattern. Version badge updated to v3.0.0-alpha. Footer Learn column now includes a migration guide link.

3. **`docs/site/guide/index.html`** -- Package manager guide updated with a v3 compatibility callout at the top and a new "ironc migrate" section covering command syntax, idempotence, and a link to the full guide. Migrate command added to the toolchain use-case table. Sidebar navigation updated with a v3 Migration group.

4. **`docs/language_definition.md`** -- Surgical updates throughout the spec:
   - Functions and Methods section: receiver-method form removed, replaced with in-block method description with `self` and `self.field` requirement
   - Mutation tiers sub-section added: default/readonly/pure with enforcement table and code examples
   - Init section added under Objects: anonymous and named forms, definite assignment, no inline defaults
   - Patch section added: open extension syntax, retroactive interface conformance, rules
   - Visibility section updated: pub default-private model with v2.x reference note
   - Access Control section updated: pub model with concrete before/after
   - Inheritance section: method examples converted to in-block form
   - Interfaces section: Player object converted to in-block methods
   - Enums section: methods-on-enums example converted to in-block form
   - Generics section: Pool example converted to in-block methods
   - Full Example rewritten with pub, init, and in-block methods
   - Keywords Summary updated with patch, init, pub, readonly, pure, self

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Write docs/site/migration-v2-to-v3.md | a0069208 | docs/site/migration-v2-to-v3.md (created, 324 lines) |
| 2 | Update site pages and language spec | 472f785c | index.html, guide/index.html, language_definition.md |

## Verification Results

All automated checks passed:

- `test -f docs/site/migration-v2-to-v3.md` -- PASS
- `wc -l docs/site/migration-v2-to-v3.md >= 100` -- PASS (324 lines)
- `grep -q "ironc migrate" docs/site/migration-v2-to-v3.md` -- PASS
- `grep -q "v3" docs/site/index.html` -- PASS
- `grep -q "migration-v2-to-v3" docs/site/index.html` -- PASS
- `grep -q "object" docs/language_definition.md` -- PASS
- `grep -q "readonly" docs/language_definition.md` -- PASS

## Deviations from Plan

**1. [Rule 2 - Scope] guide/index.html has no receiver-method syntax to replace**

The plan specified finding and replacing receiver-method examples in the package manager guide. That guide covers `iron init`, `iron build`, etc. -- it contains no Iron language code examples, only shell commands and TOML. Instead of fabricating replacement examples, a v3 compatibility callout and a new "ironc migrate" command section were added. This provides user value (explaining how to migrate with the package manager toolchain) without padding unrelated content.

## Phase Close-Out

This summary closes Phase 91. All six requirements are complete:

- DOCS-01: docs/release-notes/v3.md -- COMPLETE (91-01)
- DOCS-02: CHANGELOG.md v3 entry -- COMPLETE (91-01)
- DOCS-03: docs/site/ landing + guide updates -- COMPLETE (91-02)
- DOCS-04: docs/site/migration-v2-to-v3.md -- COMPLETE (91-02)
- DOCS-05: Language spec refresh -- COMPLETE (91-02)
- MIGR-06: Breaking-change messaging -- COMPLETE (91-01, carried in release notes)

## Self-Check: PASSED

- docs/site/migration-v2-to-v3.md: FOUND (324 lines)
- docs/site/index.html: FOUND (v3 banner present)
- docs/site/guide/index.html: FOUND (migrate section present)
- docs/language_definition.md: FOUND (readonly, init, patch, pub present)
- 91-02-SUMMARY.md: FOUND
- Commit a0069208: FOUND
- Commit 472f785c: FOUND
