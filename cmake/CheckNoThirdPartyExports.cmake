# CheckNoThirdPartyExports.cmake
# Gate B-1: verify no SeExpr2:: or Ptex:: symbols are exported from libusdGen.so
# (third-party code is static + hidden, plan §3.3).
#
# Inputs:
#   TARGET_FILE - path to libusdGen.so
#   CXXFILT     - path to c++filt (optional)
#
# Any nonzero tool result (nm / c++filt) is a hard FATAL_ERROR — a gate that
# cannot read its target must fail loudly, not silently pass (sol S-1).

if(NOT DEFINED TARGET_FILE OR TARGET_FILE STREQUAL "")
    message(FATAL_ERROR "CheckNoThirdPartyExports: TARGET_FILE is required")
endif()
if(NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR
        "CheckNoThirdPartyExports: TARGET_FILE does not exist: ${TARGET_FILE}")
endif()

find_program(NM_BIN nm)
if(NM_BIN)
    execute_process(
        COMMAND ${NM_BIN} -D --defined-only "${TARGET_FILE}"
        OUTPUT_VARIABLE _symbols
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "CheckNoThirdPartyExports: nm failed (rc=${_rc}) on ${TARGET_FILE}:\n${_err}")
    endif()
    if(CXXFILT)
        set(_sym_tmp "/tmp/usdGenB1Gate_exports.symbols")
        file(WRITE "${_sym_tmp}" "${_symbols}")
        execute_process(
            COMMAND ${CXXFILT}
            INPUT_FILE "${_sym_tmp}"
            OUTPUT_VARIABLE _symbols
            RESULT_VARIABLE _rc
            ERROR_VARIABLE _err)
        file(REMOVE "${_sym_tmp}")
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "CheckNoThirdPartyExports: c++filt failed (rc=${_rc}) on ${TARGET_FILE}:\n${_err}")
        endif()
    endif()

    string(REGEX REPLACE "\n" "\n;" _sym_list "${_symbols}")
    foreach(sym IN LISTS _sym_list)
        if(sym MATCHES "SeExpr2::")
            message(FATAL_ERROR
                "Gate B-1 third-party export VIOLATION: ${sym} is exported from libusdGen.so")
        endif()
        if(sym MATCHES "Ptex::")
            message(FATAL_ERROR
                "Gate B-1 third-party export VIOLATION: ${sym} is exported from libusdGen.so")
        endif()
    endforeach()
else()
    message(FATAL_ERROR
        "CheckNoThirdPartyExports: nm not found; cannot verify ${TARGET_FILE}")
endif()

message(STATUS "B-1 no-third-party-exports check passed")
