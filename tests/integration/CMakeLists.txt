# v3 root corpus moved to v3-archive/ in Phase 35 MIG-08 (clean v3->v4 break,
# no `ironc migrate` tool this milestone). The default integration runner is
# obsolete: v4 corpus runs via tests/integration/v4/CMakeLists.txt
# (test_v4_acceptance), v4 negative corpus via tests/integration/v4-fail/, and
# the archived v3 corpus runs only when IRON_RUN_ARCHIVED_V3_CORPUS=ON (see
# tests/integration/v3-archive/CMakeLists.txt + root CMakeLists.txt option
# declaration).

# Phase 35 MIG-08: v3 corpus archive -- opt-in only. The option is declared in
# the root CMakeLists.txt; this gated add_subdirectory wires the archive
# CTest registration only when the user explicitly requests it.
if(IRON_RUN_ARCHIVED_V3_CORPUS)
    add_subdirectory(v3-archive)
endif()
