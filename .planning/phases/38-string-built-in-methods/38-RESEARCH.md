# Phase 38: String Built-In Methods - Research

**Researched:** 2026-04-02
**Domain:** Iron runtime C implementation — 19 String method bodies in `iron_string.c`
**Confidence:** HIGH — all findings from direct source inspection with file and line citations

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STR-01 | User can call `s.upper()` to get an uppercase copy | `Iron_string_upper(Iron_String self)` → byte-by-byte `toupper`; returns `iron_string_from_cstr` result |
| STR-02 | User can call `s.lower()` to get a lowercase copy | `Iron_string_lower(Iron_String self)` → byte-by-byte `tolower`; same pattern |
| STR-03 | User can call `s.trim()` to remove leading/trailing whitespace | `Iron_string_trim(Iron_String self)` → advance pointer past spaces, shrink length |
| STR-04 | User can call `s.contains(sub)` to check substring presence | `Iron_string_contains(Iron_String self, Iron_String sub)` → `strstr` + NULL check |
| STR-05 | User can call `s.starts_with(prefix)` | `Iron_string_starts_with` → `strncmp` at offset 0 |
| STR-06 | User can call `s.ends_with(suffix)` | `Iron_string_ends_with` → `strncmp` at `len - suffix_len` |
| STR-07 | User can call `s.split(sep)` to split into `List[String]` | `Iron_string_split(Iron_String self, Iron_String sep) -> Iron_List_Iron_String`; uses `Iron_List_Iron_String_create/push` from already-linked `iron_collections.c` |
| STR-08 | User can call `s.replace(old, new)` to substitute all occurrences | `Iron_string_replace(Iron_String self, Iron_String old, Iron_String new_s)`; loop over `strstr` hits, build result buffer |
| STR-09 | User can call `s.substring(start, end)` to extract a slice | `Iron_string_substring(Iron_String self, int64_t start, int64_t end)` → `iron_string_from_cstr(ptr+start, end-start)` |
| STR-10 | User can call `s.index_of(sub)` → -1 if absent | `Iron_string_index_of(Iron_String self, Iron_String sub)` → `strstr` offset or -1 |
| STR-11 | User can call `s.char_at(i)` → single-char String | `Iron_string_char_at(Iron_String self, int64_t i)` → `iron_string_from_cstr(ptr+i, 1)` |
| STR-12 | User can call `s.to_int()` to parse an integer | `Iron_string_to_int(Iron_String self)` → `strtoll`; returns 0 on parse failure (no crash) |
| STR-13 | User can call `s.to_float()` to parse a float | `Iron_string_to_float(Iron_String self)` → `strtod`; returns 0.0 on parse failure |
| STR-14 | User can call `sep.join(list)` to join List[String] | `Iron_string_join(Iron_String self, Iron_List_Iron_String parts)` → sep is receiver; manual buf build |
| STR-15 | User can call `s.len()` as a method | `Iron_string_len(Iron_String self)` → `iron_string_byte_len(&self)` cast to int64_t |
| STR-16 | User can call `s.repeat(n)` | `Iron_string_repeat(Iron_String self, int64_t n)` → malloc n * len, memcpy n times |
| STR-17 | User can call `s.pad_left(width, char)` | `Iron_string_pad_left(Iron_String self, int64_t width, Iron_String ch)` → prepend pad chars |
| STR-18 | User can call `s.pad_right(width, char)` | `Iron_string_pad_right(Iron_String self, int64_t width, Iron_String ch)` → append pad chars |
| STR-19 | User can call `s.count(sub)` to count occurrences | `Iron_string_count(Iron_String self, Iron_String sub)` → loop `strstr`, advance past each hit |
| ITEST-01 | Integration tests exist for all 19 string methods | `.iron` + `.expected` file pairs under `tests/integration/`; one test file per method or per logical group |
</phase_requirements>

---

## Summary

Phase 38 is a pure C runtime implementation phase — no compiler changes are required. Phase 37 already wired the complete dispatch chain: `string.iron` is unconditionally prepended by `build.c` and `check.c`, `typecheck.c` resolves return types via the `String` method-decl scan, and `hir_to_lir.c` generates calls to `Iron_string_<method>` C function names. All 19 functions need to be added as bodies in `src/runtime/iron_string.c`, which is already in the clang link list (`argv_buf[ai++] = *rt_string_out` at build.c:416/444).

The only non-trivial structural concern is `split()` returning `Iron_List_Iron_String` and `join()` consuming one. Both are handled entirely by the macro-generated functions already instantiated in `iron_collections.c` (`IRON_LIST_IMPL(Iron_String, Iron_String)`), which is also unconditionally linked. No new linker entries, no compiler patches, no new files other than integration tests.

Memory ownership follows the existing Iron convention: return values are created via `iron_string_from_cstr()` (SSO inline for short strings, heap-allocated for long strings). Heap strings returned from methods are not freed by the callee or the caller; this is the accepted "tolerated leak" policy for v0.2.0-alpha. No `iron_string_free` helper is needed or should be introduced in this phase.

**Primary recommendation:** Implement all 19 methods in a single `iron_string.c` addition block, grouped by signature complexity. Write integration tests in parallel. No phasing within the phase is required — all 19 are independent C functions with no inter-dependencies.

---

## Standard Stack

### Core

| Component | Version/Location | Purpose | Why This |
|-----------|-----------------|---------|----------|
| `src/runtime/iron_string.c` | Current file | Host all 19 `Iron_string_*` bodies | Already in argv_buf; no new linker entries needed |
| `Iron_String` / `iron_string_cstr` / `iron_string_byte_len` | `iron_runtime.h` | SSO+heap string accessor API | Must use these; never access union fields directly |
| `iron_string_from_cstr(ptr, len)` | `iron_runtime.h` | Create result strings | Handles SSO/heap split transparently |
| `Iron_List_Iron_String_create/push/len/get` | `iron_collections.c` (already linked) | Build `split()` result; read `join()` input | Already instantiated via `IRON_LIST_IMPL(Iron_String, Iron_String)` |
| `<ctype.h>` `toupper`/`tolower` | libc | `upper()` / `lower()` | ASCII-only per STR-ADV-01 deferral; byte-by-byte is correct for v1 |
| `<string.h>` `strstr`/`strncmp`/`memcpy`/`memmove` | libc | substring search, comparison, buffer ops | Already `#include`d in `iron_string.c` |
| `<stdlib.h>` `strtoll`/`strtod`/`malloc`/`free` | libc | `to_int`/`to_float`, heap buffers | Already `#include`d |

### Supporting

| Component | When to Use |
|-----------|-------------|
| `strtoll(cstr, &end, 10)` | `to_int()`: check `end == cstr + len` to detect bad parse; return 0 if invalid |
| `strtod(cstr, &end)` | `to_float()`: same end-pointer check; return 0.0 if invalid |
| Stack buffer (VLA or fixed 256 bytes) | `upper()`/`lower()` for strings that fit in SSO; use `malloc` only when len > `IRON_STRING_SSO_MAX` (23 bytes) |

---

## Architecture Patterns

### Full Dispatch Chain (already in place from Phase 37)

```
Iron source:  s.upper()         (s: String)
      │
      ▼
  [string.iron — prepended unconditionally]
    func String.upper() -> String {}
    (IRON_NODE_METHOD_DECL, type_name="String", method_name="upper", return=String)
      │
      ▼
  [typecheck.c:654]
    obj_id->resolved_type->kind == IRON_TYPE_STRING
    → type_name_mc = "String"
    → decl scan finds method_name=="upper" → result = IRON_TYPE_STRING  ✓
      │
      ▼
  [hir_to_lir.c:782-785]
    obj_type->kind == IRON_TYPE_STRING
    → type_name = "string"
    → snprintf → "string_upper"
    → mangle_func_name → "Iron_string_upper"  ✓
      │
      ▼
  [emit_c.c] → Iron_string_upper(self)
      │
      ▼
  iron_string.c: Iron_string_upper()  ← IMPLEMENT HERE
```

### split() / join() Type Flow

```
s.split(" ")  →  Iron_string_split(Iron_String self, Iron_String sep)
                 returns Iron_List_Iron_String
                 (emit_type_to_c for [String] → "Iron_List_Iron_String")

", ".join(parts)  →  Iron_string_join(Iron_String self, Iron_List_Iron_String parts)
                     returns Iron_String
                     (self is the separator)
```

### Function Signature Table

All methods receive `self` by value (`Iron_String`, not pointer). This matches how emit_c.c emits `Iron_string_upper(self)` — the caller already loaded the value.

```c
/* iron_string.c additions */

Iron_String Iron_string_upper(Iron_String self);
Iron_String Iron_string_lower(Iron_String self);
Iron_String Iron_string_trim(Iron_String self);
bool        Iron_string_contains(Iron_String self, Iron_String sub);
bool        Iron_string_starts_with(Iron_String self, Iron_String prefix);
bool        Iron_string_ends_with(Iron_String self, Iron_String suffix);
Iron_List_Iron_String Iron_string_split(Iron_String self, Iron_String sep);
Iron_String Iron_string_replace(Iron_String self, Iron_String old_s, Iron_String new_s);
Iron_String Iron_string_substring(Iron_String self, int64_t start, int64_t end);
int64_t     Iron_string_index_of(Iron_String self, Iron_String sub);
Iron_String Iron_string_char_at(Iron_String self, int64_t i);
int64_t     Iron_string_to_int(Iron_String self);
double      Iron_string_to_float(Iron_String self);
Iron_String Iron_string_join(Iron_String self, Iron_List_Iron_String parts);
int64_t     Iron_string_len(Iron_String self);
Iron_String Iron_string_repeat(Iron_String self, int64_t n);
Iron_String Iron_string_pad_left(Iron_String self, int64_t width, Iron_String ch);
Iron_String Iron_string_pad_right(Iron_String self, int64_t width, Iron_String ch);
int64_t     Iron_string_count(Iron_String self, Iron_String sub);
```

### Accessor Idiom (MUST use — never access union directly)

```c
// Source: iron_runtime.h + iron_string.c existing bodies
const char *p   = iron_string_cstr(&self);       // NULL-safe; handles SSO and heap
size_t      len = iron_string_byte_len(&self);   // byte length (not codepoints)
Iron_String res = iron_string_from_cstr(buf, n); // create result
```

### split() Implementation Pattern

```c
Iron_List_Iron_String Iron_string_split(Iron_String self, Iron_String sep) {
    Iron_List_Iron_String result = Iron_List_Iron_String_create();
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *d    = iron_string_cstr(&sep);
    size_t      dlen = iron_string_byte_len(&sep);

    if (dlen == 0) {
        /* empty separator: each character is its own element */
        for (size_t i = 0; i < slen; i++) {
            Iron_String ch = iron_string_from_cstr(s + i, 1);
            Iron_List_Iron_String_push(&result, ch);
        }
        return result;
    }

    const char *cur = s;
    const char *end = s + slen;
    while (cur <= end) {
        const char *hit = (cur < end) ? strstr(cur, d) : NULL;
        if (!hit) hit = end;
        Iron_String part = iron_string_from_cstr(cur, (size_t)(hit - cur));
        Iron_List_Iron_String_push(&result, part);
        if (hit == end) break;
        cur = hit + dlen;
    }
    return result;
}
```

### join() Implementation Pattern

```c
Iron_String Iron_string_join(Iron_String self, Iron_List_Iron_String parts) {
    /* self is the separator */
    const char *sep    = iron_string_cstr(&self);
    size_t      seplen = iron_string_byte_len(&self);
    int64_t     n      = Iron_List_Iron_String_len(&parts);
    if (n == 0) return iron_string_from_cstr("", 0);

    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        Iron_String item = Iron_List_Iron_String_get(&parts, i);
        total += iron_string_byte_len(&item);
    }
    total += (size_t)(n > 0 ? n - 1 : 0) * seplen;

    char *buf = (char *)malloc(total + 1);
    if (!buf) return iron_string_from_cstr("", 0);
    char *p = buf;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) { memcpy(p, sep, seplen); p += seplen; }
        Iron_String item = Iron_List_Iron_String_get(&parts, i);
        size_t ilen = iron_string_byte_len(&item);
        memcpy(p, iron_string_cstr(&item), ilen);
        p += ilen;
    }
    *p = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}
```

### to_int() / to_float() Error Handling

```c
int64_t Iron_string_to_int(Iron_String self) {
    const char *s = iron_string_cstr(&self);
    char *end;
    int64_t v = (int64_t)strtoll(s, &end, 10);
    /* If end == s, nothing was consumed; return 0 on bad input (no crash) */
    if (end == s) return 0;
    return v;
}

double Iron_string_to_float(Iron_String self) {
    const char *s = iron_string_cstr(&self);
    char *end;
    double v = strtod(s, &end);
    if (end == s) return 0.0;
    return v;
}
```

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| List[String] container for split() result | Custom struct | `Iron_List_Iron_String_create/push` from `iron_collections.c` | Already instantiated, already linked, already typedef'd in `iron_runtime.h` |
| String equality | `memcmp` inline | `iron_string_equals(&a, &b)` | Handles SSO vs heap correctly |
| String → C pointer | Direct union access | `iron_string_cstr(&s)` | Must not access `.heap.data` or `.sso.data` directly — SSO flag check is required |
| String length | Direct union access | `iron_string_byte_len(&s)` | Same reason — SSO vs heap branch |
| New string creation | Direct struct initialization | `iron_string_from_cstr(buf, len)` | Handles SSO/heap split, sets all fields, null-terminates |

---

## Common Pitfalls

### Pitfall 1: Direct Union Access Instead of Accessors
**What goes wrong:** Writing `self.heap.data` or `self.sso.data` directly. The `is_heap` flag at `self.heap.flags & 0x01` determines which union arm is valid; ignoring it produces garbage for SSO strings.
**How to avoid:** Always use `iron_string_cstr(&s)` and `iron_string_byte_len(&s)`. These are defined in `iron_runtime.h` and handle both cases.
**Warning signs:** Any code accessing `.heap.` or `.sso.` fields outside of `iron_string.c`'s own helper functions.

### Pitfall 2: Passing Iron_String by Pointer When Emit_c Passes by Value
**What goes wrong:** Declaring `Iron_String Iron_string_upper(Iron_String *self)` (pointer) when emit_c generates `Iron_string_upper(self)` (value). This causes the generated C to pass a value where a pointer is expected — silent corruption or crash.
**How to avoid:** All 19 method signatures receive `Iron_String self` by value (matching the emit_c call pattern confirmed in hir_to_lir.c:874).
**Warning signs:** Clang warning about incompatible pointer types at link or compile time.

### Pitfall 3: Iron_string.c Missing Include for List[String]
**What goes wrong:** `Iron_List_Iron_String` and its functions are declared via `IRON_LIST_DECL` in `iron_runtime.h` and defined via `IRON_LIST_IMPL` in `iron_collections.c`. If `iron_string.c` doesn't `#include "iron_runtime.h"` it won't see the typedef or the function prototypes.
**How to avoid:** `iron_string.c` already has `#include "iron_runtime.h"` at line 1 — no change needed.
**Warning signs:** Clang error `unknown type Iron_List_Iron_String`.

### Pitfall 4: split() With Empty Separator Infinite Loop
**What goes wrong:** If `sep` is an empty string, `strstr(cur, "")` always returns `cur` — infinite loop.
**How to avoid:** Add early check `if (dlen == 0)` before the main loop; handle by splitting into individual characters (see pattern above).
**Warning signs:** Test with `"abc".split("")` hanging.

### Pitfall 5: replace() Infinite Loop When old Is Substring of new
**What goes wrong:** Naively calling `strstr` on the current write position after inserting `new_s` that contains `old_s` → infinite replacement loop.
**How to avoid:** Track position in the *source* string (before modification), not in the output buffer. Walk the original source for `strstr` hits, copy segments to output.
**Warning signs:** `"aa".replace("a", "aa")` hanging or producing enormous output.

### Pitfall 6: to_int() With Whitespace or Partial Match
**What goes wrong:** `strtoll("42abc", NULL, 10)` returns 42 and sets `end` to point at 'a'. Depending on intent, this might not be "valid".
**How to avoid:** The success criteria says "handle non-numeric input without crashing" — returning the parsed prefix value (`42`) is acceptable for `"42abc"`. Only return 0 when `end == s` (nothing consumed at all). Do not require full consumption.
**Warning signs:** Test case expecting `"42abc".to_int()` to return 0 when spec allows 42.

### Pitfall 7: substring() Out-of-Bounds Not Clamped
**What goes wrong:** `Iron_string_substring(s, 1, 100)` where `s` has 5 bytes — reading past the buffer.
**How to avoid:** Clamp `start` and `end` to `[0, byte_len]` before computing the slice: `if (start < 0) start = 0; if (end > (int64_t)len) end = (int64_t)len; if (start > end) start = end;`.
**Warning signs:** ASan heap-buffer-overflow on substring tests.

### Pitfall 8: char_at() Out-of-Bounds
**What goes wrong:** Accessing `ptr + i` when `i >= len` or `i < 0`.
**How to avoid:** Clamp `i` to `[0, len-1]`. Return empty string for out-of-range.

---

## Code Examples

### upper() and lower() (byte-by-byte, ASCII)

```c
// Source: ctype.h toupper/tolower; iron_string.c accessor pattern
Iron_String Iron_string_upper(Iron_String self) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return iron_string_from_cstr("", 0);
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)toupper((unsigned char)s[i]);
    buf[len] = '\0';
    Iron_String result = iron_string_from_cstr(buf, len);
    free(buf);
    return result;
}
```

### trim()

```c
// Source: direct inspection of iron_string.c accessor pattern
Iron_String Iron_string_trim(Iron_String self) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    size_t start = 0, end = len;
    while (start < end && (unsigned char)s[start] <= ' ') start++;
    while (end > start && (unsigned char)s[end-1] <= ' ') end--;
    return iron_string_from_cstr(s + start, end - start);
}
```

### contains() / starts_with() / ends_with()

```c
bool Iron_string_contains(Iron_String self, Iron_String sub) {
    const char *s = iron_string_cstr(&self);
    const char *d = iron_string_cstr(&sub);
    return strstr(s, d) != NULL;
}

bool Iron_string_starts_with(Iron_String self, Iron_String prefix) {
    const char *s = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *p = iron_string_cstr(&prefix);
    size_t      plen = iron_string_byte_len(&prefix);
    if (plen > slen) return false;
    return memcmp(s, p, plen) == 0;
}

bool Iron_string_ends_with(Iron_String self, Iron_String suffix) {
    const char *s = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *sfx = iron_string_cstr(&suffix);
    size_t      sfxlen = iron_string_byte_len(&suffix);
    if (sfxlen > slen) return false;
    return memcmp(s + slen - sfxlen, sfx, sfxlen) == 0;
}
```

### repeat() and count()

```c
Iron_String Iron_string_repeat(Iron_String self, int64_t n) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    if (n <= 0 || len == 0) return iron_string_from_cstr("", 0);
    size_t total = len * (size_t)n;
    char *buf = (char *)malloc(total + 1);
    if (!buf) return iron_string_from_cstr("", 0);
    for (int64_t i = 0; i < n; i++) memcpy(buf + (size_t)i * len, s, len);
    buf[total] = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}

int64_t Iron_string_count(Iron_String self, Iron_String sub) {
    const char *s = iron_string_cstr(&self);
    const char *d = iron_string_cstr(&sub);
    size_t dlen = iron_string_byte_len(&sub);
    if (dlen == 0) return 0;
    int64_t cnt = 0;
    const char *p = s;
    while ((p = strstr(p, d)) != NULL) { cnt++; p += dlen; }
    return cnt;
}
```

### Integration Test File Format

Each test is a `.iron` + `.expected` pair in `tests/integration/`. The runner compiles and compares stdout.

```
-- tests/integration/str_upper_lower.iron
func main() {
    val s = "Hello World"
    println(s.upper())
    println(s.lower())
}
```
```
-- tests/integration/str_upper_lower.expected
HELLO WORLD
hello world
```

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Shell-based integration runner (`tests/run_tests.sh integration`) |
| Config file | `tests/integration/CMakeLists.txt` (invokes `run_tests.sh`) |
| Quick run command | `cd /path/to/iron-lang && ./tests/run_tests.sh integration ./build/iron 2>&1 \| grep -E "str_|PASS|FAIL|Results"` |
| Full suite command | `cd /path/to/iron-lang/build && ctest -L integration -V` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| STR-01 | `"hello".upper()` → `"HELLO"` | integration | `run_tests.sh integration` (str_upper_lower test) | Wave 0 |
| STR-02 | `"WORLD".lower()` → `"world"` | integration | same file | Wave 0 |
| STR-03 | `"  hi  ".trim()` → `"hi"` | integration | str_trim test | Wave 0 |
| STR-04 | `s.contains(sub)` → Bool | integration | str_contains test | Wave 0 |
| STR-05 | `s.starts_with(prefix)` | integration | str_starts_ends test | Wave 0 |
| STR-06 | `s.ends_with(suffix)` | integration | str_starts_ends test | Wave 0 |
| STR-07 | `"a b".split(" ")` → `["a","b"]` | integration | str_split_join test | Wave 0 |
| STR-08 | `"abcabc".replace("a","x")` → `"xbcxbc"` | integration | str_replace test | Wave 0 |
| STR-09 | `s.substring(1,3)` | integration | str_substring_indexof test | Wave 0 |
| STR-10 | `s.index_of(sub)` → -1 if absent | integration | str_substring_indexof test | Wave 0 |
| STR-11 | `s.char_at(i)` → 1-char String | integration | str_char_at test | Wave 0 |
| STR-12 | `"42".to_int()` → 42 | integration | str_parse test | Wave 0 |
| STR-13 | `"3.14".to_float()` → 3.14 | integration | str_parse test | Wave 0 |
| STR-14 | `", ".join(list)` | integration | str_split_join test | Wave 0 |
| STR-15 | `s.len()` → Int | integration | str_len_repeat test | Wave 0 |
| STR-16 | `s.repeat(n)` | integration | str_len_repeat test | Wave 0 |
| STR-17 | `s.pad_left(width, ch)` | integration | str_pad test | Wave 0 |
| STR-18 | `s.pad_right(width, ch)` | integration | str_pad test | Wave 0 |
| STR-19 | `s.count(sub)` → Int | integration | str_count test | Wave 0 |
| ITEST-01 | All 19 method tests exist and pass | integration | `run_tests.sh integration` | Wave 0 |

### Sampling Rate
- **Per task commit:** `./tests/run_tests.sh integration ./build/iron 2>&1 | grep -E "str_|Results"`
- **Per wave merge:** `./tests/run_tests.sh integration ./build/iron`
- **Phase gate:** Full suite green (all integration tests pass) before `/gsd:verify-work`

### Wave 0 Gaps

All integration test `.iron` and `.expected` files are missing — they do not exist yet. They must be created as part of Wave 1 alongside the runtime implementation.

Suggested groupings (one `.iron` + `.expected` per file):

- [ ] `tests/integration/str_upper_lower.iron` / `.expected` — covers STR-01, STR-02
- [ ] `tests/integration/str_trim.iron` / `.expected` — covers STR-03
- [ ] `tests/integration/str_contains.iron` / `.expected` — covers STR-04
- [ ] `tests/integration/str_starts_ends.iron` / `.expected` — covers STR-05, STR-06
- [ ] `tests/integration/str_split_join.iron` / `.expected` — covers STR-07, STR-14
- [ ] `tests/integration/str_replace.iron` / `.expected` — covers STR-08
- [ ] `tests/integration/str_substring_indexof.iron` / `.expected` — covers STR-09, STR-10
- [ ] `tests/integration/str_char_at.iron` / `.expected` — covers STR-11
- [ ] `tests/integration/str_parse.iron` / `.expected` — covers STR-12, STR-13
- [ ] `tests/integration/str_len_repeat.iron` / `.expected` — covers STR-15, STR-16
- [ ] `tests/integration/str_pad.iron` / `.expected` — covers STR-17, STR-18
- [ ] `tests/integration/str_count.iron` / `.expected` — covers STR-19

---

## Critical Design Decision: String Memory Ownership

STATE.md records: "String memory ownership policy (heap leak vs iron_string_free cleanup) must be decided before Phase 38 begins."

**Resolution for Phase 38:** Use the "tolerated leak" policy already established by all existing stdlib functions.

Evidence from `iron_io.c` and `iron_string.c`:
- `iron_string_from_cstr(buf, len)` copies the bytes into a new SSO/heap string.
- The caller `free(buf)` after creating the string.
- The returned `Iron_String` (if heap) leaks at program end — no `iron_string_free` exists or is called anywhere.
- This is consistent with v0.2.0-alpha's approach: no GC, no explicit string free, process exit reclaims all.

**Implication for method implementations:**
- Allocate a temporary `char *buf = malloc(n)`, build the result, call `iron_string_from_cstr(buf, n)`, then `free(buf)`.
- The `Iron_String` result either fits in SSO (no heap) or holds a separate heap allocation.
- Methods that return `Iron_String` must NOT retain their `buf` — `iron_string_from_cstr` copies the bytes.

---

## What the Compiler Already Provides (No Changes Needed)

| Component | Status | Evidence |
|-----------|--------|----------|
| `string.iron` prepended unconditionally | DONE | `build.c:763-780`, `check.c` parallel block |
| `typecheck.c` String method return-type dispatch | DONE | `typecheck.c:654-656` (IRON_TYPE_STRING branch → type_name_mc = "String") |
| `hir_to_lir.c` String method → `Iron_string_*` C name | DONE | `hir_to_lir.c:782-785` (type_name = "string") |
| `iron_string.c` in build link list | DONE | `build.c:416,444` |
| `iron_collections.c` in build link list | DONE | `build.c:420,448` |
| `Iron_List_Iron_String` typedef and functions declared | DONE | `iron_runtime.h:564,579` |
| `Iron_List_Iron_String` function bodies instantiated | DONE | `iron_collections.c:39` (`IRON_LIST_IMPL(Iron_String, Iron_String)`) |
| `iron_runtime_init(argc, argv)` signature | DONE | `iron_string.c:206` (Phase 37 Plan 02) |

---

## Open Questions

1. **`len()` byte-count vs codepoint-count**
   - What we know: `iron_string_byte_len` returns byte count; `iron_string_codepoint_count` returns Unicode codepoints. STR-15 says "character count" without specifying.
   - What's unclear: Does `"café".len()` return 4 (codepoints) or 5 (bytes for the é)?
   - Recommendation: Use byte count (`iron_string_byte_len`) for v1; the language definition (line 42) says "unicode" but STR-ADV-01 defers unicode-aware operations to v2. Codepoint count is O(n) for SSO strings; byte count is O(1). The success criteria tests only ASCII examples ("hello" → 5), so either is correct for the test suite.

2. **`substring(start, end)` byte vs codepoint indices**
   - What we know: Same ambiguity as `len()`.
   - Recommendation: Treat as byte indices for v1 (consistent with `char_at`). All test inputs will be ASCII.

3. **`to_int()` return on whitespace-only input like `"  42  "`**
   - What we know: `strtoll("  42  ", &end, 10)` skips leading whitespace and returns 42. This is `strtoll` C standard behavior.
   - Recommendation: Accept this behavior — do not add whitespace-stripping logic. "42" test cases pass; edge cases are not in success criteria.

---

## Sources

### Primary (HIGH confidence)

- `src/runtime/iron_string.c` — full source audit; accessor functions, `iron_string_from_cstr` pattern, memory ownership convention
- `src/runtime/iron_runtime.h` — `Iron_String` struct layout (lines 93-116), `IRON_STRING_SSO_MAX=23`, `IRON_LIST_DECL(Iron_String, Iron_String)` (line 579), `Iron_List_Iron_String` typedef (line 564)
- `src/runtime/iron_collections.c` — `IRON_LIST_IMPL(Iron_String, Iron_String)` (line 39) confirms function bodies exist
- `src/cli/build.c:416,420,444,448` — `rt_string_out` and `rt_collect_out` both in `argv_buf` (always linked)
- `src/cli/build.c:763-780` — `string.iron` unconditionally prepended
- `src/analyzer/typecheck.c:654-656` — `IRON_TYPE_STRING` branch sets `type_name_mc = "String"`
- `src/hir/hir_to_lir.c:782-785` — `IRON_TYPE_STRING` branch sets `type_name = "string"` → `Iron_string_<method>`
- `src/lir/emit_c.c:235-253` — `IRON_TYPE_ARRAY` → `Iron_List_<elem>` C type name
- `src/stdlib/string.iron` — all 19 method declarations with correct signatures
- `src/stdlib/iron_io.c:11-54` — existing pattern for `iron_string_from_cstr` usage and malloc/free lifecycle

### Secondary (MEDIUM confidence)

- `docs/language_definition.md:42,87-165` — String semantics (immutable, unicode, SSO; upper/lower/contains/split examples shown)
- `.planning/STATE.md:65,78` — "String memory ownership policy must be decided before Phase 38 begins" + blocker note

---

## Metadata

**Confidence breakdown:**

| Area | Level | Reason |
|------|-------|--------|
| Standard stack | HIGH | All components from direct source audit; no new dependencies |
| Architecture | HIGH | Phase 37 wired dispatch completely; function signatures derived from emit_c call pattern |
| Implementation patterns | HIGH | `iron_string.c` and `iron_io.c` establish exact conventions; C standard library functions are stable |
| Pitfalls | HIGH | Each pitfall derived from direct source audit or C stdlib gotcha with specific evidence |
| Integration test format | HIGH | `tests/run_tests.sh` and existing `.iron`/`.expected` pairs confirm exact format |

**Research date:** 2026-04-02
**Valid until:** 2026-06-02 (stable — all findings from source code, not external docs)
