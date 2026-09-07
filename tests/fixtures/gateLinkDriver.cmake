# gateLinkDriver.cmake
# Test driver for the B-1 link gate (cmake/CheckNoStageLink.cmake).
# Runs the gate against a target file and verifies the verdict matches the
# expectation, proving the gate demonstrably fails on a forbidden shared
# library and passes on a clean build-tree target.
#
# Inputs (via -D):
#   TARGET_FILE - .so or .a to check
#   OBJDUMP     - path to objdump (shared branch)
#   CXXFILT     - path to c++filt (static branch; optional)
#   FORBIDDEN   - regex for forbidden DT_NEEDED names / pxr class prefixes
#   EXPECT      - "fail" (gate must FATAL_ERROR) or "pass" (gate must pass)

if(NOT DEFINED TARGET_FILE OR NOT DEFINED FORBIDDEN OR NOT DEFINED EXPECT)
    message(FATAL_ERROR "gateLinkDriver: TARGET_FILE, FORBIDDEN and EXPECT are required")
endif()
if(NOT EXPECT MATCHES "^(pass|fail)$")
    message(FATAL_ERROR "gateLinkDriver: EXPECT must be 'pass' or 'fail', got '${EXPECT}'")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DTARGET_FILE=${TARGET_FILE}
        -DOBJDUMP=${OBJDUMP}
        -DCXXFILT=${CXXFILT}
        -DFORBIDDEN=${FORBIDDEN}
        -P ${CMAKE_CURRENT_LIST_DIR}/../../cmake/CheckNoStageLink.cmake
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)

if(EXPECT STREQUAL "fail")
    if(_rc EQUAL 0)
        message(FATAL_ERROR
            "gateLinkDriver: B-1 link gate PASSED on forbidden target ${TARGET_FILE} "
            "(expected a hard failure):\n${_out}")
    endif()
    # A configuration error (missing target, etc.) is not proof the gate
    # works: the failure must be a real link VIOLATION on the target.
    if(NOT _err MATCHES "Gate B-1 VIOLATION")
        message(FATAL_ERROR
            "gateLinkDriver: gate did not fail with a real link VIOLATION; "
            "wrong reason or misconfiguration:\n${_out}\n${_err}")
    endif()
    message(STATUS "ok: B-1 link gate correctly FAILED on forbidden target (${TARGET_FILE})")
else()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "gateLinkDriver: B-1 link gate FAILED on clean target ${TARGET_FILE} "
            "(expected a pass):\n${_err}")
    endif()
    message(STATUS "ok: B-1 link gate correctly passed on clean target (${TARGET_FILE})")
endif()
