if(NOT WIN32)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(http_benchmark
        bench_http.c
        ${CMAKE_SOURCE_DIR}/tests/http/vendor/picohttpparser/picohttpparser.c)
    target_include_directories(http_benchmark PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/http/vendor/picohttpparser)
    target_link_libraries(http_benchmark iron_stdlib iron_runtime Threads::Threads)

    add_test(NAME test_http_bench_smoke
        COMMAND http_benchmark serialize iron small 10)
    set_tests_properties(test_http_bench_smoke PROPERTIES
        LABELS "stdlib"
        PASS_REGULAR_EXPRESSION "SAMPLE iron serialize small")

    add_test(
        NAME test_http_bench_table
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/table.py
                ${CMAKE_SOURCE_DIR}/tests/http/bench_table_fixture.txt)
    set_tests_properties(test_http_bench_table PROPERTIES
        LABELS "stdlib"
        PASS_REGULAR_EXPRESSION "Small GET.*1.000")
endif()
