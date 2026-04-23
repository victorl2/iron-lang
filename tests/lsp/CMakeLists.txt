# LSP tests — Phase 1 ships the parity harness; full LSP suite arrives in
# Phase 2+ once `ironls` proper starts landing. Adding new subdirectories
# here does not require any edit to the root CMakeLists.txt (which only
# references this directory).

add_subdirectory(parity)
add_subdirectory(unit)
add_subdirectory(smoke)
# Phase 5 Plan 05-05 (FMT-06, D-07): quickfix fmt-clean CI gate.
add_subdirectory(fmt)
# Phase 7 Plan 07-01 (HARD-14, HARD-20): crash-dump + parent-death
# shell invariants. Pure CTest shell scripts (no compiled targets).
add_subdirectory(invariant)
