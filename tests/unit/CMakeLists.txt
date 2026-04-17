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

# ── Runtime unit tests ───────────────────────────────────────────────────────
add_executable(test_runtime_string test_runtime_string.c)
target_link_libraries(test_runtime_string iron_runtime unity)
add_test(NAME test_runtime_string COMMAND test_runtime_string)
set_tests_properties(test_runtime_string PROPERTIES LABELS "unit")

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
)
target_include_directories(test_toml_web PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_toml_web unity)
add_test(NAME test_toml_web COMMAND test_toml_web)
set_tests_properties(test_toml_web PROPERTIES LABELS "unit")

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
