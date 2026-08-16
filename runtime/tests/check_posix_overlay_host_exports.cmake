if(NOT EXISTS "${RUNTIME}")
    message(FATAL_ERROR "runtime executable does not exist: ${RUNTIME}")
endif()

execute_process(
    COMMAND "${NM}" -D --defined-only "${RUNTIME}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE dynamic_symbols
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "cannot inspect runtime exports: ${nm_error}")
endif()

if(NOT dynamic_symbols MATCHES "(^|\n)[^\n]*[ \t]g_psx_resume_seed(\n|$)")
    message(FATAL_ERROR
        "runtime does not export g_psx_resume_seed for POSIX overlay libraries")
endif()
