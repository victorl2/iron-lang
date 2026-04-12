---
phase: 10-asset-preload-top-level-loader-guard
plan: 02
subsystem: cli/web-build
tags: [web, emcc, assets, preload, argv, iron_build_web_link, toml]
dependency_graph:
  requires:
    - 10-plan-01-toml-dir-field  # IronProject.toml_dir populated by iron_toml_parse
  provides:
    - --preload-file argv wiring in iron_build_web_link (WEB-ASSET-01/02/04/05)
    - hello_raylib_assets integration fixture
    - Phase 10 CI smoke step in web.yml
  affects:
    - src/cli/build_web.h
    - src/cli/build_web.c
    - src/cli/build.c
    - .github/workflows/web.yml
tech_stack:
  added: []
  patterns:
    - stat + S_ISDIR directory existence check
    - heap-allocated argv entries with tracked cleanup array
    - strrchr-based basename extraction with trailing-slash stripping
key_files:
  created:
    - tests/integration/web/hello_raylib_assets/hello_raylib_assets.iron
    - tests/integration/web/hello_raylib_assets/iron.toml
    - tests/integration/web/hello_raylib_assets/assets/.gitkeep
  modified:
    - src/cli/build_web.h
    - src/cli/build_web.c
    - src/cli/build.c
    - .github/workflows/web.yml
decisions:
  - "Add const char *toml_dir as 4th parameter to iron_build_web_link (not an IronProject* — keeps the function interface minimal and avoids pulling in the full TOML struct)"
  - "Parse iron.toml in build.c step 13 independently of the Phase 8 raylib-detection parse — deferred merge to a future refactor"
  - "Use preload_mappings[16] stack array (not dynamic) — cap at 16 asset roots, consistent with IronWebConfig.asset_count design"
  - "Strip trailing slashes from asset path before strrchr for mount basename — handles 'assets/', 'sounds/sfx/' correctly"
metrics:
  duration_minutes: 15
  completed_date: "2026-04-12"
  tasks_completed: 3
  files_changed: 7
---

# Phase 10 Plan 02: Asset Preload Argv Summary

**One-liner:** Wire `[web].assets` paths into emcc `--preload-file <abs>@/<basename>` argv via `iron_build_web_link`, resolving relative to `proj->toml_dir` from Plan 01.

## What Was Built

### Signature Change: `iron_build_web_link`

**Before (Plan 01 state):**
```c
int iron_build_web_link(const char *c_file_path, IronBuildOpts opts,
                        IronWebConfig *cfg);
```

**After (this plan):**
```c
int iron_build_web_link(const char *c_file_path, IronBuildOpts opts,
                        IronWebConfig *cfg, const char *toml_dir);
```

`toml_dir` is the directory containing `iron.toml`, populated by `iron_toml_parse` (Plan 01). When NULL (bare builds without an `iron.toml`), it is treated as `"."` so existing Phase 7/8/9 smoke still passes.

### Asset Argv Construction

Inside `iron_build_web_link`, after the raylib block and before `argv[n] = NULL`, a new section iterates `cfg->assets[0..asset_count-1]`:

1. For each entry: resolve `<toml_dir>/<rel>` via `snprintf`
2. Call `stat(abs, &st)` + `S_ISDIR(st.st_mode)` check
3. If missing or not a directory: emit `warning: [web].assets directory '<rel>' not found — asset-free build continues\n` to stderr and `continue`
4. Strip trailing slashes from the original `rel` entry, then `strrchr('/')` to extract the mount basename (e.g., `"assets/"` → `"assets"`, `"sounds/sfx/"` → `"sfx"`)
5. Heap-allocate `"<abs>@/<basename>"` mapping string
6. Track in `preload_mappings[preload_count++]`, push `"--preload-file"` + `mapping` into `argv`

`max_argv` raised from 48 to 64 to accommodate up to 8 asset entries (16 slots) on top of the Phase 9 high-water mark of 40.

### Cleanup Refactor: `preload_mappings[]` Free on Every Return Path

A `preload_mappings[16]` / `preload_count` pair tracks all heap-allocated mapping strings. Four cleanup sites were updated — each now includes:
```c
for (int pi = 0; pi < preload_count; pi++) free(preload_mappings[pi]);
```
alongside the existing `free(stdlib_i_flag)` / `free(src_i_flag)` / abs_paths cleanup:

1. Forbidden-flag rejection branch
2. `posix_spawnp` failure branch
3. `waitpid` failure branch
4. Final success cleanup (step 8)

### `build.c` Step 13 Update

Replaced `iron_build_web_link(c_file_path, opts, NULL)` with a block that:
- Parses `<dirname(source_path)>/iron.toml` via `iron_toml_parse`
- Captures `web_cfg = &web_proj->web` and `web_toml_dir = web_proj->toml_dir` on success
- Calls `iron_build_web_link(c_file_path, opts, web_cfg, web_toml_dir)`
- Frees `web_proj` after the link returns

When no `iron.toml` exists (`web_proj == NULL`), both `web_cfg` and `web_toml_dir` are NULL — `iron_build_web_link` handles both safely (NULL cfg = no assets, NULL toml_dir = treated as `"."`).

### Native/Web Parity Guarantee (WEB-ASSET-02)

`LoadTexture("assets/foo.png")` resolves identically on both targets:
- **Native:** the binary's cwd contains the `assets/` directory; the call reads `./assets/foo.png` directly
- **Web:** `--preload-file <abs>/assets@/assets` mounts the directory at `/assets` inside MEMFS; the same string `"assets/foo.png"` resolves to `/assets/foo.png` in the virtual filesystem

### Integration Fixture

`tests/integration/web/hello_raylib_assets/` contains:
- `hello_raylib_assets.iron`: raylib program with `LoadTexture("assets/missing.png")` *inside* `main()` (not at module level — deferred to Plan 03)
- `iron.toml`: `[dependencies] raylib = true` + `[web] assets = "assets/"`
- `assets/.gitkeep`: empty sentinel so `assets/` survives `git add`

The `assets/` directory contains only `.gitkeep`, not a real PNG. The CI smoke asserts the `--preload-file` flag lands in the emcc argv (via `--verbose`), not that a PNG file is found at runtime (that is Phase 12 Pong's job).

### CI Smoke Step (web.yml)

New step "End-to-end smoke — asset preload (Phase 10, WEB-ASSET-01/02/04/05)":
- Runs `ironc build --target=web --verbose` on the fixture, captures stdout+stderr to `/tmp/phase10_build.log`
- Asserts `dist/web/index.{html,js,wasm}` all exist and are non-empty
- Greps log for `--preload-file` (flag was emitted)
- Greps log for `assets@/assets` (correct mount mapping)
- Asserts no `warning: [web].assets` appears (directory exists via `.gitkeep`)

Paths filters updated in both `push:` and `pull_request:` sections to add `tests/integration/web/hello_raylib_assets/**`.

## Deviations from Plan

None — plan executed exactly as written.

## Self-Check: PASSED

All required files exist on disk. All task commits verified:
- `34becee` feat(10-02): wire [web].assets --preload-file argv into iron_build_web_link
- `4deb2ca` feat(10-02): add hello_raylib_assets integration fixture
- `816fb4a` ci(10-02): add asset preload smoke step to web.yml
