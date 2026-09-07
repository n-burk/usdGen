# CheckNoStageLink.cmake
# Gate B-1 (link half): verify that a target does not depend on forbidden pxr
# library families (usd/usdImaging/hd/hdsi/hdi/hgi/glf/garch).
#
# Inputs (via -D):
#   TARGET_FILE - path to .so or .a
#   OBJDUMP     - path to objdump (shared-library branch; required)
#   CXXFILT     - path to c++filt (static-archive branch; optional)
#   FORBIDDEN   - regex for forbidden DT_NEEDED names, e.g.
#                 ^libusd_(usd|hd|hgi|glf|garch)
#
# Shared library: objdump -p, match NEEDED entries.
# Static archive: nm -u (undefined symbols), demangle with c++filt and match
# the forbidden OpenUSD class-name families. In installed OpenUSD every pxr
# class lives in one flat reserved namespace (pxrInternal_v...__pxrReserved__),
# so the family must be recognised by class-name prefix:
#   usd family  -> Usd* (UsdStage, UsdGeomBoundable, UsdImaging*, ...)
#   hd family   -> Hd*  (HdSceneIndexBase, HdSt*, Hdsi*, Hdi*, ...)
#   hgi/glf/garch -> Hgi*, Glf*, Garch*
# Our own classes are in the global usdGen:: namespace (lowercase) and never
# match.
#
# Any nonzero tool result (objdump / nm / c++filt) is a hard FATAL_ERROR —
# a gate that cannot read its target must fail loudly, not silently pass.

if(NOT DEFINED TARGET_FILE OR TARGET_FILE STREQUAL "")
    message(FATAL_ERROR "CheckNoStageLink: TARGET_FILE is required")
endif()
if(NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR
        "CheckNoStageLink: TARGET_FILE does not exist: ${TARGET_FILE}")
endif()
if(NOT DEFINED FORBIDDEN OR FORBIDDEN STREQUAL "")
    message(FATAL_ERROR "CheckNoStageLink: FORBIDDEN regex is required")
endif()

get_filename_component(_target_name ${TARGET_FILE} NAME)

# Classify the file by its 4-byte magic, read as hex (the literal "\177ELF"
# comparison is not a valid CMake escape and never matches):
#   7f454c46 = "\177ELF"  -> ELF shared object
#   213c6172 = "!<ar"     -> static archive
file(READ "${TARGET_FILE}" _magic_hex HEX LIMIT 4)

if(_magic_hex STREQUAL "7f454c46")
    # Shared library: check DT_NEEDED entries.
    if(NOT DEFINED OBJDUMP OR OBJDUMP STREQUAL "")
        message(FATAL_ERROR
            "CheckNoStageLink: OBJDUMP is required for shared target ${_target_name}")
    endif()
    execute_process(
        COMMAND ${OBJDUMP} -p "${TARGET_FILE}"
        OUTPUT_VARIABLE _needed
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "CheckNoStageLink: objdump failed (rc=${_rc}) on ${_target_name}:\n${_err}")
    endif()
    string(REGEX REPLACE "\n" "\n;" _needed_list "${_needed}")
    foreach(line IN LISTS _needed_list)
        if(line MATCHES "NEEDED")
            string(REGEX REPLACE "^ *NEEDED +" "" _libname "${line}")
            # objdump prints "  NEEDED               libfoo.so"; extract the
            # library name (CMake regex has no [[:space:]] POSIX classes — use
            # a plain space quantifier; objdump output is space-indented).
            # FORBIDDEN keeps its ^ anchor; CMake MATCHES is unanchored, so the
            # anchor still binds to the start of the extracted name.
            if(_libname MATCHES "${FORBIDDEN}")
                message(FATAL_ERROR
                    "Gate B-1 VIOLATION: ${_target_name} links forbidden library: ${_libname}")
            endif()
        endif()
    endforeach()
elseif(_magic_hex STREQUAL "213c6172")
    # Static archive - undefined symbol check.
    execute_process(
        COMMAND nm -u "${TARGET_FILE}"
        OUTPUT_VARIABLE _undefined
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "CheckNoStageLink: nm failed (rc=${_rc}) on ${_target_name}:\n${_err}")
    endif()
    if(CXXFILT)
        set(_sym_tmp "/tmp/usdGenB1Gate_${_target_name}.sym")
        file(WRITE "${_sym_tmp}" "${_undefined}")
        execute_process(
            COMMAND ${CXXFILT}
            INPUT_FILE "${_sym_tmp}"
            OUTPUT_VARIABLE _undefined
            RESULT_VARIABLE _rc
            ERROR_VARIABLE _err)
        file(REMOVE "${_sym_tmp}")
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "CheckNoStageLink: c++filt failed (rc=${_rc}) on ${_target_name}:\n${_err}")
        endif()
    endif()
    string(REGEX REPLACE "\n" "\n;" _undef_list "${_undefined}")
    foreach(sym IN LISTS _undef_list)
        if(sym MATCHES "__pxrReserved__::(Usd|Hd|Hgi|Glf|Garch)")
            message(FATAL_ERROR
                "Gate B-1 VIOLATION: ${_target_name} references forbidden pxr symbol: ${sym}")
        endif()
    endforeach()
else()
    message(FATAL_ERROR
        "CheckNoStageLink: ${_target_name} is neither an ELF shared object (magic 7f454c46) "
        "nor a static archive (magic 213c6172); got ${_magic_hex}")
endif()

message(STATUS "B-1 link check passed for ${_target_name}")
