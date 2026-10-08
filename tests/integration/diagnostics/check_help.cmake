# Run `ironc check` on SOURCE and require every line of EXPECTED to appear
# in its output (each is a help text a diagnostic must carry).
execute_process(COMMAND ${IRONC} check ${SOURCE}
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
set(all "${out}${err}")
file(STRINGS ${EXPECTED} wants)
foreach(w IN LISTS wants)
    string(FIND "${all}" "${w}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing help: ${w}\n--- output ---\n${all}")
    endif()
endforeach()
