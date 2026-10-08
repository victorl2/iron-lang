# `iron test` on a project: test blocks in src/ (with access to private
# helpers) and in tests/ (with the project's pub functions) are compiled
# with the project and run one process each; a tests/test_*.iron program
# with its own main still runs whole. Invoked with -DIRON=<iron binary>
# -DWORK=<scratch dir>.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src" "${WORK}/tests")
file(WRITE "${WORK}/iron.toml" "[package]\nname = \"proj\"\nversion = \"0.1.0\"\n")
file(WRITE "${WORK}/src/main.iron" "func main() {\n    println(\"app {double(4)}\")\n}\n")
file(WRITE "${WORK}/src/math.iron"
"pub func double(x: Int) -> Int {\n    return helper(x) * 2\n}\n\nfunc helper(x: Int) -> Int {\n    return x\n}\n\ntest \"helper is identity\" {\n    assert_eq(helper(7), 7)\n}\n")
file(WRITE "${WORK}/tests/math_test.iron"
"test \"double doubles\" {\n    assert_eq(double(21), 42)\n}\n\ntest \"double of zero\" {\n    assert_eq(double(0), 1)\n}\n")
file(WRITE "${WORK}/tests/test_legacy.iron" "func main() {\n    println(\"legacy ok\")\n}\n")

function(expect out pattern)
    string(REGEX MATCH "${pattern}" m "${out}")
    if(NOT m)
        message(FATAL_ERROR "expected /${pattern}/ in:\n${out}")
    endif()
endfunction()

execute_process(COMMAND "${IRON}" test WORKING_DIRECTORY "${WORK}"
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
set(all "${out}${err}")
if(rc EQUAL 0)
    message(FATAL_ERROR "iron test passed although a test fails:\n${all}")
endif()
expect("${all}" "test helper is identity \\.\\.\\. ok")
expect("${all}" "test double doubles \\.\\.\\. ok")
expect("${all}" "test double of zero \\.\\.\\. FAILED")
expect("${all}" "assert_eq at tests/math_test\\.iron:6: expected 1, got 0")
expect("${all}" "2 passed, 1 failed")
expect("${all}" "legacy ok")

execute_process(COMMAND "${IRON}" test helper WORKING_DIRECTORY "${WORK}"
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
set(all "${out}${err}")
expect("${all}" "1 passed, 0 failed, 2 filtered out")

# Fix the failing test: everything passes and iron test exits 0.
file(WRITE "${WORK}/tests/math_test.iron"
"test \"double doubles\" {\n    assert_eq(double(21), 42)\n}\n")
execute_process(COMMAND "${IRON}" test WORKING_DIRECTORY "${WORK}"
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "iron test failed with every test passing:\n${out}${err}")
endif()
expect("${out}${err}" "2 passed, 0 failed")
