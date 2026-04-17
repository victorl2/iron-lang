# LSP tests — Phase 1 ships the parity harness; full LSP suite arrives in
# Phase 2+ once `ironls` proper starts landing. Adding new subdirectories
# here does not require any edit to the root CMakeLists.txt (which only
# references this directory).

add_subdirectory(parity)
add_subdirectory(unit)
add_subdirectory(smoke)
