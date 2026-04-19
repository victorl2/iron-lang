cmake_minimum_required(VERSION 3.25)
project(iron VERSION 3.1.1 LANGUAGES C)

# Full release version, including any pre-release suffix (e.g. "1.0.0-alpha").
# CMake's project(VERSION ...) only accepts numeric MAJOR.MINOR.PATCH[.TWEAK],
# so the suffixed form lives in its own variable and is baked into the binaries
# as IRON_VERSION_STRING below. Both this line and the project() line above are
# kept in sync with the latest GitHub release by .github/workflows/changelog-on-release.yml.
set(IRON_VERSION_FULL "3.1.1-alpha")

# ── Version info baked at compile time ─────────────────────────────────────
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE IRON_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT IRON_GIT_HASH)
    set(IRON_GIT_HASH "unknown")
endif()

string(TIMESTAMP IRON_BUILD_DATE "%Y-%m-%d" UTC)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(IRON_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer (requires Debug build)" OFF)
option(IRON_ENABLE_FUZZING "Build libFuzzer fuzz targets (requires clang)" OFF)
option(IRON_ENABLE_COVERAGE "Build with llvm-cov instrumentation (requires clang)" OFF)

# Three-way mutex: at most one of SANITIZERS / FUZZING / COVERAGE may be ON.
# llvm-cov and libFuzzer profile formats conflict; sanitizer instrumentation
# distorts coverage line counts and fuzzer coverage metrics. Use separate
# build directories for each purpose. Evaluated BEFORE the per-option clang
# checks below so the fail-fast happens regardless of compiler detection.
if(IRON_ENABLE_COVERAGE AND IRON_ENABLE_FUZZING)
    message(FATAL_ERROR
        "IRON_ENABLE_COVERAGE and IRON_ENABLE_FUZZING are mutually exclusive "
        "(llvm-cov profile format conflicts with libFuzzer's coverage "
        "instrumentation). Use separate build directories.")
endif()
if(IRON_ENABLE_COVERAGE AND IRON_ENABLE_SANITIZERS)
    message(FATAL_ERROR
        "IRON_ENABLE_COVERAGE and IRON_ENABLE_SANITIZERS are mutually exclusive "
        "(ASan/UBSan instrumentation distorts llvm-cov line/branch counts). "
        "Use separate build directories.")
endif()
if(IRON_ENABLE_FUZZING AND IRON_ENABLE_SANITIZERS)
    message(FATAL_ERROR
        "IRON_ENABLE_FUZZING and IRON_ENABLE_SANITIZERS are mutually exclusive "
        "(libFuzzer ships its own sanitizer-equivalent instrumentation via "
        "-fsanitize=fuzzer,address,undefined). Use separate build directories.")
endif()

if(MSVC)
    add_compile_options(/W3 /WX)
    # Silence POSIX-deprecation warnings (C4996) for getenv, strdup, strerror,
    # strcpy, getcwd, etc. — these are used in cross-platform code in src/pkg/
    # and src/cli/toml.c and are already bounded-safe at the call site. Without
    # this define the /WX flag turns C4996 into an error and blocks the MSVC
    # build. Scoped to MSVC only so clang/gcc strictness is unchanged.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic -Werror=switch-enum)
    # GCC's -Wtype-limits / -Wunused-value fire inside stb_ds.h macro expansions
    # (e.g. `stbds_arrcap(a) < (size_t)(n)` when n is a literal 0). Clang does
    # not warn on these. Scope-disable to unblock the Linux GCC build without
    # relaxing real-code warnings.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_compile_options(
            -Wno-type-limits -Wno-unused-value -Wno-format-truncation
            -Wno-unused-but-set-variable
        )
    endif()
    if(IRON_ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
    if(IRON_ENABLE_FUZZING)
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR
                "IRON_ENABLE_FUZZING=ON requires clang (-fsanitize=fuzzer is a "
                "clang-only feature). Re-configure with -DCMAKE_C_COMPILER=clang. "
                "Linux: sudo apt-get install clang. macOS: clang ships with Xcode CLT.")
        endif()
    endif()
    if(IRON_ENABLE_COVERAGE)
        # Accept both "Clang" (upstream/Linux) and "AppleClang" (macOS Xcode CLT)
        # — both ship the -fprofile-instr-generate / -fcoverage-mapping pair and
        # llvm-profdata/llvm-cov tooling. GCC's gcov format is intentionally
        # rejected: llvm-cov cannot consume it.
        if(NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang"))
            message(FATAL_ERROR
                "IRON_ENABLE_COVERAGE=ON requires clang "
                "(-fprofile-instr-generate -fcoverage-mapping are clang-only). "
                "Re-configure with -DCMAKE_C_COMPILER=clang. "
                "Linux: sudo apt-get install clang. macOS: clang ships with Xcode CLT.")
        endif()
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
        message(STATUS "Iron: coverage instrumentation ON (clang llvm-cov)")
    endif()
endif()

# ── Unity test framework ────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.1
)
FetchContent_MakeAvailable(unity)
# Enable double-precision float assertions for all Unity test targets
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)
# Unity is a third-party library — don't apply our /W3 /WX strictness to it.
# On MSVC, Unity's own source emits C4996 (POSIX deprecation) and C5045
# (Spectre mitigation hints) which our global /WX would promote to errors.
# Drop the strictness locally for the Unity target only.
if(MSVC)
    target_compile_options(unity PRIVATE /W0 /wd4996 /wd5045)
endif()

# ── iron_runtime static library ────────────────────────────────────────────
# stb_ds_impl.c lives here so runtime tests can link without iron_compiler.
add_library(iron_runtime STATIC
    src/util/stb_ds_impl.c
    src/runtime/iron_string.c
    src/runtime/iron_fmt.c
    src/runtime/iron_rc.c
    src/runtime/iron_builtins.c
    src/runtime/iron_threads.c
    src/runtime/iron_collections.c
    src/runtime/iron_net_init.c
    src/runtime/iron_oom.c
)
target_include_directories(iron_runtime PUBLIC src src/vendor)
if(NOT WIN32)
  target_link_libraries(iron_runtime pthread)
else()
  # ws2_32 needed for WSAStartup/WSACleanup/socket() in iron_net_init.c
  target_link_libraries(iron_runtime ws2_32)
endif()

# ── iron_compiler static library ───────────────────────────────────────────
# NOTE: More source files will be added in later plans.
add_library(iron_compiler STATIC
    src/util/arena.c
    src/util/strbuf.c
    src/diagnostics/diagnostics.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/parser/printer.c
    src/analyzer/types.c
    src/analyzer/scope.c
    src/analyzer/resolve.c
    src/analyzer/typecheck.c
    src/analyzer/typo_candidate.c
    src/analyzer/escape.c
    src/analyzer/capture.c
    src/analyzer/concurrency.c
    src/analyzer/web_await_check.c
    src/analyzer/web_top_level_loader_check.c
    src/analyzer/init_check.c
    src/analyzer/iface_collect.c
    src/analyzer/analyzer.c
    src/comptime/comptime.c
    src/hir/hir.c
    src/hir/hir_print.c
    src/hir/hir_verify.c
    src/hir/hir_lower.c
    src/hir/hir_to_lir.c
    src/lir/lir.c
    src/lir/emit_c.c
    src/lir/emit_web.c
    src/lir/emit_helpers.c
    src/lir/emit_structs.c
    src/lir/emit_split.c
    src/lir/emit_fusion.c
    src/lir/layout_analysis.c
    src/lir/value_range.c
    src/lir/print.c
    src/lir/verify.c
    src/lir/lir_optimize.c
    src/lir/web_main_loop_split.c
)
target_include_directories(iron_compiler PUBLIC src src/vendor)
target_link_libraries(iron_compiler iron_runtime)

# ── iron_stdlib static library ─────────────────────────────────────────────
add_library(iron_stdlib STATIC
    src/stdlib/iron_math.c
    src/stdlib/iron_io.c
    src/stdlib/iron_time.c
    src/stdlib/iron_log.c
    src/stdlib/iron_hint.c
    src/stdlib/iron_net.c
)
target_include_directories(iron_stdlib PUBLIC src)
if(NOT WIN32)
  target_link_libraries(iron_stdlib iron_runtime m)  # -lm for math
else()
  # ws2_32 + iphlpapi for Winsock 2 + GetAdaptersAddresses (future Phase 59 UDP/DNS)
  target_link_libraries(iron_stdlib iron_runtime ws2_32 iphlpapi)
endif()

# ── Fuzz targets (opt-in) ──────────────────────────────────────────────────
# Gated behind -DIRON_ENABLE_FUZZING=ON to keep normal developer builds
# hermetic. Not part of ctest — fuzz binaries are invoked directly from
# .github/workflows/fuzz.yml (see Phase 68 CONTEXT.md).
if(IRON_ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────
enable_testing()

add_subdirectory(tests/unit)
add_subdirectory(tests/lir)
add_subdirectory(tests/hir)
add_subdirectory(tests/lsp)                 # Phase 1 Plan 05: parity harness
add_subdirectory(tests/integration)
add_subdirectory(tests/algorithms)
add_subdirectory(tests/composite)
add_subdirectory(tests/manual)

# ── ironc compiler executable ────────────────────────────────────────────────
add_executable(ironc
    src/cli/main.c
    src/cli/build.c
    src/cli/build_web.c
    src/cli/check.c
    src/cli/fmt.c
    src/cli/test_runner.c
    src/cli/toml.c
    src/cli/iron_import_detect.c
)
target_include_directories(ironc PRIVATE src)
if(NOT WIN32)
  target_link_libraries(ironc iron_compiler iron_runtime iron_stdlib m pthread)
else()
  target_link_libraries(ironc iron_compiler iron_runtime iron_stdlib ws2_32 iphlpapi)
endif()
# Inject the absolute path to src/ so the CLI can find runtime sources at build time
target_compile_definitions(ironc PRIVATE
    IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
    IRON_VERSION_STRING="${IRON_VERSION_FULL}"
    IRON_GIT_HASH="${IRON_GIT_HASH}"
    IRON_BUILD_DATE="${IRON_BUILD_DATE}"
    IRON_BINARY_NAME="ironc"
)

# ── ironls language server executable (Phase 2 M1 LSP Core) ──────────────────
# Second binary next to ironc/iron. Links iron_compiler as a PUBLIC static
# lib so the LSP analyzer output never diverges from the compiler's. Platform
# posture follows the compiler: Linux + macOS in v1; Windows is hard-failed
# with a friendly FATAL_ERROR until the compiler's MSVC issues clear.
option(IRON_BUILD_LSP "Build ironls (Iron language server)" ON)

if(IRON_BUILD_LSP AND WIN32)
    message(FATAL_ERROR
        "ironls is not supported on Windows in v1 -- re-enable when the "
        "compiler's Windows build clears the existing MSVC issues. Configure "
        "with -DIRON_BUILD_LSP=OFF to skip the LSP binary.")
endif()

# ── Phase 4 Plan 04-02 Task 01 (EDIT-06): keyword mirror header ─────────────
# Extract the `kw_table` keyword strings from src/lexer/lexer.c at configure
# time and materialise them as a static const char *[] array in the
# generated keyword_mirror.h header consumed by the completion engine
# (src/lsp/facade/edit/complete/*). The drift-guard Unity test
# tests/unit/test_completion_keyword_mirror.c asserts byte-for-byte parity
# with the lexer's single-token API at run time. PITFALL H in 04-PATTERNS.md.
#
# Strategy: file(STRINGS) reads every line of lexer.c that looks like a
# kw_table entry ("kw", IRON_TOK_...), we regex-capture the quoted keyword
# and emit a comma-separated, C-quoted list into IRON_KEYWORD_LIST which
# feeds configure_file. No helper script — the CMake graph already depends
# on lexer.c via iron_compiler, and configure_file re-runs whenever any of
# its inputs (including lexer.c) change.
if(IRON_BUILD_LSP)
    set(_kw_table_src "${CMAKE_SOURCE_DIR}/src/lexer/lexer.c")
    file(STRINGS "${_kw_table_src}" _kw_lines
         REGEX "^[ \t]*\\{[ \t]*\"[a-zA-Z_]+\"[ \t]*,[ \t]*IRON_TOK_")
    set(IRON_KEYWORD_LIST "")
    foreach(_line IN LISTS _kw_lines)
        string(REGEX MATCH "\"([a-zA-Z_]+)\"" _m "${_line}")
        if(_m)
            set(_kw "${CMAKE_MATCH_1}")
            set(IRON_KEYWORD_LIST "${IRON_KEYWORD_LIST}    \"${_kw}\",\n")
        endif()
    endforeach()
    if(IRON_KEYWORD_LIST STREQUAL "")
        message(FATAL_ERROR
            "Phase 4 Plan 04-02 Task 01: failed to extract Iron keyword list "
            "from ${_kw_table_src} -- regex drift? The completion engine "
            "cannot build without the generated keyword_mirror.h header.")
    endif()
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/lsp/facade/edit/complete/keyword_mirror.h.in"
        "${CMAKE_BINARY_DIR}/generated/keyword_mirror.h"
        @ONLY)
    message(STATUS "Phase 4 Plan 04-02: keyword_mirror.h generated")
endif()

if(IRON_BUILD_LSP)
    add_executable(ironls
        src/lsp/main.c
        src/lsp/transport/frame.c
        src/lsp/transport/json.c
        src/lsp/transport/reader.c
        src/lsp/transport/writer.c
        # Phase 2 Plan 03: server subsystem (dispatcher, lifecycle FSM,
        # capabilities, dynamic registration, cancellation, lifecycle handlers).
        src/lsp/server/dispatch.c
        src/lsp/server/lifecycle.c
        src/lsp/server/capabilities.c
        src/lsp/server/dyn_register.c
        src/lsp/server/cancel.c
        src/lsp/server/handlers_lifecycle.c
        # Phase 2 Plan 04: document store (per-doc buffer, line_starts,
        # UTF math, SHA-256 drift detection, workspace root discovery,
        # didOpen/didChange/didClose/didSave/didChangeWatchedFiles).
        src/lsp/store/document.c
        src/lsp/store/line_index.c
        src/lsp/store/utf.c
        src/lsp/store/sha256.c
        src/lsp/store/workspace.c
        src/lsp/server/handlers_document.c
        # Phase 2 Plan 05: compilation pipeline (per-doc ASTWorker +
        # coalescing mailbox + 250ms debounce + SIGABRT boundary +
        # window/showMessage notifications + facade single call site).
        src/lsp/workers/mailbox.c
        src/lsp/workers/ast_worker.c
        src/lsp/obs/abort_handler.c
        src/lsp/server/notifications.c
        src/lsp/facade/compile.c
        src/lsp/facade/span.c
        src/lsp/facade/diagnostics.c
        # Phase 3 Plan 01: nav foundations (NAV-16). symbol_id derives
        # the (canonical_path, name_path, kind) identity triple used by
        # every hover/goto endpoint; node_at resolves LSP Position ->
        # AST Iron_Node by walking the sealed Iron_Program.
        src/lsp/facade/nav/symbol_id.c
        src/lsp/facade/nav/node_at.c
        # Phase 3 Plan 03 Task 01 (D-11): fzy-algorithm fuzzy scorer
        # shared by workspace/symbol + future completion ranking.
        src/lsp/facade/nav/fuzzy.c
        # Phase 3 Plan 03 Task 02/03: nav endpoints (definition,
        # declaration, typeDefinition, documentSymbol, workspaceSymbol)
        # and the shared nav_common helpers. handlers_nav.c is the
        # handler-table seam; it delegates to these facade TUs per the
        # Handler -> Facade -> iron_compiler hard rule.
        src/lsp/facade/nav/nav_common.c
        src/lsp/facade/nav/definition.c
        src/lsp/facade/nav/declaration.c
        src/lsp/facade/nav/type_definition.c
        src/lsp/facade/nav/document_symbol.c
        src/lsp/facade/nav/workspace_symbol.c
        # Phase 3 Plan 04 Task 01/02/03: references + hover +
        # signatureHelp facade TUs + reverse-ref index.
        src/lsp/facade/nav/references_index.c
        src/lsp/facade/nav/references.c
        src/lsp/facade/hover.c
        src/lsp/facade/signature_help.c
        # Phase 3 Plan 05 Task 01/02: iface workspace aggregator +
        # implementation + typeHierarchy facade TUs.
        src/lsp/facade/nav/iface_workspace.c
        src/lsp/facade/nav/implementation.c
        src/lsp/facade/nav/type_hierarchy.c
        src/lsp/server/handlers_nav.c
        # Phase 3 Plan 06 Task 01 (NAV-12, NAV-13, D-12): workspace/diagnostic
        # facade + pull handler + refresh push. Cache is keyed on
        # canonical_path; facade is a HandlerBridgeFacade delegate preserving
        # the single iron_analyze_buffer call site (CORE-22).
        src/lsp/facade/workspace_diagnostic.c
        src/lsp/server/handlers_workspace_diag.c
        # Phase 4 Plan 04-02 (EDIT-01, EDIT-02, EDIT-03, EDIT-06): completion
        # engine facade + handlers + capability shape override. Consumes the
        # configure_file-generated keyword_mirror.h for Bucket 6 (keywords).
        src/lsp/facade/edit/complete/context_classify.c
        src/lsp/facade/edit/complete/buckets.c
        src/lsp/facade/edit/complete/complete.c
        src/lsp/facade/edit/complete/resolve.c
        # Phase 4 Plan 04-03 (EDIT-04, EDIT-05, D-02, D-15):
        # auto-import additionalTextEdits builder (Task 01) + snippet
        # renderer (Task 02). Wired into complete.c's orchestrator
        # by Task 03; client snippetSupport cap gating lives in
        # handlers_edit.c + handlers_lifecycle.c.
        src/lsp/facade/edit/complete/auto_import.c
        src/lsp/facade/edit/complete/snippet.c
        # Phase 4 Plan 04-04 (EDIT-07, EDIT-08, D-06, D-07): code-action
        # registry + 5 P1 quickfix handler TUs. handlers_edit.c hosts the
        # textDocument/codeAction + codeAction/resolve handlers that
        # delegate to the registry + per-code handlers below.
        src/lsp/facade/edit/codeaction/registry.c
        src/lsp/facade/edit/codeaction/quickfix_undefined_var.c
        src/lsp/facade/edit/codeaction/quickfix_unused_import.c
        src/lsp/facade/edit/codeaction/quickfix_missing_return.c
        src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c
        src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c
        # Plan 04-04 Task 02: code-action orchestrator + codeAction/resolve
        # facade (lazy edit compute, version-gated per D-07).
        src/lsp/facade/edit/codeaction/codeaction.c
        src/lsp/facade/edit/codeaction/resolve.c
        # Plan 04-05 Task 01 (EDIT-09, D-08): source.organizeImports facade.
        # Classifies + sorts + dedups + optionally unused-removes the leading
        # Iron_ImportDecl run and emits a single TextEdit replacing its span.
        src/lsp/facade/edit/codeaction/organize_imports.c
        src/lsp/server/handlers_edit.c
        # Phase 3 Plan 02: workspace index + stdlib cache + dep map.
        # workspace_index.c owns per-file arena-scoped IndexEntry map;
        # stdlib_cache.c is the process-lifetime pre-parsed surface-file
        # cache (pthread_once-guarded); dep_map.c wraps the library-linked
        # src/pkg/resolver.c.
        src/lsp/store/workspace_index.c
        src/lsp/store/stdlib_cache.c
        src/lsp/store/dep_map.c
        # Phase 3 Plan 02 Task 03: dep_map.c links src/pkg/resolver.c as
        # library code (no subprocess).  Pull in the resolver's transitive
        # dependencies (fetcher, lockfile, toml) so ironls can resolve
        # iron.toml deps in-process.
        src/pkg/resolver.c
        src/pkg/fetcher.c
        src/pkg/lockfile.c
        src/cli/toml.c
        # Phase 2 Plan 06: observability (XDG JSON-line log sink,
        # in-memory trace histogram) used by main.c + transport threads.
        src/lsp/obs/log.c
        src/lsp/obs/trace.c
        src/vendor/yyjson/yyjson.c
    )
    target_include_directories(ironls PRIVATE
        src
        src/vendor
        "${CMAKE_BINARY_DIR}/generated"   # Plan 04-02: keyword_mirror.h
    )
    target_link_libraries(ironls iron_compiler iron_runtime iron_stdlib m pthread)
    target_compile_definitions(ironls PRIVATE
        IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
        IRON_VERSION_STRING="${IRON_VERSION_FULL}"
        IRON_GIT_HASH="${IRON_GIT_HASH}"
        IRON_BUILD_DATE="${IRON_BUILD_DATE}"
        IRON_BINARY_NAME="ironls"
    )
    # yyjson is a third-party vendored C library; silence the specific
    # warnings it emits under our -Wall -Wextra -Werror -Wpedantic discipline.
    # The drop-in is reviewed at vendoring time (see Plan 02-01 SUMMARY); we
    # do not want it to hold back our own -Werror posture.
    set_source_files_properties(src/vendor/yyjson/yyjson.c PROPERTIES
        COMPILE_FLAGS "-Wno-error -w")
endif()

# ── iron package manager executable ─────────────────────────────────────────
# iron does NOT link iron_compiler or iron_runtime -- clean process boundary.
# It only spawns ironc as a subprocess.
add_executable(iron
    src/pkg/main.c
    src/pkg/init.c
    src/pkg/pkg_build.c
    src/pkg/fetcher.c
    src/pkg/resolver.c
    src/pkg/lockfile.c
    src/cli/toml.c
)
target_include_directories(iron PRIVATE src)
target_compile_definitions(iron PRIVATE
    IRON_VERSION_STRING="${IRON_VERSION_FULL}"
    IRON_GIT_HASH="${IRON_GIT_HASH}"
    IRON_BUILD_DATE="${IRON_BUILD_DATE}"
)

# ── CLI smoke tests ──────────────────────────────────────────────────────────
add_test(
    NAME test_ironc_help
    COMMAND ironc
)
set_tests_properties(test_ironc_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: ironc"
)

add_test(
    NAME test_ironc_version
    COMMAND ironc --version
)
set_tests_properties(test_ironc_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^ironc [0-9]"
)

# Phase 2 — CLI flag surface smoke tests

add_test(
    NAME test_ironc_usage_lists_target
    COMMAND ironc
)
set_tests_properties(test_ironc_usage_lists_target PROPERTIES
    PASS_REGULAR_EXPRESSION "--target=<t>"
)

add_test(
    NAME test_ironc_usage_lists_release
    COMMAND ironc
)
set_tests_properties(test_ironc_usage_lists_release PROPERTIES
    PASS_REGULAR_EXPRESSION "--release"
)

if(NOT WIN32)
    add_test(
        NAME test_ironc_target_unknown
        COMMAND sh -c "echo 'func main() {}' > /tmp/iron_test_target_unk.iron && OUT=$($<TARGET_FILE:ironc> build --target=bogus /tmp/iron_test_target_unk.iron 2>&1); RC=$?; if [ $RC -eq 0 ]; then echo 'FAIL: ironc should have exited non-zero'; exit 1; fi; echo \"$OUT\" | grep -q 'valid targets: web, native' || { echo 'FAIL: error message missing expected substring:'; echo \"$OUT\"; exit 1; }; echo OK; exit 0"
    )

    add_test(
        NAME test_ironc_target_web_smoke
        # Runs ironc build --target=web on a trivial fixture.
        # Accepts either outcome: emcc found (banner on stdout) or emcc absent (install one-liner on stderr).
        # Both prove dispatch reached build_web.c — neither is a parse error.
        COMMAND sh -c "echo 'func main() {}' > /tmp/iron_test_target_web.iron && $<TARGET_FILE:ironc> build --target=web /tmp/iron_test_target_web.iron 2>&1 | grep -qE 'using emcc|emcc not found in PATH'"
    )
endif()

# Phase 2 — emsdk pin-discipline invariants.
# src/cli/build_web.c and the new Phase 2 headers MUST NOT hard-code the
# version string "4.0.23" — the version is read from .emsdk-version at runtime.

add_test(
    NAME test_emsdk_pin_discipline
    COMMAND sh -c "! grep -Fq '4.0.23' \"${CMAKE_SOURCE_DIR}/src/cli/build_web.c\""
)
set_tests_properties(test_emsdk_pin_discipline PROPERTIES
    LABELS "phase2-invariant"
)

add_test(
    NAME test_emsdk_pin_discipline_headers
    COMMAND sh -c "! grep -Fq '4.0.23' \"${CMAKE_SOURCE_DIR}/src/cli/build_web.h\" \"${CMAKE_SOURCE_DIR}/src/cli/web_config.h\""
)
set_tests_properties(test_emsdk_pin_discipline_headers PROPERTIES
    LABELS "phase2-invariant"
)

# ── WEB-RUNTIME-07 portability grep invariant ──────────────────────────────
# Fails the local build if iron_rc.c / iron_builtins.c / iron_collections.c
# gain a reference to any of these non-portable APIs under -Wall -Werror on
# emcc. The full emcc try-compile runs in CI (.github/workflows/web.yml);
# this local grep gives development-time feedback without requiring a local
# emsdk install. The ERE alternation matches headers + symbols that are
# currently absent from all three files.
add_test(
    NAME test_runtime_files_portability_grep
    COMMAND sh -c "! grep -E 'sysconf|pthread_attr|sigaction|sys/wait\\.h|sys/resource\\.h' \"${CMAKE_SOURCE_DIR}/src/runtime/iron_rc.c\" \"${CMAKE_SOURCE_DIR}/src/runtime/iron_builtins.c\" \"${CMAKE_SOURCE_DIR}/src/runtime/iron_collections.c\""
)
set_tests_properties(test_runtime_files_portability_grep PROPERTIES
    LABELS "unit;web-portability"
)

# ── Phase 4 plan-02: WEB-RUNTIME-06 iron_time_web.c symbol gate ─────────────
# Fails the local build if src/stdlib/iron_time_web.c loses any of the 9
# public symbols declared in src/stdlib/iron_time.h. Catches accidental
# rename drift without requiring a local emsdk install. The full emcc
# try-compile runs in CI (.github/workflows/web.yml probe step), which
# covers type + syntax drift; this grep covers symbol-presence drift.
# Labels `unit;web-portability` so `ctest -L web-portability` runs this
# gate alongside the Phase 3 runtime-files portability grep.
add_test(
    NAME test_iron_time_web_symbols
    COMMAND sh -c "\
set -e; \
F=\"${CMAKE_SOURCE_DIR}/src/stdlib/iron_time_web.c\"; \
test -f \"$F\" || { echo 'FAIL: iron_time_web.c missing'; exit 1; }; \
for sym in Iron_time_now Iron_time_now_ms Iron_time_now_ns Iron_time_sleep Iron_time_since Iron_time_Timer Iron_timer_done Iron_timer_update Iron_timer_reset; do \
  grep -q \"$sym\" \"$F\" || { echo \"FAIL: $sym missing from iron_time_web.c\"; exit 1; }; \
done; \
echo 'OK: all 9 iron_time.h symbols present in iron_time_web.c'"
)
set_tests_properties(test_iron_time_web_symbols PROPERTIES
    LABELS "unit;web-portability"
)

# ── Phase 4 plan-02: pin discipline on iron_time_web.c ──────────────────────
# Extends the Phase 2 pin-discipline invariant to the new web time shim:
# the emsdk version string 4.0.23 must live in .emsdk-version only, never
# in a source file. Same inverted-grep pattern as test_emsdk_pin_discipline.
add_test(
    NAME test_emsdk_pin_discipline_iron_time_web
    COMMAND sh -c "! grep -Fq '4.0.23' \"${CMAKE_SOURCE_DIR}/src/stdlib/iron_time_web.c\""
)
set_tests_properties(test_emsdk_pin_discipline_iron_time_web PROPERTIES
    LABELS "phase4-invariant"
)

# ── Phase 7 plan-04: integration test + fixture + pin discipline ─────────────
#
# test_cli_parse_web validates the Phase 2 CLI parse path (--target=web,
# --target=native, unknown target, default target). Named in Phase 7 success
# criterion 7. Linux/macOS only — the #ifdef _WIN32 #error guard in
# test_cli_parse.c enforces this at compile time.

if(NOT WIN32)
    add_executable(test_cli_parse_web
        tests/integration/web/test_cli_parse.c
    )
    add_test(
        NAME test_cli_parse_web
        COMMAND test_cli_parse_web
    )
    set_tests_properties(test_cli_parse_web PROPERTIES
        LABELS "integration;phase7"
        ENVIRONMENT "IRONC_BINARY=$<TARGET_FILE:ironc>"
    )
    # Ensure ironc is built before test_cli_parse_web runs
    add_dependencies(test_cli_parse_web ironc)

    # Phase 7 pin discipline: new fixture + test files must not hardcode 4.0.23
    add_test(
        NAME test_emsdk_pin_discipline_phase7
        COMMAND sh -c "! grep -Fq '4.0.23' \"${CMAKE_SOURCE_DIR}/tests/integration/web/hello.iron\" \"${CMAKE_SOURCE_DIR}/tests/integration/web/test_cli_parse.c\""
    )
    set_tests_properties(test_emsdk_pin_discipline_phase7 PROPERTIES
        LABELS "phase7-invariant"
    )
endif()

# iron CLI tests
add_test(
    NAME test_iron_help
    COMMAND iron
)
set_tests_properties(test_iron_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Package Commands:"
)

add_test(
    NAME test_iron_version
    COMMAND iron --version
)
set_tests_properties(test_iron_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^iron [0-9]"
)

add_test(
    NAME test_iron_no_color
    COMMAND ${CMAKE_COMMAND} -E env NO_COLOR=1 $<TARGET_FILE:iron>
)
set_tests_properties(test_iron_no_color PROPERTIES
    PASS_REGULAR_EXPRESSION "Package Commands:"
)

# iron init tests
if(NOT WIN32)
    add_test(
        NAME test_iron_init
        COMMAND ${CMAKE_COMMAND} -E env PATH=$<TARGET_FILE_DIR:iron>:$ENV{PATH}
            sh -c "rm -rf /tmp/iron_test_init && mkdir -p /tmp/iron_test_init && cd /tmp/iron_test_init && $<TARGET_FILE:iron> init && test -f iron.toml && test -f src/main.iron && test -f .gitignore && grep -q '\\[package\\]' iron.toml && grep -q 'println' src/main.iron"
    )

    add_test(
        NAME test_iron_init_lib
        COMMAND ${CMAKE_COMMAND} -E env PATH=$<TARGET_FILE_DIR:iron>:$ENV{PATH}
            sh -c "rm -rf /tmp/iron_test_init_lib && mkdir -p /tmp/iron_test_init_lib && cd /tmp/iron_test_init_lib && $<TARGET_FILE:iron> init --lib && test -f iron.toml && test -f src/lib.iron && grep -q 'type = \"lib\"' iron.toml"
    )
endif()

# iron package-mode tests
if(NOT WIN32)
    # iron build in package mode
    add_test(
        NAME test_iron_build_package
        COMMAND sh -c "rm -rf /tmp/iron_test_build && mkdir -p /tmp/iron_test_build && cd /tmp/iron_test_build && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> build && test -f target/iron_test_build"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_build_package PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # iron run in package mode
    add_test(
        NAME test_iron_run_package
        COMMAND sh -c "rm -rf /tmp/iron_test_run && mkdir -p /tmp/iron_test_run && cd /tmp/iron_test_run && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> run"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_run_package PROPERTIES
        PASS_REGULAR_EXPRESSION "Hello, Iron!"
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # iron check in package mode
    add_test(
        NAME test_iron_check_package
        COMMAND sh -c "rm -rf /tmp/iron_test_check && mkdir -p /tmp/iron_test_check && cd /tmp/iron_test_check && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> check"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_check_package PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # Regression: iron build still works for projects with no dependencies
    add_test(
        NAME test_iron_build_no_deps_still_works
        COMMAND sh -c "set -e && rm -rf /tmp/iron_test_nodeps && mkdir -p /tmp/iron_test_nodeps && cd /tmp/iron_test_nodeps && $<TARGET_FILE:iron> init && $<TARGET_FILE:iron> build && test -f target/iron_test_nodeps && ./target/iron_test_nodeps | grep -q Hello"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_build_no_deps_still_works PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )

    # Test: iron build creates NO iron.lock when project has no deps
    add_test(
        NAME test_iron_dep_lockfile
        COMMAND sh -c "set -e && rm -rf /tmp/iron_test_dep && mkdir -p /tmp/iron_test_dep/src && printf '[package]\nname = \"test-dep-project\"\nversion = \"0.1.0\"\n\n[dependencies]\n' > /tmp/iron_test_dep/iron.toml && printf 'func main() {\n    println(\"hello deps\")\n}\n' > /tmp/iron_test_dep/src/main.iron && cd /tmp/iron_test_dep && $<TARGET_FILE:iron> build && test -f target/test-dep-project && test ! -f iron.lock"
        WORKING_DIRECTORY /tmp
    )
    set_tests_properties(test_iron_dep_lockfile PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:iron>:$<TARGET_FILE_DIR:ironc>:$ENV{PATH}"
    )
endif()

# ── Benchmark smoke test ───────────────────────────────────────────────────
add_test(NAME benchmark_smoke
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks/run_benchmarks.sh
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(benchmark_smoke PROPERTIES
    TIMEOUT 300
    LABELS "benchmark")

# ---- Install rules ----
if(IRON_BUILD_LSP)
    install(TARGETS ironc iron ironls DESTINATION bin)
else()
    install(TARGETS ironc iron DESTINATION bin)
endif()

# Install runtime C source files (needed by ironc at compile time)
install(DIRECTORY src/runtime/ DESTINATION lib/runtime FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
# Install util files used by the build pipeline
install(FILES
    src/util/stb_ds_impl.c
    src/util/arena.c
    src/util/arena.h
    src/util/strbuf.c
    src/util/strbuf.h
    DESTINATION lib/util
)
# Install all header files from src/ that ironc passes via -I (runtime, vendor)
install(DIRECTORY src/vendor/ DESTINATION lib/vendor)
# Install stdlib C implementations and headers
install(DIRECTORY src/stdlib/ DESTINATION lib/stdlib FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
# Install stdlib .iron files
install(DIRECTORY src/stdlib/ DESTINATION lib/stdlib FILES_MATCHING PATTERN "*.iron")
# Install diagnostics module: header is referenced by lib/runtime/iron_runtime.h
# at user-build time, so it must be present in the install prefix. Without
# this rule, every fresh install fails to compile any program with
# 'diagnostics/diagnostics.h: file not found' (issue #47).
install(DIRECTORY src/diagnostics/ DESTINATION lib/diagnostics FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
