# tests/migrate_identity

Release-blocker identity fixtures for `scripts/verify-v3-migration.sh`.

- `sources/` holds the v3-grammar Iron source files under test (stdlib modules)
- `expected/` holds the golden C output committed at generation time

Run verification:

    scripts/verify-v3-migration.sh

Regenerate goldens (requires ironc in PATH or ./build/ironc):

    scripts/verify-v3-migration.sh --generate
