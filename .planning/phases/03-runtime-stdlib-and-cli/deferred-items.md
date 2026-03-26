# Deferred Items

## Out-of-scope pre-existing issues found during plan 03-03

### test_codegen_builtin_len failure

- **Found during:** Task 2 verification (running all tests)
- **Status:** Pre-existing — test was added to tests/test_codegen.c as an uncommitted working tree change (not from this plan)
- **Issue:** `test_codegen_builtin_len` expects the codegen to emit `Iron_len(` when processing `len(s)`. The codegen currently does not emit `Iron_len` for builtin `len` calls.
- **Scope:** This is a future plan task (plan 03-05 or later — builtin len/print wiring in codegen).
- **Action required:** Implement `Iron_len` codegen emission in the appropriate future plan.
- **Test file:** tests/test_codegen.c (line 913)
