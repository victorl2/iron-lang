#ifndef IRON_CLI_VERSION_H
#define IRON_CLI_VERSION_H

/* Single source of truth for the iron compiler version string.
 *
 * CMake injects IRON_VERSION_STRING via target_compile_definitions on the
 * iron and ironc executables (see CMakeLists.txt around the
 * iron / ironc add_executable + target_compile_definitions blocks).
 *
 * The #ifndef guard below is a defensive fallback for build paths that
 * bypass the CMake target (e.g., direct unit-test compiles of toml.c
 * via the unit/CMakeLists.txt direct-compile pattern). The fallback
 * value should track the in-tree version; CMake-driven builds always
 * win because their -DIRON_VERSION_STRING flag is set BEFORE this
 * header is included.
 *
 * Phase 95 PIN-02 / PIN-03: pkg_build.c's check_iron_version reads this
 * macro to compare the running compiler against the [package].iron
 * constraint declared in iron.toml. Keeping a single source means
 * `iron --version`, `ironc --version`, and the version-mismatch error
 * all report the same string.
 */
#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "0.1.1"
#endif

#endif /* IRON_CLI_VERSION_H */
