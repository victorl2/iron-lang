# Code generator fuzzing (tests/fuzz/cg_fuzz.py) and the regressions it
# found. Included from the top-level CMakeLists.txt.

if(Python3_EXECUTABLE)
    # Fixed seeds, about a minute each with -j4:
    #   modes   every generator's programs in every build mode (release,
    #           --debug, --debug-build, --no-optimize, C at -O0 / -O1, LIR
    #           pass subsets, UBSan, --emit-c twice): same behavior, no C
    #           compiler warnings
    #   meta    semantics-preserving rewrites of the programs (pf_meta.py)
    #   mutate  near-valid mutants: a diagnostic code or a clean build, never
    #           a crash, a hang or C the C compiler rejects (pf_mutate.py)
    #   shapes  nested generic, enum, tuple and optional types (pf_shapes.py)
    foreach(_cg_kind modes meta mutate shapes)
        add_test(NAME cg_fuzz_${_cg_kind}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_fuzz.py
                --suite ${_cg_kind} $<TARGET_FILE:ironc> ${CMAKE_BINARY_DIR}/cg_fuzz_${_cg_kind})
        # A sanitizer build of ironc on a loaded machine needs more than
        # the default 120 s per build (60 s before a mutant's build counts
        # as a hang).
        set_tests_properties(cg_fuzz_${_cg_kind} PROPERTIES LABELS "codegen" TIMEOUT 3600
            ENVIRONMENT "PF_TIMEOUT=600;PF_MUTATE_TIMEOUT=300")
    endforeach()
endif()

if(NOT WIN32)
    set(_cg_reg ${CMAKE_SOURCE_DIR}/tests/integration/v4/regressions)
    # #561: a captured parameter with copy propagation left out.
    add_test(NAME cg_capture_param_no_copyprop
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/capture_param_slot.iron ${_cg_reg}/capture_param_slot.expected
            ${CMAKE_BINARY_DIR}/cg_check/capture_param)
    set_tests_properties(cg_capture_param_no_copyprop PROPERTIES LABELS "codegen" TIMEOUT 120
        ENVIRONMENT "IRON_LIR_SKIP=copyprop")
    # #559 / #560: the thread's write is seen after await, and W0604 is
    # reported for `val h = spawn`.
    add_test(NAME cg_spawn_var_after_await
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/spawn_var_after_await.iron ${_cg_reg}/spawn_var_after_await.expected
            ${CMAKE_BINARY_DIR}/cg_check/spawn_var)
    set_tests_properties(cg_spawn_var_after_await PROPERTIES LABELS "codegen" TIMEOUT 120
        ENVIRONMENT "CG_CHECK_DIAG=spawn block 'u' mutates outer variable 'n'")
    # #562: warning free C, also at -O0 and with every LIR pass left out.
    add_test(NAME cg_c_warning_free
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/c_warning_free.iron ${_cg_reg}/c_warning_free.expected
            ${CMAKE_BINARY_DIR}/cg_check/c_warning_free)
    add_test(NAME cg_c_warning_free_noopt
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/c_warning_free.iron ${_cg_reg}/c_warning_free.expected
            ${CMAKE_BINARY_DIR}/cg_check/c_warning_free_noopt)
    set_tests_properties(cg_c_warning_free PROPERTIES LABELS "codegen" TIMEOUT 120)
    set_tests_properties(cg_c_warning_free_noopt PROPERTIES LABELS "codegen" TIMEOUT 120
        ENVIRONMENT "IRON_LIR_SKIP=all;IRON_CC_FLAGS=-O0")
endif()

if(NOT WIN32)
    # #578: a boxed recursive payload read inline, copy propagation left out.
    add_test(NAME cg_recursive_payload_inline
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/recursive_payload_inline.iron ${_cg_reg}/recursive_payload_inline.expected
            ${CMAKE_BINARY_DIR}/cg_check/recursive_payload_inline)
    set_tests_properties(cg_recursive_payload_inline PROPERTIES LABELS "codegen" TIMEOUT 120
        ENVIRONMENT "IRON_LIR_SKIP=copyprop")
    # #579: bounded vector pop with the result unused, without optimization.
    add_test(NAME cg_bvec_pop_discard
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/bvec_pop_discard.iron ${_cg_reg}/bvec_pop_discard.expected
            ${CMAKE_BINARY_DIR}/cg_check/bvec_pop_discard --no-optimize)
    set_tests_properties(cg_bvec_pop_discard PROPERTIES LABELS "codegen" TIMEOUT 120)
endif()

if(NOT WIN32)
    # #596: warning free C for rc element list parameters and a hoisted weak
    # upgrade; a split list's count read inline with copy propagation off.
    foreach(_cg_t rc_list_param_const weak_upgrade_in_loop)
        add_test(NAME cg_${_cg_t}
            COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
                ${_cg_reg}/${_cg_t}.iron ${_cg_reg}/${_cg_t}.expected
                ${CMAKE_BINARY_DIR}/cg_check/${_cg_t})
        set_tests_properties(cg_${_cg_t} PROPERTIES LABELS "codegen" TIMEOUT 120)
    endforeach()
    add_test(NAME cg_interface_list_count
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/interface_list_count.iron ${_cg_reg}/interface_list_count.expected
            ${CMAKE_BINARY_DIR}/cg_check/interface_list_count)
    set_tests_properties(cg_interface_list_count PROPERTIES LABELS "codegen" TIMEOUT 120
        ENVIRONMENT "IRON_LIR_SKIP=copyprop")
endif()

if(NOT WIN32)
    # #598 .. #601: nullable enum payloads, T? tuple components, a Map of a
    # generic enum in an object field, dispatch to generic instances.
    foreach(_cg_t enum_nullable_payload tuple_nullable_component map_field_generic_enum
                  iface_generic_instance_dispatch)
        add_test(NAME cg_${_cg_t}
            COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
                ${_cg_reg}/${_cg_t}.iron ${_cg_reg}/${_cg_t}.expected
                ${CMAKE_BINARY_DIR}/cg_check/${_cg_t})
        set_tests_properties(cg_${_cg_t} PROPERTIES LABELS "codegen" TIMEOUT 120)
    endforeach()
endif()
if(NOT WIN32)
    # #826 .. #829: `rc [T]?`, the release of an inlined `rc T?` temporary,
    # and the expected type through tuples, enum payloads, `T?` and `rc`.
    foreach(_cg_t rc_list_nullable rc_nullable_inlined_temp expected_type_tuple_nullable
                  expected_type_through_nullable expected_type_method_arg_shapes)
        add_test(NAME cg_${_cg_t}
            COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
                ${_cg_reg}/${_cg_t}.iron ${_cg_reg}/${_cg_t}.expected
                ${CMAKE_BINARY_DIR}/cg_check/${_cg_t})
        set_tests_properties(cg_${_cg_t} PROPERTIES LABELS "codegen" TIMEOUT 120)
    endforeach()
endif()

if(NOT WIN32)
    # #606: nullable generic instance parameters, tuple object fields.
    add_test(NAME cg_generic_nullable_param
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/generic_nullable_param.iron ${_cg_reg}/generic_nullable_param.expected
            ${CMAKE_BINARY_DIR}/cg_check/generic_nullable_param)
    set_tests_properties(cg_generic_nullable_param PROPERTIES LABELS "codegen" TIMEOUT 120)
endif()

if(NOT WIN32)
    # pop() of an interface list with the result unused: warning free C.
    add_test(NAME cg_iface_list_pop_discard
        COMMAND ${CMAKE_SOURCE_DIR}/tests/fuzz/cg_check.sh $<TARGET_FILE:ironc>
            ${_cg_reg}/iface_list_pop_discard.iron ${_cg_reg}/iface_list_pop_discard.expected
            ${CMAKE_BINARY_DIR}/cg_check/iface_list_pop_discard)
    set_tests_properties(cg_iface_list_pop_discard PROPERTIES LABELS "codegen" TIMEOUT 120)
endif()
