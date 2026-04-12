---
gsd_state_version: 1.0
milestone: v0.0
milestone_name: milestone
current_plan: —
status: planning
stopped_at: Completed 65-04-PLAN.md
last_updated: "2026-04-12T15:02:44.291Z"
last_activity: 2026-04-12 — Roadmap created with 6 phases (65-70), 35 requirements mapped
progress:
  total_phases: 32
  completed_phases: 10
  total_plans: 43
  completed_plans: 39
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-11)

**Core value:** Iron's compiler and runtime are correct, cross-platform, and protected from repeat regressions — measured by eliminated UB classes, restored CI coverage, and a ratcheting test/coverage baseline.
**Current focus:** v0.1.4-alpha Compiler Correctness & Maintenance — roadmap created, ready to plan Phase 65

## Paused Milestone

v0.2.0-alpha Networking Standard Library is paused at 25/89 requirements shipped. Phase 59 (`.planning/phases/59-net-foundation-windows-ci-url/`) delivered INFRA-04..10 partial, NET-01..13, URL-01..07, cut as public release v1.2.0-alpha via PR #17. Phases 60–64 (HTTP/TLS/JSON/WebSocket) remain drafted in ROADMAP.md and resume after v0.1.4-alpha ships. The 64 paused requirements are archived at `.planning/REQUIREMENTS-v0.2.0.md`.

## Current Position

Milestone: v0.1.4-alpha Compiler Correctness & Maintenance
Phase: 65 of 70 (Correctness Audit) — not started
Current Plan: —
Status: Ready to plan Phase 65
Last activity: 2026-04-12 — Roadmap created with 6 phases (65-70), 35 requirements mapped

Progress: [░░░░░░░░░░] 0%

## Performance Metrics

**Velocity:**
- Total plans completed: 21
- Average duration: 20min
- Total execution time: ~6.0 hours

| Phase | Plan | Duration | Tasks | Files |
|-------|------|----------|-------|-------|
| 47-collection-methods | 01 | 7min | 2 | 4 |
| 47-collection-methods | 02 | 20min | 2 | 15 |
| 47-collection-methods | 03 | 49min | 1 | 19 |
| 48-layout-optimizations | 01 | 24min | 2 | 6 |
| 48-layout-optimizations | 02 | 21min | 2 | 7 |
| 48-layout-optimizations | 03 | 28min | 2 | 11 |
| 48-layout-optimizations | 04 | 7min | 1 | 5 |
| 49-loop-fusion | 01 | 21min | 3 | 12 |
| 49-loop-fusion | 02 | 25min | 2 | 17 |
| 49-loop-fusion | 03 | 26min | 2 | 7 |
| 50-vrc-arena | 01 | 14min | 2 | 9 |
| 50-vrc-arena | 02 | 11min | 2 | 3 |
| 50-vrc-arena | 03 | 8min | 2 | 4 |
| 52-emitter-refactoring | 01 | 13min | 2 | 4 |
| 52-emitter-refactoring | 02 | 36min | 2 | 4 |
| 52-emitter-refactoring | 03 | 37min | 3 | 7 |
| 53-analysis-improvements | 01 | 39min | 2 | 6 |
| Phase 53 P02 | 52min | 2 tasks | 6 files |
| 53-analysis-improvements | 03 | 11min | 2 | 4 |
| Phase 54-test-hardening P01 | 5min | 2 tasks | 10 files |
| 54-test-hardening | 02 | 9min | 3 | 94 |
| 54-test-hardening | 03 | 6min | 2 | 10 |
| Phase 55-push-on-interface-arrays P01 | 27min | 3 tasks | 13 files |
| Phase 55-push-on-interface-arrays P02 | 10min | 3 tasks | 7 files |
| Phase 55-push-on-interface-arrays P03 | 18min | 3 tasks | 7 files |
| Phase 55.1-empty-typed-array-literal P01 | 23min | 3 tasks | 11 files |
| Phase 59-net-foundation-windows-ci-url P01a | 20min | 1 tasks | 5 files |
| Phase 59-net-foundation-windows-ci-url P01b | 6min | 1 tasks | 4 files |
| Phase 59-net-foundation-windows-ci-url P01c | 25min | 1 tasks | 12 files |
| Phase 59-net-foundation-windows-ci-url P01d | 57min | 4 tasks | 21 files |
| Phase 59-net-foundation-windows-ci-url P02 | 45min | 1 tasks | 14 files |
| Phase 59-net-foundation-windows-ci-url P03 | 32 min | 1 tasks | 9 files |
| Phase 59-net-foundation-windows-ci-url P05 | 90min | 1 tasks | 5 files |
| Phase 59-net-foundation-windows-ci-url P04 | 38min | 1 tasks | 10 files |
| Phase 59-net-foundation-windows-ci-url P06 | 21min | 1 tasks | 7 files |
| Phase 65-correctness-audit P01 | 7min | 2 tasks | 1 files |
| Phase 65-correctness-audit P02 | 12min | 2 tasks | 1 files |
| Phase 65-correctness-audit PP03 | 7min | 2 tasks | 1 files |
| Phase 65-correctness-audit P04 | 7min | 2 tasks | 1 files |

## Accumulated Context

### Decisions

- [v0.1-alpha]: Core dispatch with tagged unions -- shipped
- [v0.1-alpha]: Collection splitting with per-type sub-arrays -- shipped
- [v0.1.1-alpha]: Method syntax for collection operations
- [v0.1.1-alpha]: Full closure capture before collection methods
- [Phase 49]: Monomorphic collapse to standard Iron_List path (not plain typed array)
- [Phase 49]: Conservative monomorphic detection: only ARRAY_LIT-local collections
- [Phase 50]: Conservative TOP for function call return values (no interprocedural call-site analysis)
- [Phase 50]: Inline pointer registry in SplitList instead of embedding full Iron_Arena
- [Phase 51]: Memory investigation resolved -- no critical leak found
- [Phase 52]: emit_ prefix convention for all shared emitter functions; emit_ctx_cleanup for consolidated resource freeing
- [Phase 52]: Type declaration emission isolated in emit_structs module; estimate_type_size renamed to emit_estimate_type_size
- [Phase 52]: Split collection emission isolated in emit_split module; fusion emission isolated in emit_fusion module
- [Phase 52]: emit_expr_to_buf made non-static for cross-module access from emit_fusion.c
- [Phase 53]: func_ref resolution via value_table for CALL-site return range propagation
- [Phase 53]: Block entry range replace semantics (not intersect) for correct cross-path analysis
- [Phase 53]: Conditional narrowing shared in both collect_return_ranges and analyze_function_ranges
- [Phase 53]: emit_type_to_c uses Iron_SplitList_ for interface arrays (fixes return type mismatch)
- [Phase 53]: Specialization heuristic: <=50 instrs, 1-2 callers, dispatch branches required
- [Phase 53]: Phase A.1 wires CALL results to coll_types via func_return_types for interprocedural type propagation
- [Phase 54]: Monomorphic collapse still emits SplitList infrastructure; verification checks single-type push not SplitList absence
- [Phase 54]: Single-implementor tests use for-loop (monomorphic + .map() chain is pre-existing compiler limitation)
- [Phase 54]: Large collection stress test uses typed Int array push loop (interface arrays don't support runtime push)
- [Phase 54]: Benchmark speed thresholds 1.5x->2.5x across 88 configs to tolerate CI runner variance
- [Phase 54]: SoA+fusion composition uses for-loop path (ordered iteration on SoA has Stor type mismatch bug)
- [Phase 54]: Mono+computation uses for-loop (mono + .map() chain is known compiler limitation)
- [Phase 54]: Mega test exercises split+SoA+dead field+compression+arena via for-loop; fusion deferred
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 01: Inline _push branch in emit_c.c split-collection interception block, ships both concrete (Mode a) and interface-typed (Mode b) dispatch
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 01: Mode b tag-switch uses impl->tag (not loop index) and honors ctx->indirect_variants for pointer-stored large payloads
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 01 tests: use 2-element initial literals with both implementors to avoid monomorphic-collapse path (scoped to Phase 56)
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 02: _len uses direct _total_count field read (not per-type sum) — authoritative combined-length field, mirrors .count accessor at emit_c.c:1054
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 02: _pop reads _order[_total_count-1] for (tag, idx), dispatches via switch over impl->tag, boxes via Iron_<Iface>_from_<Type>, decrements per-type count inside case and _order_count/_total_count after switch
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 02: SoA-implementor pop emits defensive zero-init fallback per 55-CONTEXT.md scoping; AoS is fully implemented, SoA pop is documented known limitation
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 03: _get mirrors _pop branch structure except no count/order decrement — pure read with tag switch over _order[i] and Iron_<Iface>_from_<Type> wrapping
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 03: _set uses same-type in-place overwrite with runtime tag check guard; different-type and interface-typed writes are documented silent no-op (known limitation)
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 03: Plans 01-03 together deliver full PUSH-01 broad scope (push + len + pop + get + set) for interface split collections; interception block now handles every stdlib array method
- [Phase 55.1-empty-typed-array-literal]: check_expr_with_expected helper threads expected types into empty array literal inference at 4 expression contexts; check_expr signature unchanged
- [Phase 55.1-empty-typed-array-literal]: Error code IRON_ERR_EMPTY_LITERAL_NO_TYPE=229 used (not 220 as in CONTEXT placeholder; 220 was already IRON_ERR_NO_SUCH_METHOD)
- [Phase 55.1-empty-typed-array-literal]: Call-arg test uses [Int] not [Shape] because passing interface arrays as function parameters hits a pre-existing codegen gap (deferred to future IFACE-PARAM-01 requirement)
- [v0.2.0-alpha roadmap]: 6-phase bottom-up structure per research SUMMARY.md -- Net foundation first, TLS second, JSON third, HTTP client/server split, WebSocket last
- [v0.2.0-alpha roadmap]: Security-critical requirements co-located with the feature they guard -- hostname verify with TLS, auth stripping with HTTP client, smuggling rejection with parser, Slowloris defense with server, JSON depth limits with JSON phase
- [v0.2.0-alpha roadmap]: Windows CI (INFRA-05) lands in Phase 59 -- cross-platform regressions cannot remain invisible for 5 phases
- [v0.2.0-alpha roadmap]: URL module slotted into Phase 59 (pure Iron, unblocks HTTP parsing, no reason to delay)
- [v0.2.0-alpha roadmap]: INFRA-10 (Iron_Error normalization) starts in Phase 59 and extends in later phases as each subsystem adds codes
- [v0.2.0-alpha roadmap]: 89 requirements mapped across 6 phases -- REQUIREMENTS.md header said "87 total" but actual count is 89 (footer updated)
- [Phase 59-net-foundation-windows-ci-url]: Iron_Deadline uses uint64_t with 0 sentinel for expired/poll-once; saves a bool field and lets remaining_ms/expired short-circuit without a clock read
- [Phase 59-net-foundation-windows-ci-url]: pthread_cond_timedwait uses CLOCK_REALTIME for Phase 59 with documented NTP-slew caveat; TLS phase can upgrade via pthread_condattr_setclock(CLOCK_MONOTONIC)
- [Phase 59-net-foundation-windows-ci-url]: iron_cond_timedwait_ms returns tri-state int (OK/EXPIRED/ERROR) instead of Iron_Error to avoid 16-byte return ABI on the hot path
- [Phase 59-net-foundation-windows-ci-url]: iron_errors.h included from iron_runtime.h so every downstream TU gets IRON_ERR_* codes transitively without new includes; canonical location per INFRA-10
- [Phase 59-net-foundation-windows-ci-url]: Iron_PoolWait uses abandoned-flag variant (not refcount): caller owns the wait struct end-to-end; worker_finish in abandoned case destroys result and returns without freeing w
- [Phase 59-net-foundation-windows-ci-url]: Iron_poolwait_wait_ms shipped as a first-class P01b export (header + impl) to resolve checker blocker #2 — P04 DNS consumes it directly without cross-plan edits
- [Phase 59-net-foundation-windows-ci-url]: Iron_pool_mark_one_leaked decrements pending AND thread_count AND broadcasts work_done when pending hits zero — prevents ghost-count barrier hangs (Pitfall 13); adds broadcast beyond the plan's sketch
- [Phase 59-net-foundation-windows-ci-url]: Elastic pool destroy snapshots live slots under the lock and joins the snapshot; retired workers have already NULLed their slot and pthread_detached themselves so destroy skips them cleanly
- [Phase 59-net-foundation-windows-ci-url]: Iron_Pool read accessors (is_elastic/max_threads/live_thread_count/leaked_count) exported so tests and callers never touch internal struct layout — struct remains opaque behind typedef in the header
- [Phase 59-net-foundation-windows-ci-url P01c]: Iron_net_wsa_startup_once is refcounted under an internal mutex so per-test iron_runtime_init/shutdown cycles (Unity setUp/tearDown) are safe; return value is ignored in iron_runtime_init because the runtime has no panic channel
- [Phase 59-net-foundation-windows-ci-url P01c]: iron_runtime_shutdown tears down WSA via Iron_net_wsa_cleanup_once BEFORE iron_threads_shutdown so elastic I/O workers still holding sockets don't race WSACleanup (Windows-only ordering concern; POSIX path is a no-op)
- [Phase 59-net-foundation-windows-ci-url P01c]: String.from_byte moved from Phase 59 P05 to P01c per checker warning #3 — all three string primitives (rindex_of / byte_at / from_byte) ship together to avoid split ownership
- [Phase 59-net-foundation-windows-ci-url P01c]: String primitive C impls use lowercase Iron_string_ prefix (Iron_string_rindex_of/byte_at/from_byte) to match existing Iron_string_index_of convention — the plan's Iron_String_ capitalization was incorrect
- [Phase 59-net-foundation-windows-ci-url P01c]: Duration value type shipped in stdlib/time.iron for future Phase 63 HTTP server consumption; NOT wired into Phase 59 net.iron signatures (net APIs take plain Int ms per the 2026-04-10 CONTEXT revision)
- [Phase 59-net-foundation-windows-ci-url P01c]: ironc build-path parity is mandatory — every new runtime .c file needs BOTH CMakeLists.txt iron_runtime sources AND src/cli/build.c runtime source list + argv_buf entries (plus platform-specific link libs like ws2_32.lib on Windows); missing the build.c edit silently breaks `iron build` without breaking `cmake --build`
- [Phase 59-net-foundation-windows-ci-url P01c]: **HARD BLOCKER — tuple-return codegen gap**: Iron frontend has zero tuple support (grep of src/parser/analyzer/hir/lir returns nothing). `ironc check` on `-> (Int, Iron_Error)` hangs with no diagnostic; `ironc build` allocates unboundedly (>3.6 GB RSS in 2min before kill). **All Phase 59 network APIs (P02 TCP, P03 UDP, P04 DNS) MUST use out-param fallback style** — e.g. `func Net.tcp_dial(host: String, port: Int, sock_out: *Int) -> Iron_Error` — NOT `-> (TcpSocket, Iron_Error)` tuples. Smoke test kept at tests/integration/tuple_return_smoke.iron (no .expected → runner skips) as regression target for the future phase that implements tuple returns.
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: Tuple literal storage reuses Iron_ArrayLit with is_tuple sentinel on type_ann (Decision 1 Option B) — zero new AST node kinds; the typechecker inspects the sentinel and stamps real IRON_TYPE_TUPLE on al->resolved_type
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: Destructure lowering desugars to let+field_access at the HIR boundary (Decision 2 Option A) — zero new HIR stmt/expr kinds; mirrors the spawn multi-stmt precedent in hir_lower.c
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: Tuple mangling uses iron_type_to_string per element with non-identifier characters sanitized to underscore — mirrors emit_helpers.c array list type name sanitization
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: Parser arity >= 2 enforced in both type annotation and literal branches — (x) stays a parenthesized expression / single type; no 1-tuples; no unit ()
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: tuple_return_smoke.iron rewritten from (Int, Iron_Error) to (Int, Int) — Iron_Error is a C-only runtime type and was never a legal Iron annotation; 01c's 'regression marker' aspiration was based on a false premise
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: test_tuple_codegen.c uses compile-time _Static_assert + runtime offsetof cross-check vs stdlib Iron_Result_String_Error — the binary layout lock lives in the test harness rather than being extracted from a compiled Iron artifact because net APIs use Int codes not Iron_Error
- [Phase Phase 59-net-foundation-windows-ci-url P01d]: Parser no-progress guard wired on BOTH iron_parse_block AND the top-level iron_parse loop, plus advance-on-error in iron_parse_primary fall-through and iron_parse_val_decl 'expected variable name' path — defense-in-depth against the 01c hang pattern
- [Phase 59-net-foundation-windows-ci-url]: P02: Iron side uses object NetError + (Handle, NetError) tuple returns instead of the C Iron_Error type (not a legal Iron type). Preserves err.code ergonomics while staying in the Iron type system.
- [Phase 59-net-foundation-windows-ci-url]: P02: iron_net.h is NOT included by generated C. emit_c.c Phase 3 emits extern prototypes for the 7 net stub functions directly into ctx.prototypes (after struct_bodies), avoiding the redefinition conflict that arises when both the header and the compiler emit struct bodies for NetError/TcpSocket/TcpListener.
- [Phase 59-net-foundation-windows-ci-url]: P02: iron_type_to_string IRON_TYPE_OBJECT/INTERFACE returns the concrete decl->name instead of '<object>'/'<interface>' literals — mandatory correctness fix for tuple mangling. Prior to this, every tuple containing an object mangled to the same Iron_Tuple__object__* C struct name (silent collision).
- [Phase 59-net-foundation-windows-ci-url]: P02: All blocking TCP paths use Iron_Deadline + non-blocking socket + poll/WSAPoll, never SO_RCVTIMEO/SO_SNDTIMEO. Error translation tables are static switches with compile-time literal messages — no heap, no snprintf on the hot path.
- [Phase 59-net-foundation-windows-ci-url]: P02: IPV6_V6ONLY=0 is set on any AF_INET6 socket returned by getaddrinfo (not only when host literally == '::'). Broader than the plan sketch and removes string-matching fragility.
- [Phase 59-net-foundation-windows-ci-url]: P03: enum Address { V4(IPv4Addr), V6(IPv6Addr) } — Iron keyword is enum, not variant; the plan text was ambiguous, semantics identical to generic_enum_match fixture's Option[T] pattern
- [Phase 59-net-foundation-windows-ci-url]: P03: Iron object layout is canonical — C-side Iron_IPv4Addr mirrors Iron-emitted {int64_t a,b,c,d} byte-for-byte; marshalling to uint8_t[4] happens at the inet_pton boundary via ipv4_octets_{from,to}_bytes helpers
- [Phase 59-net-foundation-windows-ci-url]: P03: Iron_IPv6Addr stores 16 raw octets as Iron_String payload (SSO inline since 16<23) — avoids 16x int64 field blowup and matches iron_io.c binary buffer convention; inet_ntop reads through iron_string_cstr(&bytes)
- [Phase 59-net-foundation-windows-ci-url]: P03: Iron_UdpRecvResult is a 6-field struct-return ABI — not a 6-tuple — because 59-01d tuple codegen is proven for 2-tuples only; matches iron_io_read_file_result convention
- [Phase 59-net-foundation-windows-ci-url]: P03: Flat Net.udp_sendto_v4/v6 helpers instead of a single Net.udp_sendto(addr: Address) — keeps C ABI free of variant plumbing; any Address-dispatching wrapper lives as Iron source-level match code
- [Phase 59-net-foundation-windows-ci-url]: P03: C symbol names match hir_to_lir instance-method mangling — IPv4Addr.parse → Iron_ipv4addr_parse (not Iron_net_ipv4addr_parse); Net.* static methods keep Iron_net_* prefix as expected. Capital-first aliases retained as static-inline header wrappers for Unity tests
- [Phase 59-net-foundation-windows-ci-url]: P05: Static-method-with-body dispatch via call-site synth-self in hir_to_lir.c, NOT a decl-side heuristic. The body-uses-self check fails for instance methods like edge_zero_field's func TypeA.tag() { return 1 } that don't reference self but ARE called as instance methods.
- [Phase 59-net-foundation-windows-ci-url]: P05: Case-insensitive builtin-type FUNC_REF match for static call detection. String/Int/Float/Bool IDENTs lower to FUNC_REF with source-case names but type_name is lowercased at mangling — scoped the case-insensitive compare to the FUNC_REF branch only to avoid collapsing user-defined type names.
- [Phase 59-net-foundation-windows-ci-url]: P05: Plain String == String was a pre-existing bug in emit_c.c — only the tuple-element path routed through iron_string_equals. Extended both IRON_LIR_EQ/NEQ emitters (expression + statement) to handle non-tuple IRON_TYPE_STRING operands via iron_string_equals.
- [Phase 59-net-foundation-windows-ci-url]: P05: Synth-self uses uninitialised alloca + load rather than explicit zero-init literal — no LIR_ZERO primitive exists and the method body is guaranteed not to read self (that's why it's safe to synth in the first place). Future improvement: add LIR_ZERO for defensiveness.
- [Phase 59-net-foundation-windows-ci-url]: P05: stdlib/url.iron is 638 lines of pure Iron — zero @extern_c, zero #include, zero FFI. Every helper (is_unreserved/is_sub_delim/byte_to_hex/int_to_string/etc.) is a method on Url and gets dispatched via the new synth-self bridge; Url_Builder uses functional-update chaining because Iron object vals can't mutate self in place.
- [Phase 59-net-foundation-windows-ci-url]: P04: Abandoned-flag variant of Iron_PoolWait for DNS (not refcount) — caller owns DnsJob end-to-end on normal path, worker's dns_abandoned_destructor frees addrinfo+wait+job on abandoned path. Pattern reusable for TLS handshake and any libc blocking call without native cancellation.
- [Phase 59-net-foundation-windows-ci-url]: P04: Tuple type name for ([Address], NetError) is Iron_Tuple__Address__NetError — matches tuple_build_mangled_name byte-for-byte (iron_type_to_string on [Address] yields '[Address]', sanitized to '_Address_', producing the double underscore between Tuple and Address). iron_net.h + iron_net.c + emit_c.c all reference this exact spelling.
- [Phase 59-net-foundation-windows-ci-url]: P04: hir_to_lir.c list-method dispatcher gained IRON_TYPE_ENUM case in elem_suffix switch — was defaulting to int64_t for [Enum].method() calls. Minimal fix that unlocks future Iron stdlib APIs returning [Enum] lists.
- [Phase 59-net-foundation-windows-ci-url]: P04: DnsJob passes itself as the Iron_poolwait_worker_finish result payload. Single pointer + single destructor covers both ownership models cleanly. Avoids the 'wait->result readback without a getter' problem that would otherwise require extending Iron_PoolWait's API across plans.
- [Phase 59-net-foundation-windows-ci-url]: P04: Iron_List_Iron_Address + IRON_LIST_DECL/IMPL emitted inline by emit_c.c's need_dns branch, NOT by emit_mono_list_decls. The mono scan only handles IRON_TYPE_OBJECT; extending it to ENUM would affect every enum-in-array user. Phase 3 stub-prototype block is strictly more targeted — only emits when Iron_net_lookup_host is referenced.
- [Phase 59-net-foundation-windows-ci-url]: P06 sequential dial-before-accept in net_full_roundtrip.iron instead of spawn-based concurrent listener; loopback SYN queue reliably holds the pending connection between dial-returns and accept-is-called, and spawn+Net was unproven end-to-end
- [Phase 59-net-foundation-windows-ci-url]: P06 test_stdlib_net_cross_platform.c is a positive-path smoke battery exercising TCP/UDP/IP/DNS in one binary; edge cases live in the per-subsystem Unity tests (_tcp/_udp/_ip/_dns) and the cross-platform file is deliberately minimal for easy Windows regression bisection
- [Phase 59-net-foundation-windows-ci-url]: P06 Windows ctest exclusion regex -E 'benchmark|test_integration' is belt-and-braces; test_integration is already CMake-guarded if(NOT WIN32), the regex makes intent explicit and survives future CMakeLists refactors
- [Phase 65-correctness-audit]: No H-severity findings in parser+lexer; all 182 issues are M (OOM paths) or L (theoretical)
- [Phase 65-correctness-audit]: stb_ds-to-arena pointer storage classified as M (design tradeoff), allocation errors counted per call-site for accurate remediation scope
- [Phase 65-correctness-audit]: typecheck.c:1360/1785 SYM_TYPE->ObjectDecl blind casts classified H because InterfaceDecl/EnumDecl decl_node silently misinterprets memory
- [Phase 65-correctness-audit]: resolve.c:674/680 in-place node reinterpretation classified H -- layout-dependent UB; allocation error handling is largest category (40 M-severity issues)
- [Phase 65-correctness-audit]: Iron_ExprNode common layout assumption in hir_lower.c:109 classified H -- locally-defined struct bypasses real type definitions
- [Phase 65-correctness-audit]: comptime.c sys/stat.h and mkdir() classified H for cross-platform -- POSIX-only APIs break Windows
- [Phase 65-correctness-audit]: collect_mono_enums_node switch incomplete (8 missing node kinds) classified M -- silently misses monomorphized enums
- [Phase 65-correctness-audit]: emit_c.c emit_instr main switch is fully exhaustive over all 44 IronLIR_OpKind values
- [Phase 65-correctness-audit]: 2 unique H-severity: generated HEAP_ALLOC and RC_ALLOC malloc without NULL check in emitted C code
- [Phase 65-correctness-audit]: LIR audit totals: 4H, 42M, 84L = 130 findings across 15,818 lines in 11 files

### Roadmap Evolution

- Phase 51 inserted: Memory Investigation & Leak Audit (resolved)
- v0.1.2-alpha milestone created: Phases 52-54 (emitter refactoring, analysis improvements, test hardening)
- v0.2.0-alpha milestone created: Phases 59-64 (Networking Standard Library)
- v0.2.0-alpha paused at Phase 59 shipped; Phases 60-64 marked [paused]
- v0.1.4-alpha milestone created: Phases 65-70 (Compiler Correctness & Maintenance)

### Pending Todos

None yet.

### Blockers/Concerns

- emit_c.c monolithic problem RESOLVED by Phase 52 (4 sub-modules extracted)
- Value range analysis uses conservative TOP at call boundaries -- RESOLVED by Phase 53 Plan 01
- Monomorphic detection is local-only -- Phase 53 extends it interprocedurally

## Session Continuity

Last session: 2026-04-12T15:02:44.288Z
Stopped at: Completed 65-04-PLAN.md
Resume file: None
