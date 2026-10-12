if(NOT WIN32)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(http_conformance conformance.c)
    target_link_libraries(http_conformance iron_stdlib iron_runtime Threads::Threads)
    add_test(NAME test_http_conformance COMMAND http_conformance)
    set_tests_properties(test_http_conformance PROPERTIES
        LABELS "stdlib;networking"
        TIMEOUT 120)

    add_executable(http_diff_runner
        differential/runner.c
        vendor/picohttpparser/picohttpparser.c)
    target_include_directories(http_diff_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/picohttpparser)
    target_link_libraries(http_diff_runner iron_stdlib iron_runtime)
    add_test(
        NAME test_http_differential
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/differential/difffuzz.py
                --runner $<TARGET_FILE:http_diff_runner>
                --count 1500 --seed 20261012)
    set_tests_properties(test_http_differential PROPERTIES
        LABELS "stdlib;networking"
        TIMEOUT 120)
endif()
