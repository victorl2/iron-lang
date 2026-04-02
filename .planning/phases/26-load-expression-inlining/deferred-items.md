# Deferred Items - Phase 26

## Pre-existing Test Failures (Out of Scope)

These failures existed before phase 26 work began and are unrelated to LOAD expression inlining.

### 1. bug_vla_goto_bypass (integration)

**Status:** Pre-existing FAIL
**Confirmed:** Fails on commit `c333493` (before any phase 26 changes)
**Symptom:** Exit code 133 at runtime when `search(arr, 0, 1)` is called (early return path)
**Root cause (preliminary):** The generated C frees `_v14` (Iron_List) in `return_exit_2_b4` before it was initialized, because the early-return `goto` bypasses the `_v14` initialization in `if_merge_1_b3`.
**Fix scope:** Requires the compiler to hoist the `Iron_List_int64_t _v14` declaration (and zero-initialize it) to the function entry block for VLA-typed values that appear in defer/cleanup exit blocks.

### 2. concurrent_hash_map (algorithms)

**Status:** Pre-existing FAIL (build failed)
**Confirmed:** Fails on commit `c333493` (before any phase 26 changes)
**Symptom:** Build fails when compiling the concurrent_hash_map algorithm test
