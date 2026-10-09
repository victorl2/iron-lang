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

# FORBIDDEN (optional): lines that must not appear (a cascade of a
# diagnostic already reported, for example).
if(DEFINED FORBIDDEN)
    file(STRINGS ${FORBIDDEN} nots)
    foreach(n IN LISTS nots)
        string(FIND "${all}" "${n}" pos)
        if(NOT pos EQUAL -1)
            message(FATAL_ERROR "unexpected output: ${n}\n--- output ---\n${all}")
        endif()
    endforeach()
endif()
