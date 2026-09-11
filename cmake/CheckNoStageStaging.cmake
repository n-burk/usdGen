# CheckNoStageStaging.cmake
# Gate B-2 (13-codebase-alignment.md §7 V2-8): the production staging path
# touches no UsdStage, UsdPrim, UsdAttribute, UsdRelationship, UsdGeom*, or
# UsdShade* API. Two halves, B-1 pattern: (1) include-regex over the fenced
# staging TUs; (2) nm undefined-symbol check over the fenced TUs' compiled
# objects. Fail-closed: an empty TU list, a missing source file, a missing
# object file, or an nm failure itself is a hard error, never a silent pass.
# EXPECT=pass (default) fails on any violation; EXPECT=fail requires at
# least one violation (fixture proof the gate bites) and fails otherwise.
#
# Inputs (via -D):
#   SOURCE_DIR    - root the STAGING_TUS paths are relative to
#   STAGING_TUS   - |-separated TU paths (headers and sources alike)
#   OBJECTS       - |-separated absolute .o paths for the nm half;
#                   empty string skips the nm half (source-only fixtures)
#   FORBIDDEN_INC - regex matching forbidden include paths
#   FORBIDDEN_SYM - regex matching forbidden demangled symbol names
#   NM            - nm binary (required iff OBJECTS is non-empty)
#   EXPECT        - "pass" (default) or "fail"

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "CheckNoStageStaging: SOURCE_DIR is required")
endif()
if(NOT DEFINED STAGING_TUS OR STAGING_TUS STREQUAL "")
    message(FATAL_ERROR "CheckNoStageStaging: STAGING_TUS is required")
endif()
if(NOT DEFINED FORBIDDEN_INC OR FORBIDDEN_INC STREQUAL "")
    message(FATAL_ERROR "CheckNoStageStaging: FORBIDDEN_INC is required")
endif()
if(NOT DEFINED FORBIDDEN_SYM OR FORBIDDEN_SYM STREQUAL "")
    message(FATAL_ERROR "CheckNoStageStaging: FORBIDDEN_SYM is required")
endif()
if(NOT DEFINED EXPECT OR EXPECT STREQUAL "")
    set(EXPECT "pass")
endif()
if(NOT EXPECT MATCHES "^(pass|fail)$")
    message(FATAL_ERROR
        "CheckNoStageStaging: EXPECT must be 'pass' or 'fail', got '${EXPECT}'")
endif()

string(REPLACE "|" ";" _tus "${STAGING_TUS}")
list(LENGTH _tus _n_tus)
if(_n_tus EQUAL 0)
    message(FATAL_ERROR "CheckNoStageStaging: no TUs to scan (silent pass refused)")
endif()

set(_violations "")

# Half 1: forbidden includes in the fenced sources.
foreach(rel IN LISTS _tus)
    set(f "${SOURCE_DIR}/${rel}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR
            "CheckNoStageStaging: fenced TU is missing: ${f}")
    endif()
    file(STRINGS "${f}" _lines)
    foreach(line IN LISTS _lines)
        if(line MATCHES "#include.*${FORBIDDEN_INC}")
            list(APPEND _violations "include: ${f}: ${line}")
        endif()
    endforeach()
endforeach()

# Half 2: forbidden undefined symbols in the fenced objects.
if(DEFINED OBJECTS AND NOT OBJECTS STREQUAL "")
    if(NOT DEFINED NM OR NM STREQUAL "" OR NM MATCHES "NOTFOUND")
        message(FATAL_ERROR
            "CheckNoStageStaging: NM (nm binary) is required when OBJECTS is non-empty")
    endif()
    string(REPLACE "|" ";" _objs "${OBJECTS}")
    foreach(o IN LISTS _objs)
        if(NOT EXISTS "${o}")
            message(FATAL_ERROR
                "CheckNoStageStaging: fenced object is missing (stale build?): ${o}")
        endif()
        execute_process(
            COMMAND "${NM}" -C --undefined-only "${o}"
            OUTPUT_VARIABLE _syms
            ERROR_VARIABLE _nm_err
            RESULT_VARIABLE _nm_rc)
        if(NOT _nm_rc EQUAL 0)
            message(FATAL_ERROR
                "CheckNoStageStaging: nm failed on ${o}: ${_nm_err}")
        endif()
        string(REPLACE "\n" ";" _sym_lines "${_syms}")
        foreach(symline IN LISTS _sym_lines)
            if(symline MATCHES "${FORBIDDEN_SYM}")
                list(APPEND _violations "symbol: ${o}: ${symline}")
            endif()
        endforeach()
    endforeach()
endif()

list(LENGTH _violations _n_violations)
if(EXPECT STREQUAL "fail")
    if(_n_violations EQUAL 0)
        message(FATAL_ERROR
            "CheckNoStageStaging: gate did NOT trigger on forbidden input "
            "(misconfigured fixture?)")
    endif()
    message(STATUS
        "ok: B-2 staging gate correctly FAILED on forbidden input (${_n_violations} hits)")
    return()
endif()

if(NOT _n_violations EQUAL 0)
    list(LENGTH _violations _n)
    set(_shown "")
    set(_i 0)
    foreach(v IN LISTS _violations)
        if(_i LESS 10)
            set(_shown "${_shown}\n  ${v}")
        endif()
        math(EXPR _i "${_i} + 1")
    endforeach()
    message(FATAL_ERROR
        "Gate B-2 staging VIOLATION (${_n} hits, first 10):${_shown}")
endif()
message(STATUS "B-2 staging check passed")
