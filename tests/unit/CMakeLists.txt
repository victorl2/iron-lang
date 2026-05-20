# Unit tests: C test executables linked against iron_compiler, iron_runtime,
# iron_stdlib, and the Unity test framework.
# All tests are labelled "unit" for ctest -L unit filtering.

# ── Compiler unit tests ──────────────────────────────────────────────────────
add_executable(test_arena test_arena.c)
target_link_libraries(test_arena iron_compiler unity)
add_test(NAME test_arena COMMAND test_arena)
set_tests_properties(test_arena PROPERTIES LABELS "unit")

add_executable(test_diagnostics test_diagnostics.c)
target_link_libraries(test_diagnostics iron_compiler unity)
add_test(NAME test_diagnostics COMMAND test_diagnostics)
set_tests_properties(test_diagnostics PROPERTIES LABELS "unit")

add_executable(test_lexer test_lexer.c)
target_link_libraries(test_lexer iron_compiler unity)
add_test(NAME test_lexer COMMAND test_lexer)
set_tests_properties(test_lexer PROPERTIES LABELS "unit")

add_executable(test_parser test_parser.c)
target_link_libraries(test_parser iron_compiler unity)
add_test(NAME test_parser COMMAND test_parser)
set_tests_properties(test_parser PROPERTIES LABELS "unit")

add_executable(test_parser_errors test_parser_errors.c)
target_link_libraries(test_parser_errors iron_compiler unity)
add_test(NAME test_parser_errors COMMAND test_parser_errors)
set_tests_properties(test_parser_errors PROPERTIES LABELS "unit")

# Phase 16 Wave 0: v4 keyword tokenization (RED until Plan 16-02 lands lexer additions).
add_executable(test_lexer_v4_keywords test_lexer_v4_keywords.c)
target_link_libraries(test_lexer_v4_keywords iron_compiler unity)
add_test(NAME test_lexer_v4_keywords COMMAND test_lexer_v4_keywords)
set_tests_properties(test_lexer_v4_keywords PROPERTIES LABELS "unit")

# Phase 16 Wave 0: v4 keyword binding-position rejection (RED until Plan 16-02 lands parser helper).
add_executable(test_parser_v4_keyword_rejection test_parser_v4_keyword_rejection.c)
target_link_libraries(test_parser_v4_keyword_rejection iron_compiler unity)
add_test(NAME test_parser_v4_keyword_rejection COMMAND test_parser_v4_keyword_rejection)
set_tests_properties(test_parser_v4_keyword_rejection PROPERTIES LABELS "unit")

# Phase 17 Wave 0: VAL-01 (bare local) + VAL-02 (bare field) parser rejection.
# RED until Plan 17-01 lands IRON_ERR_MISSING_VAL_VAR=176 + parser lookahead.
add_executable(test_parser_val_var_required test_parser_val_var_required.c)
target_link_libraries(test_parser_val_var_required iron_compiler unity)
add_test(NAME test_parser_val_var_required COMMAND test_parser_val_var_required)
set_tests_properties(test_parser_val_var_required PROPERTIES
    LABELS "unit;phase17-invariant")

# Phase 17 Wave 0 (Plan 02): VAL-03 init-context check + VAL-04 var-field
# reassignability. RED until Plan 17-02 lands IRON_ERR_VAL_FIELD_REASSIGN=265
# and extends typecheck.c:3924-3969 with the non-pub val branch.
add_executable(test_analyzer_val_field_reassign test_analyzer_val_field_reassign.c)
target_link_libraries(test_analyzer_val_field_reassign iron_compiler unity)
add_test(NAME test_analyzer_val_field_reassign COMMAND test_analyzer_val_field_reassign)
set_tests_properties(test_analyzer_val_field_reassign PROPERTIES
    LABELS "unit;phase17-invariant")

# Phase 17 Wave 0 (Plan 02): VAL-05 + VAL-06 unused-var pass.
# RED until Plan 17-02 lands IRON_WARN_UNUSED_VAR=613 +
# IRON_WARN_UNUSED_VAR_PARAM=614 + src/analyzer/unused_var.c pass +
# src/analyzer/analyzer.c dispatcher insertion between Step 3.5 and Step 4.
add_executable(test_analyzer_unused_var test_analyzer_unused_var.c)
target_link_libraries(test_analyzer_unused_var iron_compiler unity)
add_test(NAME test_analyzer_unused_var COMMAND test_analyzer_unused_var)
set_tests_properties(test_analyzer_unused_var PROPERTIES
    LABELS "unit;phase17-invariant")

# Phase 18 Wave 0 (Plan 01): PARM-01 read-only parameter mutation rejection
# + PARM-02 var-opt-in sanity. RED until Plan 18-01 lands
# IRON_ERR_PARM_READ_ONLY=266 in src/diagnostics/diagnostics.h and
# extends typecheck.c IRON_NODE_ASSIGN handler with sym_kind == IRON_SYM_PARAM
# branch.
add_executable(test_analyzer_parm_read_only test_analyzer_parm_read_only.c)
target_link_libraries(test_analyzer_parm_read_only iron_compiler unity)
add_test(NAME test_analyzer_parm_read_only COMMAND test_analyzer_parm_read_only)
set_tests_properties(test_analyzer_parm_read_only PROPERTIES
    LABELS "unit;phase18-invariant")

# Phase 18 Wave 0 (Plan 01): VAL-06 false-positive regression test.
# RED until Plan 18-01 extends src/analyzer/unused_var.c scan_for_writes
# to mark FIELD_ACCESS-rooted IDENTs as written.
add_executable(test_analyzer_unused_var_param_field_write
    test_analyzer_unused_var_param_field_write.c)
target_link_libraries(test_analyzer_unused_var_param_field_write iron_compiler unity)
add_test(NAME test_analyzer_unused_var_param_field_write
    COMMAND test_analyzer_unused_var_param_field_write)
set_tests_properties(test_analyzer_unused_var_param_field_write PROPERTIES
    LABELS "unit;phase18-invariant")

# Phase 18 Wave 0 (Plan 02): PARM-03 read-only-arg-to-var-slot rejection
# covering BOTH IRON_NODE_CALL (free-function) AND IRON_NODE_METHOD_CALL
# (method-call) paths. RED until Plan 18-02 lands
# IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 in src/diagnostics/diagnostics.h
# AND extends typecheck.c IRON_NODE_CALL handler at lines 2204-2226
# + IRON_NODE_METHOD_CALL handler at line 2286+ with the per-arg PARM-03
# check using the static helper arg_source_is_mutable.
add_executable(test_analyzer_parm_var_slot test_analyzer_parm_var_slot.c)
target_link_libraries(test_analyzer_parm_var_slot iron_compiler unity)
add_test(NAME test_analyzer_parm_var_slot COMMAND test_analyzer_parm_var_slot)
set_tests_properties(test_analyzer_parm_var_slot PROPERTIES
    LABELS "unit;phase18-invariant")

# Phase 18 Wave 0 (Plan 02): PARM-04 method receiver tier regression-anchor
# (NOT a Wave 0 RED gate — Plan 18-02 leaves the existing Phase 84
# IRON_ERR_READONLY_WRITE_SELF=238 enforcement untouched). 3 RUN_TESTs
# verify default-mutable receiver works + readonly opt-out continues to
# emit E0238 + CONTEXT.md "reuse 265" amendment lock (KEEP 238 dedicated).
add_executable(test_analyzer_method_receiver test_analyzer_method_receiver.c)
target_link_libraries(test_analyzer_method_receiver iron_compiler unity)
add_test(NAME test_analyzer_method_receiver COMMAND test_analyzer_method_receiver)
set_tests_properties(test_analyzer_method_receiver PROPERTIES
    LABELS "unit;phase18-invariant")

add_executable(test_interp test_interp.c)
target_link_libraries(test_interp iron_compiler unity)
add_test(NAME test_interp COMMAND test_interp)
set_tests_properties(test_interp PROPERTIES LABELS "unit")

add_executable(test_printer test_printer.c)
target_link_libraries(test_printer iron_compiler unity)
add_test(NAME test_printer COMMAND test_printer)
set_tests_properties(test_printer PROPERTIES LABELS "unit")

add_executable(test_types test_types.c)
target_link_libraries(test_types iron_compiler unity)
add_test(NAME test_types COMMAND test_types)
set_tests_properties(test_types PROPERTIES LABELS "unit")

add_executable(test_resolver test_resolver.c)
target_link_libraries(test_resolver iron_compiler unity)
add_test(NAME test_resolver COMMAND test_resolver)
set_tests_properties(test_resolver PROPERTIES LABELS "unit")

add_executable(test_typecheck test_typecheck.c)
target_link_libraries(test_typecheck iron_compiler unity)
add_test(NAME test_typecheck COMMAND test_typecheck)
set_tests_properties(test_typecheck PROPERTIES LABELS "unit")

add_executable(test_escape test_escape.c)
target_link_libraries(test_escape iron_compiler unity)
add_test(NAME test_escape COMMAND test_escape)
set_tests_properties(test_escape PROPERTIES LABELS "unit")

add_executable(test_concurrency test_concurrency.c)
target_link_libraries(test_concurrency iron_compiler unity)
add_test(NAME test_concurrency COMMAND test_concurrency)
set_tests_properties(test_concurrency PROPERTIES LABELS "unit")

add_executable(test_init_check test_init_check.c)
target_link_libraries(test_init_check iron_compiler unity)
add_test(NAME test_init_check COMMAND test_init_check)
set_tests_properties(test_init_check PROPERTIES LABELS "unit")

add_executable(test_comptime test_comptime.c)
target_link_libraries(test_comptime iron_compiler unity)
add_test(NAME test_comptime COMMAND test_comptime)
set_tests_properties(test_comptime PROPERTIES LABELS "unit")

# ── Phase 01 M0 Plan 01 (HARD-01): iron_analyze_buffer API smoke test ──────
add_executable(test_analyze_buffer_basic test_analyze_buffer_basic.c)
target_link_libraries(test_analyze_buffer_basic iron_compiler unity)
add_test(NAME test_analyze_buffer_basic COMMAND test_analyze_buffer_basic)
set_tests_properties(test_analyze_buffer_basic PROPERTIES LABELS "unit;phase-m0-invariant")

# ── Phase 01 M0 Plan 02 (HARD-04): ErrorNode-tolerance invariant ───────────
add_executable(test_analyzer_errornode test_analyzer_errornode.c)
target_link_libraries(test_analyzer_errornode iron_compiler unity)
add_test(NAME test_analyzer_errornode COMMAND test_analyzer_errornode)
set_tests_properties(test_analyzer_errornode PROPERTIES LABELS "unit;phase-m0-invariant")

# ── Phase 01 M0 Plan 02 (HARD-03): no-short-circuit invariant ──────────────
add_executable(test_analyzer_no_short_circuit test_analyzer_no_short_circuit.c)
target_link_libraries(test_analyzer_no_short_circuit iron_compiler unity)
add_test(NAME test_analyzer_no_short_circuit COMMAND test_analyzer_no_short_circuit)
set_tests_properties(test_analyzer_no_short_circuit PROPERTIES LABELS "unit;phase-m0-invariant")

# ── Phase 01 M0 Plan 03 (HARD-05): cancel_flag latency invariant ───────────
add_executable(test_cancel_latency test_cancel_latency.c)
target_link_libraries(test_cancel_latency iron_compiler unity pthread)
add_test(NAME test_cancel_latency COMMAND test_cancel_latency)
set_tests_properties(test_cancel_latency PROPERTIES LABELS "unit;phase-m0-invariant" TIMEOUT 30)

# ── Phase 01 M0 Plan 03 (HARD-06): per-request arena RSS stability ─────────
add_executable(test_arena_scoping_stress test_arena_scoping_stress.c)
target_link_libraries(test_arena_scoping_stress iron_compiler unity)
add_test(NAME test_arena_scoping_stress COMMAND test_arena_scoping_stress)
set_tests_properties(test_arena_scoping_stress PROPERTIES LABELS "unit;phase-m0-invariant" TIMEOUT 120)

# ── Phase 01 M0 Plan 04 (HARD-07): pthread_once thread-safe types init ─────
add_executable(test_types_init_once test_types_init_once.c)
target_link_libraries(test_types_init_once iron_compiler unity pthread)
add_test(NAME test_types_init_once COMMAND test_types_init_once)
set_tests_properties(test_types_init_once PROPERTIES LABELS "unit;phase-m0-invariant" TIMEOUT 60)

# ── Phase 01 M0 Plan 04 (HARD-08): parser recursion-depth guard ─────────────
add_executable(test_parser_recursion_guard test_parser_recursion_guard.c)
target_link_libraries(test_parser_recursion_guard iron_compiler unity)
add_test(NAME test_parser_recursion_guard COMMAND test_parser_recursion_guard)
set_tests_properties(test_parser_recursion_guard PROPERTIES LABELS "unit;phase-m0-invariant" TIMEOUT 60)

# ── Phase 01 M0 Plan 05 (HARD-02): LSP-mode comptime FS-gating invariant ───
add_executable(test_analyze_buffer_mode_lsp test_analyze_buffer_mode_lsp.c)
target_link_libraries(test_analyze_buffer_mode_lsp iron_compiler unity)
add_test(NAME test_analyze_buffer_mode_lsp COMMAND test_analyze_buffer_mode_lsp)
set_tests_properties(test_analyze_buffer_mode_lsp PROPERTIES LABELS "unit;phase-m0-invariant" TIMEOUT 30)

# ── Runtime unit tests ───────────────────────────────────────────────────────
add_executable(test_runtime_string test_runtime_string.c)
target_link_libraries(test_runtime_string iron_runtime unity)
add_test(NAME test_runtime_string COMMAND test_runtime_string)
set_tests_properties(test_runtime_string PROPERTIES LABELS "unit")

# Phase 96 STR-01: Unity tests for the iron_string_concat runtime helper
# (NULL-safety, SSO + heap thresholds, chained calls).
add_executable(test_string_concat test_string_concat.c)
target_link_libraries(test_string_concat iron_runtime unity)
add_test(NAME test_string_concat COMMAND test_string_concat)
set_tests_properties(test_string_concat PROPERTIES LABELS "unit")

add_executable(test_runtime_threads test_runtime_threads.c)
target_link_libraries(test_runtime_threads iron_runtime unity)
add_test(NAME test_runtime_threads COMMAND test_runtime_threads)
set_tests_properties(test_runtime_threads PROPERTIES LABELS "unit")

add_executable(test_runtime_deadline test_runtime_deadline.c)
target_link_libraries(test_runtime_deadline iron_runtime unity)
add_test(NAME test_runtime_deadline COMMAND test_runtime_deadline)
set_tests_properties(test_runtime_deadline PROPERTIES LABELS "unit")

add_executable(test_runtime_elastic_pool test_runtime_elastic_pool.c)
target_link_libraries(test_runtime_elastic_pool iron_runtime unity)
add_test(NAME test_runtime_elastic_pool COMMAND test_runtime_elastic_pool)
set_tests_properties(test_runtime_elastic_pool PROPERTIES LABELS "unit")

add_executable(test_runtime_net_init test_runtime_net_init.c)
target_link_libraries(test_runtime_net_init iron_runtime unity)
add_test(NAME test_runtime_net_init COMMAND test_runtime_net_init)
set_tests_properties(test_runtime_net_init PROPERTIES LABELS "unit")

add_executable(test_runtime_collections test_runtime_collections.c)
target_link_libraries(test_runtime_collections iron_runtime unity)
add_test(NAME test_runtime_collections COMMAND test_runtime_collections)
set_tests_properties(test_runtime_collections PROPERTIES LABELS "unit")

# ── Phase 67-02: FIX-01 rank 1 + 2 OOM guard regression tests ───────────────
# Exercise the iron_oom_abort capacity-overflow paths in IRON_LIST_IMPL _push
# and IRON_MAP_IMPL _put. Fork-based so the child's abort() does not kill the
# test binary; parent verifies SIGABRT + stderr diagnostic message.
add_executable(test_alloc_list_push_oom test_alloc_list_push_oom.c)
target_link_libraries(test_alloc_list_push_oom iron_runtime unity)
add_test(NAME test_alloc_list_push_oom COMMAND test_alloc_list_push_oom)
set_tests_properties(test_alloc_list_push_oom PROPERTIES LABELS "unit")

add_executable(test_alloc_map_put_oom test_alloc_map_put_oom.c)
target_link_libraries(test_alloc_map_put_oom iron_runtime unity)
add_test(NAME test_alloc_map_put_oom COMMAND test_alloc_map_put_oom)
set_tests_properties(test_alloc_map_put_oom PROPERTIES LABELS "unit")

# ── Stdlib unit test ─────────────────────────────────────────────────────────
add_executable(test_stdlib test_stdlib.c)
target_link_libraries(test_stdlib iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib COMMAND test_stdlib)
set_tests_properties(test_stdlib PROPERTIES LABELS "unit")

# ── Phase 59 01d: tuple binary-layout lock ──────────────────────────────────
add_executable(test_tuple_codegen test_tuple_codegen.c)
target_link_libraries(test_tuple_codegen iron_stdlib iron_runtime unity)
add_test(NAME test_tuple_codegen COMMAND test_tuple_codegen)
set_tests_properties(test_tuple_codegen PROPERTIES LABELS "unit")

# ── Phase 59 02: TCP stdlib Unity tests ─────────────────────────────────────
add_executable(test_stdlib_net_tcp test_stdlib_net_tcp.c)
target_link_libraries(test_stdlib_net_tcp iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib_net_tcp COMMAND test_stdlib_net_tcp)
set_tests_properties(test_stdlib_net_tcp PROPERTIES LABELS "unit")

# ── Phase 59 03: UDP + IP address stdlib Unity tests ────────────────────────
add_executable(test_stdlib_net_udp test_stdlib_net_udp.c)
target_link_libraries(test_stdlib_net_udp iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib_net_udp COMMAND test_stdlib_net_udp)
set_tests_properties(test_stdlib_net_udp PROPERTIES LABELS "unit")

add_executable(test_stdlib_net_ip test_stdlib_net_ip.c)
target_link_libraries(test_stdlib_net_ip iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib_net_ip COMMAND test_stdlib_net_ip)
set_tests_properties(test_stdlib_net_ip PROPERTIES LABELS "unit")

# ── Phase 59 04: DNS stdlib Unity tests ────────────────────────────────────
add_executable(test_stdlib_net_dns test_stdlib_net_dns.c)
target_link_libraries(test_stdlib_net_dns iron_stdlib iron_runtime unity)
add_test(NAME test_stdlib_net_dns COMMAND test_stdlib_net_dns)
set_tests_properties(test_stdlib_net_dns PROPERTIES LABELS "unit")

# ── Phase 59 06: Cross-platform networking battery ─────────────────────────
# Exercises the full C-level networking surface (TCP loopback + UDP loopback +
# IPv4/IPv6 parse/format + localhost DNS) in a single binary. Runs identically
# on ubuntu-latest, macos-latest, and windows-latest so Windows CI can catch
# regressions across the entire net stack with one test executable.
add_executable(test_stdlib_net_cross_platform test_stdlib_net_cross_platform.c)
target_link_libraries(test_stdlib_net_cross_platform iron_stdlib iron_runtime unity)
if(WIN32)
    target_link_libraries(test_stdlib_net_cross_platform ws2_32 iphlpapi)
endif()
add_test(NAME test_stdlib_net_cross_platform COMMAND test_stdlib_net_cross_platform)
set_tests_properties(test_stdlib_net_cross_platform PROPERTIES LABELS "unit")

# ── Phase 2 (CLI + TOML scaffold) tests ──────────────────────────────────────
# test_toml_web compiles src/cli/toml.c directly because toml.c is not currently
# part of any static library (it's linked straight into the ironc executable in
# the root CMakeLists.txt). A future refactor could extract an iron_cli_toml
# library, but for Phase 2 this direct-compile approach keeps the diff minimal.
add_executable(test_toml_web
    test_toml_web.c
    ${CMAKE_SOURCE_DIR}/src/cli/toml.c
    # Phase 5 Plan 05-01 fix: toml.c calls iron_fmt_options_default()
    ${CMAKE_SOURCE_DIR}/src/fmt/options.c
)
target_include_directories(test_toml_web PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_toml_web unity)
add_test(NAME test_toml_web COMMAND test_toml_web)
set_tests_properties(test_toml_web PROPERTIES LABELS "unit")

# Phase 94 LIB-03: [dependencies] path-form parser tests
add_executable(test_toml_path_dep
    test_toml_path_dep.c
    ${CMAKE_SOURCE_DIR}/src/cli/toml.c
    ${CMAKE_SOURCE_DIR}/src/fmt/options.c
)
target_include_directories(test_toml_path_dep PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_toml_path_dep unity)
add_test(NAME test_toml_path_dep COMMAND test_toml_path_dep)
set_tests_properties(test_toml_path_dep PROPERTIES LABELS "unit")

# Phase 95 PIN: semver constraint parser tests
add_executable(test_semver
    test_semver.c
    ${CMAKE_SOURCE_DIR}/src/cli/semver.c
)
target_include_directories(test_semver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_semver unity)
add_test(NAME test_semver COMMAND test_semver)
set_tests_properties(test_semver PROPERTIES LABELS "unit")

# Phase 95 PIN-01: [package].iron field parser tests
add_executable(test_toml_iron_field
    test_toml_iron_field.c
    ${CMAKE_SOURCE_DIR}/src/cli/toml.c
    ${CMAKE_SOURCE_DIR}/src/fmt/options.c
)
target_include_directories(test_toml_iron_field PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_toml_iron_field unity)
add_test(NAME test_toml_iron_field COMMAND test_toml_iron_field)
set_tests_properties(test_toml_iron_field PROPERTIES LABELS "unit")

# Phase 97 HELP-04: central help registry tests
add_executable(test_help_registry
    test_help_registry.c
    ${CMAKE_SOURCE_DIR}/src/cli/help_registry.c
)
target_include_directories(test_help_registry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_help_registry unity)
add_test(NAME test_help_registry COMMAND test_help_registry)
set_tests_properties(test_help_registry PROPERTIES LABELS "unit")

# ── Phase 5 plan-03: web_main_loop_split pass test (WEB-EMIT-01..04) ─────────
add_executable(test_web_main_loop_split test_web_main_loop_split.c)
target_link_libraries(test_web_main_loop_split iron_compiler unity)
add_test(NAME test_web_main_loop_split COMMAND test_web_main_loop_split)
set_tests_properties(test_web_main_loop_split PROPERTIES LABELS "unit")

# ── Phase 6 plan-03: emit_web_module snapshot test (WEB-EMIT-05..08) ─────────
add_executable(test_emit_web test_emit_web.c)
target_link_libraries(test_emit_web iron_compiler unity)
target_compile_definitions(test_emit_web PRIVATE
    IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
)
add_test(NAME test_emit_web COMMAND test_emit_web)
set_tests_properties(test_emit_web PROPERTIES LABELS "unit")

# ── Phase 3 plan-03: web_await_check pass test (WEB-RUNTIME-04) ──────────────
add_executable(test_web_await_check test_web_await_check.c)
target_link_libraries(test_web_await_check iron_compiler unity)
add_test(NAME test_web_await_check COMMAND test_web_await_check)
set_tests_properties(test_web_await_check PROPERTIES LABELS "unit")

# ── Phase 10 plan-03: web_top_level_loader_check pass test (WEB-ASSET-03) ────
add_executable(test_web_top_level_loader_check test_web_top_level_loader_check.c)
target_link_libraries(test_web_top_level_loader_check iron_compiler unity)
add_test(NAME test_web_top_level_loader_check COMMAND test_web_top_level_loader_check)
set_tests_properties(test_web_top_level_loader_check PROPERTIES LABELS "unit")

# ── Phase 3 plan-01: intern table tsan stress test (WEB-RUNTIME-01) ──────────
# Runs under ThreadSanitizer on Linux only. macOS tsan is flaky and emcc has
# no tsan. Non-Linux builds still compile+run the test but without tsan
# instrumentation, giving at least a smoke-test signal.
add_executable(test_string_intern_race test_string_intern_race.c)
target_link_libraries(test_string_intern_race iron_runtime unity)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(test_string_intern_race PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_string_intern_race PRIVATE -fsanitize=thread)
endif()
add_test(NAME test_string_intern_race COMMAND test_string_intern_race)
set_tests_properties(test_string_intern_race PROPERTIES
    LABELS "unit;tsan"
    TIMEOUT 30
    FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer"
)

# ── Phase 13: WEB-TEST-06 shell template substitution assertions ─────────────
add_executable(test_shell_subst test_shell_subst.c)
target_include_directories(test_shell_subst PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_shell_subst unity)
add_test(NAME test_shell_subst COMMAND test_shell_subst)
set_tests_properties(test_shell_subst PROPERTIES LABELS "unit;phase13")

# ── Phase 13: WEB-TEST-05 asset path round-trip structural test ──────────────
add_executable(test_asset_path
    test_asset_path.c
    ${CMAKE_SOURCE_DIR}/src/cli/toml.c
    # Phase 5 Plan 05-01 fix: toml.c calls iron_fmt_options_default()
    ${CMAKE_SOURCE_DIR}/src/fmt/options.c
)
target_include_directories(test_asset_path PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_asset_path unity)
add_test(NAME test_asset_path COMMAND test_asset_path)
set_tests_properties(test_asset_path PROPERTIES LABELS "unit;phase13")

# ── Phase 69 Plan 04 (COV-05): coverage remediation tests ───────────────────
# Each test targets one or more files that Phase 69 Plan 03 flagged below 50%
# line coverage. Grouped by link-target (runtime / stdlib / compiler) to keep
# the number of test binaries small.
#
# Runtime: src/runtime/iron_oom.c (0%) + src/runtime/iron_string.c (36%)
add_executable(test_cov_runtime_string_oom test_cov_runtime_string_oom.c)
target_link_libraries(test_cov_runtime_string_oom iron_runtime unity)
add_test(NAME test_cov_runtime_string_oom COMMAND test_cov_runtime_string_oom)
set_tests_properties(test_cov_runtime_string_oom PROPERTIES LABELS "unit;cov")

# Stdlib: src/stdlib/iron_io.h (0%) + src/stdlib/iron_io.c (22%)
add_executable(test_cov_stdlib_io test_cov_stdlib_io.c)
target_link_libraries(test_cov_stdlib_io iron_stdlib iron_runtime unity)
add_test(NAME test_cov_stdlib_io COMMAND test_cov_stdlib_io)
set_tests_properties(test_cov_stdlib_io PROPERTIES LABELS "unit;cov")

# Compiler printers + analyzer:
#   src/parser/printer.c          (32%)
#   src/hir/hir_print.c           (38%)
#   src/lir/print.c               (43%)
#   src/analyzer/web_await_check.c (49.66%)
add_executable(test_cov_compiler_printers test_cov_compiler_printers.c)
target_link_libraries(test_cov_compiler_printers iron_compiler iron_runtime unity)
add_test(NAME test_cov_compiler_printers COMMAND test_cov_compiler_printers)
set_tests_properties(test_cov_compiler_printers PROPERTIES LABELS "unit;cov")

# ── Phase 4 Plan 04-01 Task 01 (EDIT-07) — Wave 0 test targets ─────────────
# The `phase-m3-invariant` CTest label marks the M3 editing-assistance gate.
# Task 01 registers these targets; Task 02 flips the Wave-0 stubs to LIVE
# assertions. test_typo_candidate is LIVE from Task 01 (the helper is ready).
add_executable(test_typo_candidate test_typo_candidate.c)
target_link_libraries(test_typo_candidate iron_compiler iron_runtime unity)
add_test(NAME test_typo_candidate COMMAND test_typo_candidate)
set_tests_properties(test_typo_candidate PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

add_executable(test_diag_suggestion_seeded test_diag_suggestion_seeded.c)
target_link_libraries(test_diag_suggestion_seeded iron_compiler iron_runtime unity)
add_test(NAME test_diag_suggestion_seeded COMMAND test_diag_suggestion_seeded)
set_tests_properties(test_diag_suggestion_seeded PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

add_executable(test_unused_import_check test_unused_import_check.c)
target_link_libraries(test_unused_import_check iron_compiler iron_runtime unity)
add_test(NAME test_unused_import_check COMMAND test_unused_import_check)
set_tests_properties(test_unused_import_check PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

add_executable(test_missing_return test_missing_return.c)
target_link_libraries(test_missing_return iron_compiler iron_runtime unity)
add_test(NAME test_missing_return COMMAND test_missing_return)
set_tests_properties(test_missing_return PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

add_executable(test_redundant_cast test_redundant_cast.c)
target_link_libraries(test_redundant_cast iron_compiler iron_runtime unity)
add_test(NAME test_redundant_cast COMMAND test_redundant_cast)
set_tests_properties(test_redundant_cast PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

add_executable(test_type_mismatch_literal test_type_mismatch_literal.c)
target_link_libraries(test_type_mismatch_literal iron_compiler iron_runtime unity)
add_test(NAME test_type_mismatch_literal COMMAND test_type_mismatch_literal)
set_tests_properties(test_type_mismatch_literal PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

add_executable(test_parity_ironc_lsp_suggestions test_parity_ironc_lsp_suggestions.c)
target_link_libraries(test_parity_ironc_lsp_suggestions iron_compiler iron_runtime unity)
add_test(NAME test_parity_ironc_lsp_suggestions COMMAND test_parity_ironc_lsp_suggestions)
set_tests_properties(test_parity_ironc_lsp_suggestions PROPERTIES
    LABELS "unit;phase-m3-invariant"
    TIMEOUT 30)

# ── Phase 4 Plan 04-02 Task 01 (EDIT-01, EDIT-06): completion classifier +
# keyword-mirror drift guard. Both tests link the completion TUs directly so
# they can exercise the internal API without booting the full ironls server.
if(IRON_BUILD_LSP)
    add_executable(test_completion_context_classify
        test_completion_context_classify.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c
    )
    target_include_directories(test_completion_context_classify PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_completion_context_classify
        iron_compiler iron_runtime unity)
    add_test(NAME test_completion_context_classify
        COMMAND test_completion_context_classify)
    set_tests_properties(test_completion_context_classify PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    add_executable(test_completion_keyword_mirror
        test_completion_keyword_mirror.c
    )
    target_include_directories(test_completion_keyword_mirror PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_completion_keyword_mirror
        iron_compiler iron_runtime unity)
    add_test(NAME test_completion_keyword_mirror
        COMMAND test_completion_keyword_mirror)
    set_tests_properties(test_completion_keyword_mirror PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-02 Task 02 (EDIT-01, D-01): 6-bucket builder tests.
    # Compiles the facade bucket TUs directly so the test can exercise the
    # internal API without booting ironls; links the fuzzy scorer + lexer
    # + parser + yyjson-ish deps via iron_compiler.
    add_executable(test_completion_buckets
        test_completion_buckets.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/context_classify.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/buckets.c
        # Phase 12 Plan 12-02 (KW-03): buckets.c references
        # ilsp_keyword_visible_at via emit_keywords; both buckets.c and
        # keyword_filter.c call ilsp_byte_of_line / ilsp_line_of_byte
        # for cursor (line, col) derivation.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/keyword_filter.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/fuzzy.c
    )
    target_include_directories(test_completion_buckets PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_completion_buckets
        iron_compiler iron_runtime unity m)
    add_test(NAME test_completion_buckets
        COMMAND test_completion_buckets)
    set_tests_properties(test_completion_buckets PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-03 Task 01 (EDIT-05, D-02): auto-import
    # additionalTextEdits builder. Fixture-driven Unity test --
    # parses small Iron sources in-process, walks the decl list for
    # top-of-file imports, and asserts insertion site / dedup / alias
    # honor / nested-path carve-out / PITFALL C error-recovery anchor
    # skip.
    add_executable(test_completion_auto_import
        test_completion_auto_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/auto_import.c
    )
    target_include_directories(test_completion_auto_import PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_completion_auto_import
        iron_compiler iron_runtime unity)
    add_test(NAME test_completion_auto_import
        COMMAND test_completion_auto_import)
    set_tests_properties(test_completion_auto_import PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-03 Task 02 (EDIT-04, D-15): snippet renderer.
    # Golden-string tests asserting the 5 template shapes (function /
    # method / object / match / import / enum-variant) and the LSP
    # Snippet-grammar escape logic for `$`, `}`, `\` per PITFALL D
    # (the `${USER}` variable-substitution leak).
    add_executable(test_completion_snippets
        test_completion_snippets.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/snippet.c
    )
    target_include_directories(test_completion_snippets PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_completion_snippets
        iron_compiler iron_runtime unity)
    add_test(NAME test_completion_snippets
        COMMAND test_completion_snippets)
    set_tests_properties(test_completion_snippets PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-04 Task 01 (EDIT-07, EDIT-08, D-06): code-action
    # registry + 5 P1 quickfix handler golden tests. Compiles the
    # registry TU + 5 quickfix TUs + lsp store modules directly so the
    # test can exercise the handler output shape without booting ironls.
    # Pulls in lsp store (document, line_index, utf, sha256) because
    # quickfix_missing_return derives indentation via ilsp_byte_of_line
    # and the shape tests construct real IronLsp_Document values with
    # ilsp_document_create.
    add_executable(test_codeaction_registry
        test_codeaction_registry.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/registry.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_undefined_var.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_unused_import.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_missing_return.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c
        # Phase 12 Plan 12-02 (QF-01): single command-style handler shared
        # by codes 260 + 261 (registry.c references it via two rows).
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c
        # Phase 12 Plan 12-02 (QF-02): synthesize default init for code 264.
        # quickfix_object_no_init.c calls ilsp_facade_compile_for_nav, so
        # the test must additionally link compile.c + diagnostics.c +
        # nav/nav_common.c + nav/node_at.c + the transport + obs stack
        # (matches test_rename_prepare's link shape).
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_object_no_init.c
        # Phase 12 Plan 12-03 (QF-03): multi-edit handler for code 262.
        # Like quickfix_object_no_init.c, calls ilsp_facade_compile_for_nav
        # and re-tokenizes via iron_lex_all; same link-stack already in place.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_v3_inline_default.c
        # Phase 12 Plan 12-03 (QF-04): 2-action handler for code 238.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_readonly_write_self.c
        # Phase 12 Plan 12-03 (QF-05): 1-or-2 action handler for code 239.
        # Calls ilsp_nav_path_is_stdlib (already linked via nav_common.c).
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/quickfix_readonly_calls_mutating.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        # Phase 12 Plan 12-01 (D-23): shared body-indent helper consumed
        # by quickfix_missing_return.c.
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/codeaction_indent.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        # compile.c + diagnostics.c pull in transport + obs deps.
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
    )
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")
    target_include_directories(test_codeaction_registry PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_codeaction_registry
        iron_compiler iron_runtime unity)
    add_test(NAME test_codeaction_registry
        COMMAND test_codeaction_registry)
    set_tests_properties(test_codeaction_registry PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-06 Task 01 (EDIT-10, EDIT-12, D-09, D-10): rename
    # prepare + collision facade unit tests. prepare.c drives the full
    # cursor→ident→classification path (links document, compile facade,
    # nav plumbing); collision.c is a pure scope-walker (no document
    # needed), tested against hand-built Iron_Scope + Iron_Symbol values.

    add_executable(test_rename_prepare
        test_rename_prepare.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/prepare.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/nav_common.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        # compile.c pulls in json + writer + frame + log; diagnostics.c
        # pulls in json. Link the full transport + obs stack.
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
    )
    target_include_directories(test_rename_prepare PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_compile_definitions(test_rename_prepare PRIVATE _GNU_SOURCE)
    target_link_libraries(test_rename_prepare
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -w")
    add_test(NAME test_rename_prepare COMMAND test_rename_prepare)
    set_tests_properties(test_rename_prepare PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    add_executable(test_rename_collision
        test_rename_collision.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/rename/collision.c
    )
    target_include_directories(test_rename_collision PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_rename_collision
        iron_compiler iron_runtime unity)
    add_test(NAME test_rename_collision COMMAND test_rename_collision)
    set_tests_properties(test_rename_collision PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-05 Task 01 (EDIT-09, D-08): organizeImports facade
    # unit tests. Links the organize_imports facade TU directly alongside
    # the workspace-index store modules it consults (stdlib_cache for
    # classification; dep_map left NULL in tests). Uses pthread for
    # stdlib_cache's pthread_once singleton init.
    add_executable(test_organize_imports
        test_organize_imports.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/codeaction/organize_imports.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/stdlib_cache.c
    )
    target_include_directories(test_organize_imports PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_organize_imports
        iron_compiler iron_runtime unity pthread)
    target_compile_definitions(test_organize_imports PRIVATE
        IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
    )
    add_test(NAME test_organize_imports
        COMMAND test_organize_imports)
    set_tests_properties(test_organize_imports PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-07 Task 01 (EDIT-13, D-12) — documentHighlight.
    # Drives the classifier end-to-end through a real IronLsp_Document
    # and the compile facade (analyzer-backed — needs resolved_sym).
    # Links the full transport + observability stack because
    # ilsp_facade_compile_for_nav pulls in compile.c which pulls in
    # publishDiagnostics serialization.
    add_executable(test_document_highlight
        test_document_highlight.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/highlight.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/compile.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/span.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/diagnostics.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/nav/node_at.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/json.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/writer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
        ${CMAKE_SOURCE_DIR}/src/vendor/yyjson/yyjson.c
    )
    target_include_directories(test_document_highlight PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_compile_definitions(test_document_highlight PRIVATE _GNU_SOURCE)
    target_link_libraries(test_document_highlight
        iron_compiler iron_runtime iron_stdlib unity pthread m)
    add_test(NAME test_document_highlight COMMAND test_document_highlight)
    set_tests_properties(test_document_highlight PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-07 Task 02 (EDIT-14, D-13) — foldingRange. Strict
    # parser-only — the facade links folding_range.c + the lsp store
    # document helpers only; NO compile.c, NO facade/diagnostics, NO
    # transport. The test fixtures include a syntactically broken
    # program to prove Iron_ErrorNode tolerance.
    add_executable(test_folding_range
        test_folding_range.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/folding_range.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
    )
    target_include_directories(test_folding_range PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_folding_range
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_folding_range COMMAND test_folding_range)
    set_tests_properties(test_folding_range PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)

    # Phase 4 Plan 04-07 Task 03 (EDIT-15, D-14) — selectionRange.
    # Parser-only — links selection_range.c + document store only.
    # Covers 7-rung ladder construction, EOF handling, strict
    # containment invariant, Iron_ErrorNode tolerance, leaf literal
    # rung, and multiple positions in one call.
    add_executable(test_selection_range
        test_selection_range.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/selection_range.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
    )
    target_include_directories(test_selection_range PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_selection_range
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_selection_range COMMAND test_selection_range)
    set_tests_properties(test_selection_range PROPERTIES
        LABELS "unit;phase-m3-invariant"
        TIMEOUT 30)
endif()

# ── Phase 5 Plan 05-01 (FMT-01, FMT-05, D-01, D-02) — phase-m4-invariant ──
# Compiler-side foundations: src/fmt/ library + extended printer.
# Tests for the [fmt] toml parser scaffolded in Task 1; live assertions
# land in Task 2 once src/cli/toml.c gains the section==4 branch.

add_executable(test_fmt_options_defaults test_fmt_options_defaults.c)
target_link_libraries(test_fmt_options_defaults iron_compiler iron_runtime unity)
add_test(NAME test_fmt_options_defaults COMMAND test_fmt_options_defaults)
set_tests_properties(test_fmt_options_defaults PROPERTIES
    LABELS "unit;phase-m4-invariant"
    TIMEOUT 30)

add_executable(test_fmt_format_source_unit test_fmt_format_source_unit.c)
target_link_libraries(test_fmt_format_source_unit iron_compiler iron_runtime unity)
add_test(NAME test_fmt_format_source_unit COMMAND test_fmt_format_source_unit)
set_tests_properties(test_fmt_format_source_unit PROPERTIES
    LABELS "unit;phase-m4-invariant"
    TIMEOUT 30)

add_executable(test_fmt_options_honored test_fmt_options_honored.c)
target_link_libraries(test_fmt_options_honored iron_compiler iron_runtime unity)
add_test(NAME test_fmt_options_honored COMMAND test_fmt_options_honored)
set_tests_properties(test_fmt_options_honored PROPERTIES
    LABELS "unit;phase-m4-invariant"
    TIMEOUT 30)

add_executable(test_fmt_options_parse
    test_fmt_options_parse.c
    # Phase 5 Plan 05-01 fix: pull cli/toml.c directly (not in iron_compiler lib)
    ${CMAKE_SOURCE_DIR}/src/cli/toml.c
)
target_link_libraries(test_fmt_options_parse iron_compiler iron_runtime unity)
add_test(NAME test_fmt_options_parse COMMAND test_fmt_options_parse)
set_tests_properties(test_fmt_options_parse PROPERTIES
    LABELS "unit;phase-m4-invariant"
    TIMEOUT 30)

# Phase 5 Plan 05-03 Task 2 (FMT-03, D-04, D-06) -- decl-intersection
# unit test for ilsp_facade_format_range. Drives the facade with
# synthetic IronLsp_Document fixtures and asserts the returned
# TextEdit count / ordering on 5 boundary cases.
if(IRON_BUILD_LSP)
    add_executable(test_fmt_range_locator
        test_fmt_range_locator.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/range_format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
    )
    target_include_directories(test_fmt_range_locator PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_fmt_range_locator
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_fmt_range_locator COMMAND test_fmt_range_locator)
    set_tests_properties(test_fmt_range_locator PROPERTIES
        LABELS "unit;phase-m4-invariant"
        TIMEOUT 30)
endif()

# Phase 5 Plan 05-04 Task 2 (FMT-04, D-05, D-06) -- on-type formatting
# block-finder + per-line indent emitter unit test for
# ilsp_facade_format_on_type. Drives the facade with 5 boundary cases
# (top-level fn, already-correct, string-literal, parse-error,
# non-brace trigger).
if(IRON_BUILD_LSP)
    add_executable(test_fmt_on_type_block_finder
        test_fmt_on_type_block_finder.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/facade/fmt/on_type_format.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/document.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/line_index.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/utf.c
        ${CMAKE_SOURCE_DIR}/src/lsp/store/sha256.c
    )
    target_include_directories(test_fmt_on_type_block_finder PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated
    )
    target_link_libraries(test_fmt_on_type_block_finder
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_fmt_on_type_block_finder COMMAND test_fmt_on_type_block_finder)
    set_tests_properties(test_fmt_on_type_block_finder PROPERTIES
        LABELS "unit;phase-m4-invariant"
        TIMEOUT 30)
endif()

# ── Phase 7 Plan 07-01 Task 01 (HARD-14, D-02): crash-dump pipeline
#    Unity test. Labelled phase-m6-invariant. ─────────────────────────
if(IRON_BUILD_LSP)
    add_executable(test_crash_ring_buffer
        lsp/obs/test_crash_ring_buffer.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/crash_dump.c)
    target_include_directories(test_crash_ring_buffer PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_crash_ring_buffer PRIVATE
        _GNU_SOURCE
        IRON_VERSION_STRING="${IRON_VERSION_FULL}")
    target_link_libraries(test_crash_ring_buffer
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_crash_ring_buffer COMMAND test_crash_ring_buffer)
    set_tests_properties(test_crash_ring_buffer PROPERTIES
        LABELS "unit;phase-m6-invariant"
        TIMEOUT 60)
endif()

# ── Phase 7 Plan 07-01 Task 02 (HARD-13, HARD-20, D-01, D-08): supervisor
#    + parent-death Unity tests. Task 01 added test_crash_ring_buffer
#    above; Task 02 adds the three tests below. ──────────────────────────
if(IRON_BUILD_LSP)
    # Test 2: parent_watch prctl (Linux-only; skipped on non-Linux).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_parent_watch_prctl
            lsp/obs/test_parent_watch_prctl.c
            ${CMAKE_SOURCE_DIR}/src/lsp/obs/parent_watch.c
            ${CMAKE_SOURCE_DIR}/src/lsp/obs/log.c
            ${CMAKE_SOURCE_DIR}/src/lsp/transport/reader.c
            ${CMAKE_SOURCE_DIR}/src/lsp/transport/frame.c)
        target_include_directories(test_parent_watch_prctl PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/vendor
            ${CMAKE_BINARY_DIR}/generated)
        target_compile_definitions(test_parent_watch_prctl PRIVATE _GNU_SOURCE)
        target_link_libraries(test_parent_watch_prctl
            iron_compiler iron_runtime unity pthread m)
        add_test(NAME test_parent_watch_prctl COMMAND test_parent_watch_prctl)
        set_tests_properties(test_parent_watch_prctl PROPERTIES
            LABELS "unit;phase-m6-invariant"
            TIMEOUT 30)
    endif()

    # Test 3: supervisor backoff + 5-crash bailout (deterministic clock).
    add_executable(test_supervisor_backoff
        lsp/supervisor/test_supervisor_backoff.c
        ${CMAKE_SOURCE_DIR}/src/lsp/supervisor/supervisor.c)
    target_include_directories(test_supervisor_backoff PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_supervisor_backoff PRIVATE
        _GNU_SOURCE
        ILSP_SUPERVISOR_TESTING=1)
    target_link_libraries(test_supervisor_backoff
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_supervisor_backoff COMMAND test_supervisor_backoff)
    set_tests_properties(test_supervisor_backoff PROPERTIES
        LABELS "unit;phase-m6-invariant"
        TIMEOUT 30)

    # Test 4: supervisor proxy (byte-forwarder + synthetic showMessage).
    add_executable(test_supervisor_proxy
        lsp/supervisor/test_supervisor_proxy.c
        ${CMAKE_SOURCE_DIR}/src/lsp/supervisor/supervisor.c)
    target_include_directories(test_supervisor_proxy PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_supervisor_proxy PRIVATE
        _GNU_SOURCE
        ILSP_SUPERVISOR_TESTING=1)
    target_link_libraries(test_supervisor_proxy
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_supervisor_proxy COMMAND test_supervisor_proxy)
    set_tests_properties(test_supervisor_proxy PROPERTIES
        LABELS "unit;phase-m6-invariant"
        TIMEOUT 30)
endif()

# ── Phase 7 Plan 07-02 Task 01 (HARD-15, D-03): RSS measurement + cap
#    Unity test. Mocks /proc/self/status + task_info via the testing
#    hooks exposed from rss.h; exercises cap-trip branch by injecting
#    a synthetic RSS reading. Labelled phase-m6-invariant. ────────────
if(IRON_BUILD_LSP)
    add_executable(test_rss_current_bytes
        lsp/obs/test_rss_current_bytes.c
        ${CMAKE_SOURCE_DIR}/src/lsp/obs/rss.c)
    target_include_directories(test_rss_current_bytes PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(test_rss_current_bytes PRIVATE _GNU_SOURCE)
    target_link_libraries(test_rss_current_bytes
        iron_compiler iron_runtime unity pthread m)
    add_test(NAME test_rss_current_bytes COMMAND test_rss_current_bytes)
    set_tests_properties(test_rss_current_bytes PROPERTIES
        LABELS "unit;phase-m6-invariant"
        TIMEOUT 30)
endif()

# ── Phase 19 plan-01: heap-tracker SAFE-01/02/05 ─────────────────────────────
# Generational pointer infrastructure runtime substrate. Both tests link
# iron_runtime (where iron_heap_track.c lives) + unity. The double-free
# panic test inside test_runtime_heap_free is TEST_IGNORE'd until Plan 19-02
# ships iron_panic_stale_pointer's body.
add_executable(test_runtime_heap_alloc test_runtime_heap_alloc.c)
target_link_libraries(test_runtime_heap_alloc iron_runtime unity)
add_test(NAME test_runtime_heap_alloc COMMAND test_runtime_heap_alloc)
set_tests_properties(test_runtime_heap_alloc PROPERTIES LABELS "unit;phase19-invariant")

add_executable(test_runtime_heap_free test_runtime_heap_free.c)
target_link_libraries(test_runtime_heap_free iron_runtime unity)
add_test(NAME test_runtime_heap_free COMMAND test_runtime_heap_free)
set_tests_properties(test_runtime_heap_free PROPERTIES LABELS "unit;phase19-invariant")

# ── Phase 19 plan-02: panic mechanism SAFE-03/04/06 ─────────────────────────
# Stale-pointer panic format + env-cached output channel + release-build
# symbol retention. Fork-per-case panic-capture pattern (POSIX). The
# release-mode sibling test compiles the same source with -DNDEBUG=1 -O2 to
# verify SAFE-04: no `--release-skip-checks` flag exists; the deref check
# and panic must survive in release-built iron_runtime.
add_executable(test_runtime_panic_stale test_runtime_panic_stale.c)
target_link_libraries(test_runtime_panic_stale iron_runtime unity)
add_test(NAME test_runtime_panic_stale COMMAND test_runtime_panic_stale)
set_tests_properties(test_runtime_panic_stale PROPERTIES
    LABELS "unit;phase19-invariant"
    TIMEOUT 30
)

# SAFE-04 release-build symbol presence — same source, Release-mode build.
add_executable(test_runtime_panic_stale_release test_runtime_panic_stale.c)
target_link_libraries(test_runtime_panic_stale_release iron_runtime unity)
target_compile_definitions(test_runtime_panic_stale_release PRIVATE NDEBUG=1)
target_compile_options(test_runtime_panic_stale_release PRIVATE -O2)
add_test(NAME test_runtime_panic_stale_release COMMAND test_runtime_panic_stale_release)
set_tests_properties(test_runtime_panic_stale_release PROPERTIES
    LABELS "unit;phase19-invariant"
    TIMEOUT 30
)

# ── Phase 19 plan-03: thread-safety stress test SAFE-01/05 ───────────────────
# Counter atomicity (8 threads x 100k iterations alloc+free) + cross-thread
# free panic (fork-per-consumer SIGABRT capture) + cross-thread deref panic
# (fork-per-consumer SIGABRT capture). Mirrors the test_string_intern_race
# TSAN-gated pattern: -fsanitize=thread on Linux, FAIL_REGULAR_EXPRESSION
# converts any ThreadSanitizer warning into a test failure. POSIX-only;
# Win32 path emits TEST_IGNORE_MESSAGE.
add_executable(test_runtime_heap_concurrent test_runtime_heap_concurrent.c)
target_link_libraries(test_runtime_heap_concurrent iron_runtime unity)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(test_runtime_heap_concurrent PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_runtime_heap_concurrent PRIVATE -fsanitize=thread)
endif()
add_test(NAME test_runtime_heap_concurrent COMMAND test_runtime_heap_concurrent)
set_tests_properties(test_runtime_heap_concurrent PROPERTIES
    LABELS "unit;phase19-invariant;tsan"
    TIMEOUT 60
    FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer"
)

# ── Phase 20 plan-01 — checked pointer types: type-system surface ────────────
# PTR-01/11/12/13/14/15. Wave 0 RED: tests reference unallocated symbols
# (IRON_TYPE_PTR / iron_type_make_ptr / IRON_ERR_PTR_NO_ARITH /
#  Iron_TypeAnnotation.is_pointer/is_var_pointer/pointer_pointee). Build
# fails until Plan 20-01 lands the parser + analyzer + diagnostics changes.
add_executable(test_parser_ptr_type_annotation test_parser_ptr_type_annotation.c)
target_link_libraries(test_parser_ptr_type_annotation iron_compiler unity)
add_test(NAME test_parser_ptr_type_annotation COMMAND test_parser_ptr_type_annotation)
set_tests_properties(test_parser_ptr_type_annotation PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

add_executable(test_parser_amp_unary test_parser_amp_unary.c)
target_link_libraries(test_parser_amp_unary iron_compiler unity)
add_test(NAME test_parser_amp_unary COMMAND test_parser_amp_unary)
set_tests_properties(test_parser_amp_unary PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

add_executable(test_analyzer_ptr_distinct_types test_analyzer_ptr_distinct_types.c)
target_link_libraries(test_analyzer_ptr_distinct_types iron_compiler unity)
add_test(NAME test_analyzer_ptr_distinct_types COMMAND test_analyzer_ptr_distinct_types)
set_tests_properties(test_analyzer_ptr_distinct_types PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

add_executable(test_analyzer_ptr_nullability test_analyzer_ptr_nullability.c)
target_link_libraries(test_analyzer_ptr_nullability iron_compiler unity)
add_test(NAME test_analyzer_ptr_nullability COMMAND test_analyzer_ptr_nullability)
set_tests_properties(test_analyzer_ptr_nullability PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

# ── Phase 20 plan-02a — analyzer surface (PTR-06 / PTR-07 / PTR-10 compile) ──
# Wave 0 RED: tests reference unallocated symbols (Iron_FieldAccess.is_auto_deref,
# Iron_FuncDecl.takes_local_addr, iron_stack_gen, iron_check_stack_pointer_gen,
# IRON_HIR_EXPR_ADDR_OF, IRON_LIR_ADDR_OF). Build failed until Plan 20-02a Tasks
# 2-3 landed the analyzer surface; the two formerly-gated entries flipped
# GREEN once Plan 20-02b shipped HIR/LIR/codegen + runtime.
add_executable(test_analyzer_auto_deref_field test_analyzer_auto_deref_field.c)
target_link_libraries(test_analyzer_auto_deref_field iron_compiler unity)
add_test(NAME test_analyzer_auto_deref_field COMMAND test_analyzer_auto_deref_field)
set_tests_properties(test_analyzer_auto_deref_field PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

add_executable(test_analyzer_auto_address_call test_analyzer_auto_address_call.c)
target_link_libraries(test_analyzer_auto_address_call iron_compiler unity)
add_test(NAME test_analyzer_auto_address_call COMMAND test_analyzer_auto_address_call)
set_tests_properties(test_analyzer_auto_address_call PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

add_executable(test_analyzer_pointer_assignability test_analyzer_pointer_assignability.c)
target_link_libraries(test_analyzer_pointer_assignability iron_compiler unity)
add_test(NAME test_analyzer_pointer_assignability COMMAND test_analyzer_pointer_assignability)
set_tests_properties(test_analyzer_pointer_assignability PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

# Phase 20 Plan 02b — pending-LABEL dropped now that the HIR/LIR/codegen
# plumbing + runtime substrate (iron_stack_gen TLS slot,
# iron_check_stack_pointer_gen static-inline, iron_panic_stale_stack_pointer
# panic helper) have landed. These two tests now run under the default
# `ctest -L phase20-invariant` sweep alongside the other 7 Phase 20 entries
# (4 from 20-01 + 3 analyzer-pure from 20-02a) for a cumulative 9/9 GREEN.
add_executable(test_analyzer_addr_of_codegen test_analyzer_addr_of_codegen.c)
target_link_libraries(test_analyzer_addr_of_codegen iron_compiler unity)
add_test(NAME test_analyzer_addr_of_codegen COMMAND test_analyzer_addr_of_codegen)
set_tests_properties(test_analyzer_addr_of_codegen PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

add_executable(test_runtime_stack_pointer_dangling test_runtime_stack_pointer_dangling.c)
target_link_libraries(test_runtime_stack_pointer_dangling iron_runtime unity)
add_test(NAME test_runtime_stack_pointer_dangling COMMAND test_runtime_stack_pointer_dangling)
set_tests_properties(test_runtime_stack_pointer_dangling PROPERTIES
    LABELS "unit;phase20-invariant"
    TIMEOUT 30
)

# Phase 21 Plan 01 — Wave 0 RED -> GREEN
# POL-03 position locks + POL-04/05 identifier-only checks + DEFER-02 structural check
add_executable(test_heap_bad_position test_heap_bad_position.c)
target_link_libraries(test_heap_bad_position iron_compiler unity)
add_test(NAME test_heap_bad_position COMMAND test_heap_bad_position)
set_tests_properties(test_heap_bad_position PROPERTIES LABELS "unit;phase21-invariant" TIMEOUT 30)

add_executable(test_free_leak_target_check test_free_leak_target_check.c)
target_link_libraries(test_free_leak_target_check iron_compiler unity)
add_test(NAME test_free_leak_target_check COMMAND test_free_leak_target_check)
set_tests_properties(test_free_leak_target_check PROPERTIES LABELS "unit;phase21-invariant" TIMEOUT 30)

add_executable(test_defer_form_unsupported test_defer_form_unsupported.c)
target_link_libraries(test_defer_form_unsupported iron_compiler unity)
add_test(NAME test_defer_form_unsupported COMMAND test_defer_form_unsupported)
set_tests_properties(test_defer_form_unsupported PROPERTIES LABELS "unit;phase21-invariant" TIMEOUT 30)

# Phase 21 Plan 02 — Wave 0 RED -> GREEN codegen migration + is_leaked
add_executable(test_heap_alloc_codegen test_heap_alloc_codegen.c)
target_link_libraries(test_heap_alloc_codegen iron_compiler unity)
add_test(NAME test_heap_alloc_codegen COMMAND test_heap_alloc_codegen)
set_tests_properties(test_heap_alloc_codegen PROPERTIES LABELS "unit;phase21-invariant" TIMEOUT 30)

add_executable(test_leak_symbol_flag test_leak_symbol_flag.c)
target_link_libraries(test_leak_symbol_flag iron_compiler unity)
add_test(NAME test_leak_symbol_flag COMMAND test_leak_symbol_flag)
set_tests_properties(test_leak_symbol_flag PROPERTIES LABELS "unit;phase21-invariant" TIMEOUT 30)

# Phase 21 Plan 03 — OQ-09 mechanical gate (B2 closure 2026-05-08)
# Verifies heap T(...) compiles cleanly AND rc T(...) is rejected (closed-
# policy enforcement), and that the asymmetry holds within a single function.
add_executable(test_oq09_no_promotion test_oq09_no_promotion.c)
target_link_libraries(test_oq09_no_promotion iron_compiler unity)
add_test(NAME test_oq09_no_promotion COMMAND test_oq09_no_promotion)
set_tests_properties(test_oq09_no_promotion PROPERTIES LABELS "unit;phase21-invariant" TIMEOUT 30)

# Phase 22 Plan 01 — Wave 0 RED -> GREEN (READ-02/04/05 body-violation + READ-09 §6 hints)
add_executable(test_readonly_param_mutation test_readonly_param_mutation.c)
target_link_libraries(test_readonly_param_mutation iron_compiler unity)
add_test(NAME test_readonly_param_mutation COMMAND test_readonly_param_mutation)
set_tests_properties(test_readonly_param_mutation PROPERTIES LABELS "unit;phase22-invariant" TIMEOUT 30)

add_executable(test_readonly_io test_readonly_io.c)
target_link_libraries(test_readonly_io iron_compiler unity)
add_test(NAME test_readonly_io COMMAND test_readonly_io)
set_tests_properties(test_readonly_io PROPERTIES LABELS "unit;phase22-invariant" TIMEOUT 30)

add_executable(test_readonly_heap_escape test_readonly_heap_escape.c)
target_link_libraries(test_readonly_heap_escape iron_compiler unity)
add_test(NAME test_readonly_heap_escape COMMAND test_readonly_heap_escape)
set_tests_properties(test_readonly_heap_escape PROPERTIES LABELS "unit;phase22-invariant" TIMEOUT 30)

add_executable(test_readonly_body test_readonly_body.c)
target_link_libraries(test_readonly_body iron_compiler unity)
add_test(NAME test_readonly_body COMMAND test_readonly_body)
set_tests_properties(test_readonly_body PROPERTIES LABELS "unit;phase22-invariant" TIMEOUT 30)

# Phase 22 Plan 02 — Wave 0 RED -> GREEN (READ-06 return-type whitelist + OQ-04 closure capture-purity)
add_executable(test_readonly_return_type test_readonly_return_type.c)
target_link_libraries(test_readonly_return_type iron_compiler unity)
add_test(NAME test_readonly_return_type COMMAND test_readonly_return_type)
set_tests_properties(test_readonly_return_type PROPERTIES LABELS "unit;phase22-invariant" TIMEOUT 30)

add_executable(test_oq04_closure_readonly test_oq04_closure_readonly.c)
target_link_libraries(test_oq04_closure_readonly iron_compiler unity)
add_test(NAME test_oq04_closure_readonly COMMAND test_oq04_closure_readonly)
set_tests_properties(test_oq04_closure_readonly PROPERTIES LABELS "unit;phase22-invariant" TIMEOUT 30)

# Phase 23 VEC: bounded vector [T; <=N] type-level surface (Plan 23-01)
add_executable(test_bvec_type_assignability test_bvec_type_assignability.c)
target_link_libraries(test_bvec_type_assignability iron_compiler unity)
add_test(NAME test_bvec_type_assignability COMMAND test_bvec_type_assignability)
set_tests_properties(test_bvec_type_assignability PROPERTIES LABELS "unit;phase23-invariant" TIMEOUT 30)

add_executable(test_bvec_strict_mismatch test_bvec_strict_mismatch.c)
target_link_libraries(test_bvec_strict_mismatch iron_compiler unity)
add_test(NAME test_bvec_strict_mismatch COMMAND test_bvec_strict_mismatch)
set_tests_properties(test_bvec_strict_mismatch PROPERTIES LABELS "unit;phase23-invariant" TIMEOUT 30)

# Phase 23 VEC: bounded vector codegen + runtime bounds-check (Plan 23-02)
add_executable(test_bvec_basic test_bvec_basic.c)
target_link_libraries(test_bvec_basic iron_compiler iron_runtime unity)
add_test(NAME test_bvec_basic COMMAND test_bvec_basic)
set_tests_properties(test_bvec_basic PROPERTIES LABELS "unit;phase23-invariant" TIMEOUT 30)

add_executable(test_bvec_oob_panic test_bvec_oob_panic.c)
target_link_libraries(test_bvec_oob_panic iron_compiler iron_runtime unity)
add_test(NAME test_bvec_oob_panic COMMAND test_bvec_oob_panic)
set_tests_properties(test_bvec_oob_panic PROPERTIES LABELS "unit;phase23-invariant" TIMEOUT 30)

# ── Phase 26 POL-06: rc policy refcount substrate (Plan 26-01) ───────────────
# Wave 0 RED → GREEN after Task 2 lands the iron_rc_alloc / iron_rc_retain /
# iron_rc_release implementation on Iron_RcHeader.
#
# test_rc_layout — ABI layout invariants (sizeof + offsetof) + alloc/release
#                  smoke. Locks the 16B header layout at runtime in addition
#                  to the _Static_assert at TU scope.
# test_rc_atomic_ordering — single-thread atomic ordering proof (retain
#                  monotonic, drop_fn invoked exactly once on final release,
#                  NULL drop_fn skipped cleanly).
# test_runtime_rc_concurrent — Linux+TSan 8-thread × 100k retain/release
#                  stress; SKIPs cleanly on macOS / non-TSan Linux.
add_executable(test_rc_layout test_rc_layout.c)
target_link_libraries(test_rc_layout iron_runtime unity)
add_test(NAME test_rc_layout COMMAND test_rc_layout)
set_tests_properties(test_rc_layout PROPERTIES
    LABELS "unit;phase26-invariant"
    TIMEOUT 30)

add_executable(test_rc_atomic_ordering test_rc_atomic_ordering.c)
target_link_libraries(test_rc_atomic_ordering iron_runtime unity)
add_test(NAME test_rc_atomic_ordering COMMAND test_rc_atomic_ordering)
set_tests_properties(test_rc_atomic_ordering PROPERTIES
    LABELS "unit;phase26-invariant"
    TIMEOUT 30)

add_executable(test_runtime_rc_concurrent test_runtime_rc_concurrent.c)
target_link_libraries(test_runtime_rc_concurrent iron_runtime unity)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(test_runtime_rc_concurrent PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_runtime_rc_concurrent PRIVATE -fsanitize=thread)
endif()
add_test(NAME test_runtime_rc_concurrent COMMAND test_runtime_rc_concurrent)
set_tests_properties(test_runtime_rc_concurrent PROPERTIES
    LABELS "unit;phase26-invariant;tsan"
    TIMEOUT 60
    FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer")

# ── Phase 26 POL-06 (Plan 26-03 Task 1): rc drop chain order codegen test ───
# Verifies emit_ensure_rc_drop trampoline synthesis + IRON_LIR_RC_ALLOC drop_fn
# wiring: type with user drop gets <TypeName>_rc_drop trampoline pointer, type
# without drop need passes NULL. Static dispatch (GA5 lock); no vtable.
add_executable(test_rc_drop_chain_order test_rc_drop_chain_order.c)
target_link_libraries(test_rc_drop_chain_order PRIVATE iron_compiler iron_runtime unity m pthread)
target_include_directories(test_rc_drop_chain_order PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/vendor)
add_test(NAME test_rc_drop_chain_order COMMAND test_rc_drop_chain_order)
set_tests_properties(test_rc_drop_chain_order PROPERTIES
    LABELS "unit;phase26-invariant"
    TIMEOUT 30)

# ── Phase 26 OQ-03 (Plan 26-03 Task 2): closure capture retain/release ──
# Verifies hir_to_lir.c IRON_HIR_EXPR_CLOSURE arm emits IRON_LIR_RC_RETAIN
# per rc-typed val capture AND emit_c.c IRON_LIR_MAKE_CLOSURE arm synthesizes
# <func_name>_env_drop companion releasing each rc env field. Major #5
# hardening: BOTH count(retain) >= 1 minimum AND count(retain) == count(release)
# 1:1 balance assertions.
add_executable(test_oq03_closure_rc_retain_release test_oq03_closure_rc_retain_release.c)
target_link_libraries(test_oq03_closure_rc_retain_release PRIVATE iron_compiler iron_runtime unity m pthread)
target_include_directories(test_oq03_closure_rc_retain_release PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/vendor)
add_test(NAME test_oq03_closure_rc_retain_release COMMAND test_oq03_closure_rc_retain_release)
set_tests_properties(test_oq03_closure_rc_retain_release PROPERTIES
    LABELS "unit;phase26-invariant"
    TIMEOUT 30)

# ── Phase 27 GA1 (Plan 27-01 Task 1): weak rc 24B header layout invariants ──
# test_weak_rc_layout — Wave 0 RED for Plan 27-01 Task 1; flips GREEN when
# Iron_RcHeader grows from 16B → 24B with weak_count at offset 16, and
# iron_rc_alloc zero-initialises the field per CONTEXT.md GA1 Pitfall 4.
add_executable(test_weak_rc_layout test_weak_rc_layout.c)
target_link_libraries(test_weak_rc_layout iron_runtime unity)
add_test(NAME test_weak_rc_layout COMMAND test_weak_rc_layout)
set_tests_properties(test_weak_rc_layout PROPERTIES
    LABELS "unit;phase27-invariant"
    TIMEOUT 30)

# ── Phase 27 GA1 (Plan 27-01 Task 2): weak rc atomic ordering invariants ───
# Exercises iron_weak_rc_retain/release + the extended iron_rc_release
# block-free guard. Single-thread proof of the deferred-free path: strong=0
# with weak alive defers block free; weak=0 with strong alive does NOT free;
# weak=0 with strong=0 completes the deferred free.
add_executable(test_weak_rc_atomic_ordering test_weak_rc_atomic_ordering.c)
target_link_libraries(test_weak_rc_atomic_ordering iron_runtime unity)
add_test(NAME test_weak_rc_atomic_ordering COMMAND test_weak_rc_atomic_ordering)
set_tests_properties(test_weak_rc_atomic_ordering PROPERTIES
    LABELS "unit;phase27-invariant"
    TIMEOUT 30)

# ── Phase 27 GA1 (Plan 27-01 Task 2): weak rc TSan concurrent stress ───────
# 8 threads × 100k iterations interleaving strong-retain/strong-release with
# weak-retain/weak-release; final counters must return to baseline. TSan-only.
# Known issue inheritance (Phase 26 Plan 03 SUMMARY #1): podman Rocky 9 may
# need libtsan.so symlink for TSan-instrumented binaries to run. SKIP on the
# non-TSan path keeps the phase27-invariant gate green.
add_executable(test_runtime_weak_rc_concurrent test_runtime_weak_rc_concurrent.c)
target_link_libraries(test_runtime_weak_rc_concurrent iron_runtime unity)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(test_runtime_weak_rc_concurrent PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_runtime_weak_rc_concurrent PRIVATE -fsanitize=thread)
endif()
add_test(NAME test_runtime_weak_rc_concurrent COMMAND test_runtime_weak_rc_concurrent)
set_tests_properties(test_runtime_weak_rc_concurrent PROPERTIES
    LABELS "unit;phase27-invariant;tsan"
    TIMEOUT 60
    FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer"
    SKIP_REGULAR_EXPRESSION "BAD_COMMAND")
