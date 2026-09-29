cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)

# Keep the Windows SDK's min/max macros out of C++ standard-library and
# OpenUSD headers. The definition is a no-op on other platforms.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# OpenUSD discovery
# ---------------------------------------------------------------------------
set(USD_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OpenUSD_26_08"
    CACHE PATH "OpenUSD installation prefix")

# rigExec is optional (S44).  It is expected to be *before* pxr so that the
# rigExec config does not re-enter pxrConfig a second time (CMake 3.28
# double-pxrConfig trap, plan §3.2).
set(RIGEXEC_INSTALL_DIR "" CACHE PATH "rigExec install prefix (optional)")
set(USDGEN_WITH_RIGEXEC OFF CACHE BOOL "Link rigExec::rigExecMath (optional)")
if(USDGEN_WITH_RIGEXEC AND RIGEXEC_INSTALL_DIR)
    find_package(rigExec CONFIG QUIET PATHS "${RIGEXEC_INSTALL_DIR}")
endif()

if(NOT TARGET usd)
    # OpenUSD's pxrTargets reference OpenGL::GL (via garch's interface). Find
    # it first so the imported target exists before pxrConfig resolves.
    find_package(OpenGL REQUIRED)
    # build_usd.py co-installs OpenSubdiv (and, on Unix, OpenSubdivConfig.cmake)
    # into the same prefix as OpenUSD. find_package(pxr PATHS ...) does not put
    # that prefix on CMAKE_PREFIX_PATH, so pxrConfig's find_dependency(OpenSubdiv
    # CONFIG) cannot see the package unless we point at it. Prepending the
    # prefix is the relocatable equivalent of how OpenUSD itself was configured.
    list(PREPEND CMAKE_PREFIX_PATH "${USD_INSTALL_DIR}")
    if(NOT OpenSubdiv_DIR)
        foreach(_usdgen_osd_cmake
                "${USD_INSTALL_DIR}/lib/cmake/OpenSubdiv"
                "${USD_INSTALL_DIR}/lib64/cmake/OpenSubdiv")
            if(EXISTS "${_usdgen_osd_cmake}/OpenSubdivConfig.cmake")
                set(OpenSubdiv_DIR "${_usdgen_osd_cmake}")
                break()
            endif()
        endforeach()
        unset(_usdgen_osd_cmake)
    endif()
    # Windows (and any other prefix that ships OSD binaries without a CMake
    # package): skip pxrConfig's rediscovery and reconstruct the imported
    # targets that pxrTargets names. Linux pxrTargets use OpenSubdiv::osdCPU;
    # Windows uses OpenSubdiv::osdCPU_static.
    if(NOT OpenSubdiv_DIR)
        if(NOT DEFINED PXR_FIND_OPENSUBDIV_IN_CONFIG)
            set(PXR_FIND_OPENSUBDIV_IN_CONFIG OFF)
        endif()
        foreach(_usdgen_osd_name osdCPU osdGPU)
            if(NOT TARGET OpenSubdiv::${_usdgen_osd_name}_static AND
               EXISTS "${USD_INSTALL_DIR}/lib/${_usdgen_osd_name}.lib")
                add_library(OpenSubdiv::${_usdgen_osd_name}_static STATIC IMPORTED)
                set_target_properties(OpenSubdiv::${_usdgen_osd_name}_static
                    PROPERTIES
                        IMPORTED_LOCATION "${USD_INSTALL_DIR}/lib/${_usdgen_osd_name}.lib"
                        INTERFACE_INCLUDE_DIRECTORIES "${USD_INSTALL_DIR}/include"
                        INTERFACE_COMPILE_DEFINITIONS
                            "OPENSUBDIV_HAS_PATCH_SHADER_SOURCE_GLSL;OPENSUBDIV_HAS_GLSL_TRANSFORM_FEEDBACK;OPENSUBDIV_HAS_GLSL_COMPUTE;OPENSUBDIV_HAS_OPENGL")
            endif()
            if(NOT TARGET OpenSubdiv::${_usdgen_osd_name})
                foreach(_usdgen_osd_file
                        "lib${_usdgen_osd_name}${CMAKE_SHARED_LIBRARY_SUFFIX}"
                        "lib${_usdgen_osd_name}${CMAKE_STATIC_LIBRARY_SUFFIX}")
                    if(EXISTS "${USD_INSTALL_DIR}/lib/${_usdgen_osd_file}")
                        if(_usdgen_osd_file STREQUAL
                           "lib${_usdgen_osd_name}${CMAKE_SHARED_LIBRARY_SUFFIX}")
                            add_library(OpenSubdiv::${_usdgen_osd_name} SHARED IMPORTED)
                        else()
                            add_library(OpenSubdiv::${_usdgen_osd_name} STATIC IMPORTED)
                        endif()
                        set_target_properties(OpenSubdiv::${_usdgen_osd_name}
                            PROPERTIES
                                IMPORTED_LOCATION
                                    "${USD_INSTALL_DIR}/lib/${_usdgen_osd_file}"
                                INTERFACE_INCLUDE_DIRECTORIES
                                    "${USD_INSTALL_DIR}/include")
                        break()
                    endif()
                endforeach()
            endif()
        endforeach()
        unset(_usdgen_osd_name)
        unset(_usdgen_osd_file)
    endif()
    find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
endif()

# Threads are used by the expression frontend. No system zlib is required:
# requiring one made a standard Windows build_usd.py OpenUSD prefix unusable
# because that prefix intentionally omits zlib when no USD feature needs it.
# The only zlib consumer, the vendored Ptex, links the private vendored copy
# (usdGen_zlib, thirdparty/zlib).
find_package(Threads REQUIRED)
# tbb/task.h, reached through the OpenUSD headers, prints a deprecation
# banner on every translation unit unless told not to.
add_compile_definitions(TBB_SUPPRESS_DEPRECATED_MESSAGES=1)

# ---------------------------------------------------------------------------
# Compile hardening
# ---------------------------------------------------------------------------
# Vendored sources (thirdparty/) are compiled as-is and carry the
# USDGEN_VENDORED target property, which keeps the warning level off them;
# the generator expressions below read that property on the consuming target.
set(_usdgen_first_party "$<NOT:$<BOOL:$<TARGET_PROPERTY:USDGEN_VENDORED>>>")
if(MSVC)
    # C4251 fires for every std:: member of an exported class; the DSOs here
    # are built and consumed with one toolchain, which is the case the warning
    # is not about. /external:W0 keeps third-party (SYSTEM) headers quiet.
    add_compile_options(
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,${_usdgen_first_party}>:/W4;/wd4251;/external:W0>"
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:${_usdgen_first_party}>>:/W0>")
    # The tests use getenv/fopen; the "_s" variants are not portable.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,${_usdgen_first_party}>:-Wall;-Wextra;-Wno-cpp>")
endif()

# Release adds NDEBUG through the per-config flag strings. The CUDA oracle
# tests rely on assert, and undefining it again (-UNDEBUG) makes cl warn D9025
# on each of their translation units. Strip it from the flag strings instead
# and re-add it per target, except where USDGEN_KEEP_ASSERTS is set.
foreach(_config RELEASE RELWITHDEBINFO MINSIZEREL)
    foreach(_lang C CXX)
        string(REGEX REPLACE "[/-]DNDEBUG" "" CMAKE_${_lang}_FLAGS_${_config}
            "${CMAKE_${_lang}_FLAGS_${_config}}")
    endforeach()
endforeach()
add_compile_definitions(
    "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:USDGEN_KEEP_ASSERTS>>>>:NDEBUG>")

# $ORIGIN is ELF-only.  Use the native loader spelling where applicable and
# leave Windows without an RPATH.
if(UNIX AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
elseif(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# The host's dynamic-loader variable, which the test registrations extend.
# The three hosts disagree on the name, and on Windows it is PATH itself.
# No companion path-list separator is defined on purpose: test environments
# are built with CTest's ENVIRONMENT_MODIFICATION path_list_* operations,
# which join using the host's native separator, so nothing in this build
# hard-codes ';' or ':' (see the helpers in the USDGEN_BUILD_TESTS block).
if(WIN32)
    set(USDGEN_RUNTIME_LIBRARY_PATH_ENV "PATH")
elseif(APPLE)
    set(USDGEN_RUNTIME_LIBRARY_PATH_ENV "DYLD_LIBRARY_PATH")
else()
    set(USDGEN_RUNTIME_LIBRARY_PATH_ENV "LD_LIBRARY_PATH")
endif()

# ---------------------------------------------------------------------------
# Per-target cache variables (plan §3.5.2)
# ---------------------------------------------------------------------------
set(USDGEN_FP_CONTRACT off CACHE STRING
    "-ffp-contract value for usdGenMath, usdGen_seexpr and noise.cpp (GPU parity) (off|fast)")
set_property(CACHE USDGEN_FP_CONTRACT PROPERTY STRINGS off fast)

set(USDGEN_INSTALL_LIBDIR     "lib"         CACHE PATH "lib dir")
set(USDGEN_INSTALL_PLUGINDIR  "lib/usd"     CACHE PATH "usd plugin dir")
set(USDGEN_INSTALL_PYTHONDIR  "lib/python"  CACHE PATH "python pkg dir")
set(USDGEN_INSTALL_CMAKEDIR   "lib/cmake/usdGen" CACHE PATH "cmake config dir")

# ---------------------------------------------------------------------------
# Third-party: nanoflann (header-only, INTERFACE target)
# ---------------------------------------------------------------------------
add_library(nanoflann INTERFACE)
target_include_directories(nanoflann SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nanoflann>
    $<INSTALL_INTERFACE:include>)
# N-7 (plan §1.2/§2.3): nanoflann's header is never installed. It is a
# private dependency of the usdGenMath archive (compiled hidden, stripped
# out of libusdGen.so by --exclude-libs,ALL), the install-tree test asserts
# no include/nanoflann* exists in the prefix, and the target is not in the
# export set either — so no install rule is emitted at all (supersedes the
# sol S-6 header-layout fix; the INSTALL_INTERFACE root above is unused by
# design).

# ---------------------------------------------------------------------------
# Third-party: SeExpr noise (M0 = Noise.cpp + headers only, S38)
# ---------------------------------------------------------------------------
add_library(usdGen_seexpr STATIC
    thirdparty/seexpr/SeExpr2/Noise.cpp)
target_include_directories(usdGen_seexpr PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/seexpr>
    $<INSTALL_INTERFACE:include/seexpr>)
target_link_libraries(usdGen_seexpr PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(usdGen_seexpr PROPERTIES
    USDGEN_VENDORED ON
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# GPU parity: Noise.cpp must not contract; the CUDA kernel pins its bit patterns with --fmad=false (mirrors usdGenMath).
# N-7 (plan §1.2): SeExpr2 headers are never installed — usdGen_seexpr only
# exists to compile Noise.cpp into the hidden archives, is never installed
# or exported, so no header rule is emitted (supersedes the sol S-6
# double-nesting fix; the INSTALL_INTERFACE root above is unused by design).

# --- BEGIN vendored ptex (usdGen_ptex) ---
# ---------------------------------------------------------------------------
# Third-party: Ptex 2.4.3 + zlib 1.3.1 (plan/07-look-maps-expressions.md §6)
# ---------------------------------------------------------------------------
# The OpenUSD prefix has no Ptex, and Storm's Ptex path is mesh-only, so
# usdGen samples .ptx files itself (usdGen/maps/ptexMap.h). Both archives are
# private: linked into the usdGen DLL only, compiled hidden, never installed
# or exported, and their headers are build-tree includes only. zlib exists
# solely for Ptex, so only usdGen_ptex sees its include directory.
set(_usdgen_zlib_dir "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zlib")
add_library(usdGen_zlib STATIC
    "${_usdgen_zlib_dir}/adler32.c" "${_usdgen_zlib_dir}/compress.c"
    "${_usdgen_zlib_dir}/crc32.c" "${_usdgen_zlib_dir}/deflate.c"
    "${_usdgen_zlib_dir}/gzclose.c" "${_usdgen_zlib_dir}/gzlib.c"
    "${_usdgen_zlib_dir}/gzread.c" "${_usdgen_zlib_dir}/gzwrite.c"
    "${_usdgen_zlib_dir}/infback.c" "${_usdgen_zlib_dir}/inffast.c"
    "${_usdgen_zlib_dir}/inflate.c" "${_usdgen_zlib_dir}/inftrees.c"
    "${_usdgen_zlib_dir}/trees.c" "${_usdgen_zlib_dir}/uncompr.c"
    "${_usdgen_zlib_dir}/zutil.c")
target_include_directories(usdGen_zlib PRIVATE "${_usdgen_zlib_dir}")
# zconf.h is vendored as shipped; HAVE_UNISTD_H is what upstream's configure
# step would have written into it on POSIX hosts (gzlib.c needs lseek/read).
if(MSVC)
    target_compile_definitions(usdGen_zlib PRIVATE
        _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
elseif(NOT WIN32)
    target_compile_definitions(usdGen_zlib PRIVATE HAVE_UNISTD_H)
endif()
set_target_properties(usdGen_zlib PROPERTIES
    USDGEN_VENDORED ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)

set(_usdgen_ptex_dir "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ptex/ptex")
add_library(usdGen_ptex STATIC
    "${_usdgen_ptex_dir}/PtexCache.cpp"
    "${_usdgen_ptex_dir}/PtexFilters.cpp"
    "${_usdgen_ptex_dir}/PtexHalf.cpp"
    "${_usdgen_ptex_dir}/PtexReader.cpp"
    "${_usdgen_ptex_dir}/PtexSeparableFilter.cpp"
    "${_usdgen_ptex_dir}/PtexSeparableKernel.cpp"
    "${_usdgen_ptex_dir}/PtexTriangleFilter.cpp"
    "${_usdgen_ptex_dir}/PtexTriangleKernel.cpp"
    "${_usdgen_ptex_dir}/PtexUtils.cpp"
    "${_usdgen_ptex_dir}/PtexWriter.cpp")
# SYSTEM keeps Ptexture.h (which pulls in <Windows.h>) at the consumer's
# external warning level; BUILD_INTERFACE only, because nothing installs it.
target_include_directories(usdGen_ptex SYSTEM PUBLIC
    "$<BUILD_INTERFACE:${_usdgen_ptex_dir}>")
target_include_directories(usdGen_ptex PRIVATE "${_usdgen_zlib_dir}")
# PTEX_STATIC drops the dllimport/visibility attributes from PTEXAPI; it and
# PTEX_VENDOR (namespace Ptex::v2_4_usdGen, so a site's own Ptex in the same
# process cannot collide) must match in every TU that includes Ptexture.h.
target_compile_definitions(usdGen_ptex PUBLIC PTEX_STATIC PTEX_VENDOR=usdGen)
if(MSVC)
    target_compile_definitions(usdGen_ptex PRIVATE
        _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
elseif(NOT WIN32)
    target_compile_definitions(usdGen_ptex PRIVATE HAVE_UNISTD_H)
endif()
target_link_libraries(usdGen_ptex
    PUBLIC Threads::Threads
    PRIVATE usdGen_zlib)
set_target_properties(usdGen_ptex PROPERTIES
    USDGEN_VENDORED ON
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# --- END vendored ptex (usdGen_ptex) ---

# ---------------------------------------------------------------------------
# usdGenMath (STATIC, -ffp-contract=off)
# ---------------------------------------------------------------------------
set(_usdgen_frontend_dir "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/seexprFrontend/SeExpr2")
file(GLOB _usdgen_frontend_sources "${_usdgen_frontend_dir}/*.cpp")
list(FILTER _usdgen_frontend_sources EXCLUDE REGEX "ExprLLVMCodeGeneration\\.cpp$")
add_library(usdGenExprFrontend STATIC ${_usdgen_frontend_sources}
    "${_usdgen_frontend_dir}/generated/ExprParser.cpp"
    "${_usdgen_frontend_dir}/generated/ExprParserLex.cpp")
target_include_directories(usdGenExprFrontend PRIVATE
    "${_usdgen_frontend_dir}" "${_usdgen_frontend_dir}/generated")
target_compile_definitions(usdGenExprFrontend PRIVATE
    SeExpr2=UsdGenSeExprFrontend SeExprInternal2=UsdGenSeExprFrontendInternal
    USDGEN_SEEXPR_SANDBOX)
# The vendored SeExpr frontend uses its own platform switch rather than the
# compiler's _WIN32 macro in a few generated and plugin-loader sources.  In
# particular, without this definition MSVC tries to include dlfcn.h and
# unistd.h. Keep this target-local so its public API remains platform-neutral.
if(WIN32)
    target_compile_definitions(usdGenExprFrontend PRIVATE SEEXPR_WIN32)
endif()
target_link_libraries(usdGenExprFrontend PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(usdGenExprFrontend PROPERTIES
    USDGEN_VENDORED ON
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

file(GLOB_RECURSE USDGEN_MATH_SOURCES libs/usdGenMath/*.cpp)
add_library(usdGenMath STATIC ${USDGEN_MATH_SOURCES})
target_include_directories(usdGenMath PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(usdGenMath PUBLIC
    arch tf gf vt work
    TBB::tbb
    nanoflann
    usdGen_seexpr)
if(TARGET rigExec::rigExecMath)
    target_link_libraries(usdGenMath PUBLIC rigExec::rigExecMath)
endif()
set_target_properties(usdGenMath PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# GPU parity: noise.cpp computes the Capture field the kernel replicates bit-exactly; no contraction (mirrors usdGenMath).
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-ffp-contract=${USDGEN_FP_CONTRACT}" USDGEN_FP_CONTRACT_OK)
if(USDGEN_FP_CONTRACT_OK)
    target_compile_options(usdGen_seexpr PRIVATE
        "-ffp-contract=${USDGEN_FP_CONTRACT}")
    target_compile_options(usdGenMath PRIVATE
        "-ffp-contract=${USDGEN_FP_CONTRACT}")
    set_source_files_properties(libs/usdGen/usdGen/ops/noise.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=${USDGEN_FP_CONTRACT}")
    # Wind evaluates the same SeExpr FBM gust field; keep its contraction
    # contract identical to noise.cpp so CPU results match across TUs.
    set_source_files_properties(libs/usdGen/usdGen/ops/wind.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=${USDGEN_FP_CONTRACT}")
endif()

# ---------------------------------------------------------------------------
# usdGen (SHARED — the engine core, no usd/usdImaging/hd linkage)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_CORE_SOURCES libs/usdGen/*.cpp)
# exprEval.{h,cpp} (a stale SeExpr2-engine evaluator design) and its unwired
# tests/testUsdGenExpr.cpp are deleted: the IR interpreter in
# expressions/irExec.h is the design, shared by the CUDA kernel and by
# expressions/cpuEvaluator.cpp.
list(REMOVE_ITEM USDGEN_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/executionResources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/executionRetirement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/executionBackendCudaProvider.cpp"
    # CUDA-only helper exposes CUDA vector types; do not compile it in the
    # CPU-only core build selected by ordinary consumers.
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/cudaScatterInput.cpp")
add_library(usdGenExecutionResources SHARED
    libs/usdGen/usdGen/executionResources.cpp
    libs/usdGen/usdGen/executionRetirement.cpp)
target_include_directories(usdGenExecutionResources PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(usdGenExecutionResources PUBLIC TBB::tbb)
set_target_properties(usdGenExecutionResources PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
    target_link_options(usdGenExecutionResources PRIVATE "LINKER:--exclude-libs,ALL")
endif()
add_library(usdGen SHARED ${USDGEN_CORE_SOURCES})
target_sources(usdGen PRIVATE
    libs/usdGen/usdGen/executionBackendCudaProvider.cpp)
set_source_files_properties(libs/usdGen/usdGen/expressions/frontend.cpp PROPERTIES
    COMPILE_DEFINITIONS "SeExpr2=UsdGenSeExprFrontend;SeExprInternal2=UsdGenSeExprFrontendInternal;USDGEN_SEEXPR_SANDBOX"
    INCLUDE_DIRECTORIES "${_usdgen_frontend_dir}")
target_include_directories(usdGen PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen>
    $<INSTALL_INTERFACE:include>)
# Public headers expose SdfPath/TfToken/GfVec3f/VtArray, so those base libs
# must be PUBLIC for consumers (tests) to link them transitively; sdf in
# particular is required by SdfPath in the usdGen graph/compiler headers.
# usdGenMath is PRIVATE (N-7, plan §1.2): it is not installed, so it must
# not sit in usdGen's export interface (install(EXPORT) would then reference
# a non-installed target); usdGen's own sources still see its headers
# through the link, and no public header includes usdGenMath/...
target_link_libraries(usdGen
    PUBLIC tf gf vt sdf TBB::tbb usdGenExecutionResources
    PRIVATE usdGenMath arch js plug ts ar work trace hio pxOsd
            usdGen_seexpr usdGenExprFrontend)
# Vendored Ptex (usdGen/maps/ptexMap.cpp); private like the other archives.
target_link_libraries(usdGen PRIVATE usdGen_ptex)
# usdGen is a SHARED plugin-host library: its public API (UsdGenSession,
# graph/tile types, ...) is consumed across the shared boundary by
# libusdGenImaging.so, so it must keep default visibility. Third-party code
# stays hidden: usdGenMath and usdGen_seexpr are compiled with
# CXX_VISIBILITY_PRESET hidden, and --exclude-libs,ALL keeps their symbols
# out of libusdGen.so's export table (gate B-1 NoThirdPartyExports).
set_target_properties(usdGen PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
    # The Linux build keeps its C++ API visible by default. Windows requires
    # an explicit export table for the same API so usdGenImaging can link
    # against the core DLL; CMake ignores this property elsewhere.
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
    target_link_options(usdGen PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# ---------------------------------------------------------------------------
# usdGenSchema (SHARED, minimal — tokens + extent registration)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_SCHEMA_SOURCES libs/usdGenSchema/*.cpp)
add_library(usdGenSchema SHARED ${USDGEN_SCHEMA_SOURCES})
target_include_directories(usdGenSchema PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenSchema>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(usdGenSchema
    PUBLIC usd
    PRIVATE tf usdGeom usdGenMath)

# ---------------------------------------------------------------------------
# usdGenImaging (SHARED — scene-index plugins + adapters)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_IMAGING_SOURCES libs/usdGenImaging/*.cpp)
list(FILTER USDGEN_IMAGING_SOURCES EXCLUDE REGEX "usdGenGraphDescBuilderStage\\.cpp$")
add_library(usdGenImaging SHARED ${USDGEN_IMAGING_SOURCES})
target_include_directories(usdGenImaging PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenImaging>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(usdGenImaging
    PUBLIC hd hdsi hf usd usdGeom usdShade sdr usdImaging
           sdf tf gf vt trace pxOsd usdGen
    PRIVATE hio
    PRIVATE usdGenSchema)
set_target_properties(usdGenImaging PROPERTIES
    CXX_VISIBILITY_PRESET default
    # Produce the import library that downstream Windows consumers need.
    # This is ignored by non-Windows generators.
    WINDOWS_EXPORT_ALL_SYMBOLS ON)  # plugins need exported symbols

# M1: the usdGenShaders SdrDiscoveryPlugin compiles into usdGenImaging; its
# resource dir is resolved at runtime by name (PlugRegistry lookup of
# "usdGenShaders"), so no per-target macro is needed (10-build-dependencies-
# testing.md §4.4 alternative).

# ---------------------------------------------------------------------------
# usdGenTonic (SHARED — plan/17 Tonic authoring model + scene index, P0)
#
# TonicModel, the tonicApi C ABI, the host-staged transport, the selection
# and overlay state (tonicSelection.cpp, tonicGizmo.cpp: plan/18 V1), the
# UsdGenTonicSceneIndexPlugin/SceneIndex (libs/usdGenTonic/usdGenTonic/,
# imaging under imaging/) and the P1 TonicCommitter (tonicCommit.cpp, the
# only TU that touches usd, for hydrate and Save groom; gate B-1 stays true
# for usdGen itself). Links usdGen only for the gpu/ primitives
# (DeviceBuffer, execution resources). The CUDA kernel (tonicKernels.cu)
# joins below in the USDGEN_ENABLE_CUDA branch, next to the usdGenGpu wiring.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_TONIC_SOURCES libs/usdGenTonic/*.cpp)
list(FILTER USDGEN_TONIC_SOURCES EXCLUDE REGEX "\\.cu$")
add_library(usdGenTonic SHARED ${USDGEN_TONIC_SOURCES})
target_include_directories(usdGenTonic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenTonic>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(usdGenTonic
    PUBLIC hd sdf tf gf vt
    PRIVATE usd usdGeom hio usdGen usdGen_ptex usdGenImaging)
set_target_properties(usdGenTonic PROPERTIES
    CXX_VISIBILITY_PRESET default
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

# ---------------------------------------------------------------------------
# usdGenTestUtils (STATIC, T0 — no Hydra)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_TESTUTILS_SOURCES testutils/usdGenTestUtils/*.cpp)
add_library(usdGenTestUtils STATIC ${USDGEN_TESTUTILS_SOURCES})
target_include_directories(usdGenTestUtils PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/testutils")
target_link_libraries(usdGenTestUtils PUBLIC usdGen)

# ---------------------------------------------------------------------------
# usdGenTestUtilsHd (STATIC, T1/T2/T3 — Hydra allowed)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_TESTUTILS_HD_SOURCES
    testutils/usdGenTestUtilsHd/*.cpp)
add_library(usdGenTestUtilsHd STATIC ${USDGEN_TESTUTILS_HD_SOURCES})
target_sources(usdGenTestUtilsHd PRIVATE
    libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilderStage.cpp)
target_include_directories(usdGenTestUtilsHd PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/testutils")
target_link_libraries(usdGenTestUtilsHd PUBLIC
    usdGenImaging usdGenTestUtils
    hd hdsi usd usdImaging usdImagingGL hdSt hgiGL usdAppUtils hio)

# ---------------------------------------------------------------------------
# Build-tree plugin resources (generated at configure time so that the
# $<TARGET_FILE_NAME:*> generator expressions are expanded; mirrors the
# usdRig pattern of build/usd/<name>/resources).
# ---------------------------------------------------------------------------
set(_usdgen_schema_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenSchema/resources")
set(_usdgen_schema_pluginfo "${_usdgen_schema_resources}/plugInfo.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/plugInfo.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/generatedSchema.usda")
file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/plugInfo.json"
    _usdgen_schema_pluginfo_content)
string(REPLACE "\"Type\": \"resource\"" "\"Type\": \"library\""
    _usdgen_schema_pluginfo_content "${_usdgen_schema_pluginfo_content}")
string(REPLACE
    "\"schemaIdentifier\": \"UsdGenDescription\""
    "\"implementsComputeExtent\": true, \"schemaIdentifier\": \"UsdGenDescription\""
    _usdgen_schema_pluginfo_content "${_usdgen_schema_pluginfo_content}")
string(REPLACE "\"Name\": \"usdGenSchema\""
    "\"LibraryPath\": \"../../../$<TARGET_FILE_NAME:usdGenSchema>\", \"Name\": \"usdGenSchema\""
    _usdgen_schema_pluginfo_content "${_usdgen_schema_pluginfo_content}")
file(GENERATE OUTPUT "${_usdgen_schema_pluginfo}"
    CONTENT "${_usdgen_schema_pluginfo_content}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/generatedSchema.usda"
    DESTINATION "${_usdgen_schema_resources}")

set(_usdgen_imaging_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenImaging/resources")
set(_usdgen_imaging_pluginfo "${_usdgen_imaging_resources}/plugInfo.json")
file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenImaging/resources/plugInfo.json.in"
    _usdgen_imaging_pluginfo_content)
file(GENERATE OUTPUT "${_usdgen_imaging_pluginfo}"
    CONTENT "${_usdgen_imaging_pluginfo_content}")

# ---------------------------------------------------------------------------
# usdGenShaders (9th target, M1 — plan §1.2/§1.6): the usdGen hair preview
# glslfx shader defs + resources. No C++ of its own: the discovery plugin
# (UsdGenShadersDiscoveryPlugin, base SdrDiscoveryPlugin) is compiled into
# usdGenImaging, so this is an INTERFACE target carrying the resource
# installation. plugInfo points LibraryPath at libusdGenImaging and
# ShaderResources at shaders/ (mirrors stock usdShaders; 10 §4.4).
# ---------------------------------------------------------------------------
add_library(usdGenShaders INTERFACE)
add_library(usdGen::usdGenShaders ALIAS usdGenShaders)

# Offline authoring utility; leaves PointInstancer geometry instanced.
add_executable(usdGenBakeFur tools/bakeFur.cpp)
target_link_libraries(usdGenBakeFur PRIVATE usdGen usdGeom usd)
install(TARGETS usdGenBakeFur RUNTIME DESTINATION bin)
# Profiling utility: plays a scene through the groom scene index frame by
# frame and reports per-frame cost, per-operator work and a Chrome trace
# (bin/trace_playback.ps1).
add_executable(usdGenTracePlayback tools/tracePlayback.cpp)
target_link_libraries(usdGenTracePlayback PRIVATE
    usdGen usdGenImaging usdImaging hd trace tf usd sdf)
install(TARGETS usdGenTracePlayback RUNTIME DESTINATION bin)
# Offline authoring utility; writes procedural region/clump Ptex maps for a mesh.
add_executable(usdGenBakePtex tools/bakePtex.cpp)
target_link_libraries(usdGenBakePtex PRIVATE usdGen usdGen_ptex usdGeom usd)
install(TARGETS usdGenBakePtex RUNTIME DESTINATION bin)

set(_usdgen_shader_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenShaders/resources")
set(_usdgen_shader_pluginfo "${_usdgen_shader_resources}/plugInfo.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/resources/plugInfo.json.in")
file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/resources/plugInfo.json.in"
    _usdgen_shader_pluginfo_content)
file(GENERATE OUTPUT "${_usdgen_shader_pluginfo}"
    CONTENT "${_usdgen_shader_pluginfo_content}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/resources/shaders/"
    DESTINATION "${_usdgen_shader_resources}/shaders")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/resources/hairLook.usda"
    DESTINATION "${_usdgen_shader_resources}")

# ---------------------------------------------------------------------------
# usdGenTools: usdview plugin (SeExpr expression editor). Pure Python. The
# resources dir carries the Type "python" plugInfo that usdview discovers on
# PXR_PLUGINPATH_NAME; the package itself is staged into build/python, which
# the launchers put on PYTHONPATH (plan/08 §5).
# ---------------------------------------------------------------------------
set(_usdgen_tools_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenTools/resources")
file(GLOB_RECURSE _usdgen_tools_python CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/python/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/resources/plugInfo.json"
    ${_usdgen_tools_python})
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/resources/plugInfo.json"
    DESTINATION "${_usdgen_tools_resources}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/python/usdGenTools"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/python"
    FILES_MATCHING PATTERN "*.py")
# The shipped expression presets the editor's library browser reads. They sit
# beside plugInfo.json so exprLibrary finds them by walking up from the staged
# package, in the build tree and in the install tree alike.
file(GLOB _usdgen_tools_expressions
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/resources/expressions/*.se")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_usdgen_tools_expressions})
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/resources/expressions"
    DESTINATION "${_usdgen_tools_resources}"
    FILES_MATCHING PATTERN "*.se")
# The brush palette's icons (256x256 RGBA PNGs), resolved the same way as the
# expression presets: beside plugInfo.json under resources/icons.
file(GLOB _usdgen_tools_icons CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/resources/icons/*.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_usdgen_tools_icons})
if(_usdgen_tools_icons)
    file(COPY ${_usdgen_tools_icons}
        DESTINATION "${_usdgen_tools_resources}/icons")
endif()

# ---------------------------------------------------------------------------
# usdGenTonic scene-index plugin resources + usdGenTonicTools usdview plugin
# (plan/17 P0). The scene index registers through build-tree plugInfo like
# the groom index; the Python shelf stages into build/python like usdGenTools.
# ---------------------------------------------------------------------------
set(_usdgen_tonic_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenTonic/resources")
set(_usdgen_tonic_pluginfo "${_usdgen_tonic_resources}/plugInfo.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonic/resources/plugInfo.json.in")
file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonic/resources/plugInfo.json.in"
    _usdgen_tonic_pluginfo_content)
file(GENERATE OUTPUT "${_usdgen_tonic_pluginfo}"
    CONTENT "${_usdgen_tonic_pluginfo_content}")

set(_usdgen_tonic_tools_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenTonicTools/resources")
# CONFIGURE_DEPENDS: the package is staged by file(COPY) at CONFIGURE time,
# and the CMAKE_CONFIGURE_DEPENDS list below is this glob's result from the
# PREVIOUS configure -- so editing an existing module re-stages, but adding
# a module never does, and the tests keep importing a package without it.
# That cost V4 and V5 a false T3 failure each; the flag re-evaluates the
# glob at build time and re-configures when the file set changes.
file(GLOB_RECURSE _usdgen_tonic_tools_python CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/python/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/resources/plugInfo.json"
    ${_usdgen_tonic_tools_python})
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/resources/plugInfo.json"
    DESTINATION "${_usdgen_tonic_tools_resources}")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/python/usdGenTonicTools"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/python"
    FILES_MATCHING PATTERN "*.py")
# Tonic's shelf/toolbar icons (IC-01), staged the same way as the brush
# palette's under usdGenTools (L604-612 above): white PNGs beside
# plugInfo.json under resources/icons, resolved by tonicIcons.iconDir().
file(GLOB _usdgen_tonic_tools_icons CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/resources/icons/*.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_usdgen_tonic_tools_icons})
if(_usdgen_tonic_tools_icons)
    file(COPY ${_usdgen_tonic_tools_icons}
        DESTINATION "${_usdgen_tonic_tools_resources}/icons")
endif()

# ---------------------------------------------------------------------------
# CUDA foundations and legacy optional GPU noise
# ---------------------------------------------------------------------------
# SeExpr2::FBM<3,1,false,float> (noise.cpp:181-277) gains a CUDA kernel
# (noise_gpu.cu, landed by a later step). Default OFF: without it no CUDA
# compiler, toolkit, or language is involved, so configure stays green on
# GPU-less CI hosts.
option(USE_GPU_NOISE
    "Build the CUDA noise kernel for usdGen noise ops (requires CUDA toolchain)" OFF)
option(USDGEN_ENABLE_CUDA
    "Build persistent CUDA buffers and RBF execution (requires CUDA toolkit)" OFF)
if(USE_GPU_NOISE OR USDGEN_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER)
        message(FATAL_ERROR "usdGen CUDA requested but no CUDA compiler was found; "
            "install a CUDA toolkit or set CMAKE_CUDA_COMPILER")
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    # Exact float op order vs the CPU FBM path (04 §2.9): no fused FMA.
    # 1394-D is nvcc's spelling of C4251 (see the MSVC warning block above).
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --fmad=false -diag-suppress 1394")
    # nvcc establishes its own Visual Studio environment by re-running
    # vcvars on every invocation; nested inside an already-configured shell
    # that re-entry overflows cmd's line limit ("input line too long") and
    # every kernel compile dies before it starts. Kernel builds only ever
    # run where cl.exe already works, so hand nvcc that same environment
    # instead of a broken nested one (the arch probe below makes the same
    # choice per configure environment).
    if(MSVC)
        set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --use-local-env")
    endif()
    foreach(_config RELEASE RELWITHDEBINFO MINSIZEREL)
        string(REGEX REPLACE "[/-]DNDEBUG" "" CMAKE_CUDA_FLAGS_${_config}
            "${CMAKE_CUDA_FLAGS_${_config}}")
    endforeach()
    # Configure-time arch probe. The GPU in this host decides: nvidia-smi
    # reports its compute capability (12.1 on the GB10 dev host, 8.9 on an
    # Ada workstation), and that arch is tried first, so a checkout builds
    # kernels the local card can actually run. The GB10 pair stays on as the
    # fallback for a host with the toolkit but no queryable GPU. A trivial
    # kernel is compiled directly with nvcc per arch (-gencode); first
    # success wins, which also rejects an arch this toolkit is too old for.
    set(_usdgen_cuda_archs "")
    find_program(USDGEN_NVIDIA_SMI nvidia-smi)
    if(USDGEN_NVIDIA_SMI)
        execute_process(
            COMMAND "${USDGEN_NVIDIA_SMI}" --query-gpu=compute_cap
                    --format=csv,noheader
            OUTPUT_VARIABLE _usdgen_compute_caps
            RESULT_VARIABLE _usdgen_smi_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_usdgen_smi_rc EQUAL 0)
            string(REPLACE "\n" ";" _usdgen_compute_caps "${_usdgen_compute_caps}")
            foreach(_cap IN LISTS _usdgen_compute_caps)
                # "8.9" -> 89. A driver too old to answer prints "[N/A]";
                # anything that is not major.minor is dropped, not fed to nvcc.
                if(_cap MATCHES "([0-9]+)\\.([0-9]+)")
                    list(APPEND _usdgen_cuda_archs
                        "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
                endif()
            endforeach()
        endif()
    endif()
    list(APPEND _usdgen_cuda_archs 121 120)
    list(REMOVE_DUPLICATES _usdgen_cuda_archs)
    file(WRITE "${CMAKE_BINARY_DIR}/usdgen_cuda_arch_probe.cu"
        "__global__ void usdgen_cuda_arch_probe() {}\n")
    set(USDGEN_CUDA_ARCH "")
    set(_usdgen_probe_local_env "")
    if(DEFINED ENV{VCToolsInstallDir})
        # Configured from a Visual Studio shell: nvcc must reuse this
        # environment instead of re-running vcvars into it (see above).
        # From a clean shell the flag stays off and nvcc sets Visual
        # Studio up itself.
        set(_usdgen_probe_local_env "--use-local-env")
    endif()
    foreach(_arch IN LISTS _usdgen_cuda_archs)
        execute_process(
            COMMAND "${CMAKE_CUDA_COMPILER}" ${_usdgen_probe_local_env} -c
                    "-gencode=arch=compute_${_arch},code=sm_${_arch}"
                    "-o" "${CMAKE_BINARY_DIR}/usdgen_cuda_arch_probe.o"
                    "${CMAKE_BINARY_DIR}/usdgen_cuda_arch_probe.cu"
            RESULT_VARIABLE _usdgen_cuda_arch_rc
            OUTPUT_VARIABLE _usdgen_cuda_arch_out
            ERROR_VARIABLE _usdgen_cuda_arch_err)
        if(_usdgen_cuda_arch_rc EQUAL 0)
            set(USDGEN_CUDA_ARCH "${_arch}")
            break()
        endif()
        # Say why: "input line is too long" here means nvcc's nested vcvars
        # overflowed cmd (an over-long PATH), not a toolkit problem.
        message(STATUS "usdGen: sm_${_arch} probe failed: "
            "${_usdgen_cuda_arch_out}${_usdgen_cuda_arch_err}")
    endforeach()
    if(NOT USDGEN_CUDA_ARCH)
        string(REPLACE ";" ", sm_" _usdgen_cuda_archs_tried "${_usdgen_cuda_archs}")
        message(FATAL_ERROR "usdGen CUDA requested but no architecture in "
            "(sm_${_usdgen_cuda_archs_tried}) compiles with "
            "${CMAKE_CUDA_COMPILER} (CUDA ${CUDAToolkit_VERSION}); install a "
            "toolkit new enough for this host's GPU, or set "
            "CMAKE_CUDA_COMPILER at the one that is. Last nvcc output: "
            "${_usdgen_cuda_arch_out}${_usdgen_cuda_arch_err}")
    endif()
    set(CMAKE_CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}" CACHE STRING
        "CUDA arch for the noise kernel (probed at configure time)" FORCE)
    # usdGen (line 117) predates the variable, so its empty default was
    # frozen at creation: pin the property explicitly.
    set_target_properties(usdGen PROPERTIES CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}")
    message(STATUS "usdGen CUDA: ${CUDAToolkit_VERSION} (${CMAKE_CUDA_COMPILER}), arch sm_${USDGEN_CUDA_ARCH}")
    # The kernel lands at libs/usdGen/usdGen/ops/noise_gpu.cu (CUDA1b); wire
    # it into usdGen the moment the file exists (hook stays inert until then).
    if(USE_GPU_NOISE)
        target_sources(usdGen PRIVATE libs/usdGen/usdGen/ops/noise_gpu.cu)
        # Enables the #ifdef USDGEN_USE_GPU_NOISE branch in noise.cpp (04 §2.9).
        target_compile_definitions(usdGen PRIVATE USDGEN_USE_GPU_NOISE)
    endif()
    if(USDGEN_ENABLE_CUDA)
        target_sources(usdGen PRIVATE libs/usdGen/usdGen/cudaScatterInput.cpp)
        add_library(usdGenGpu STATIC
            libs/usdGen/usdGen/gpu/deviceBuffer.cu
            libs/usdGen/usdGen/gpu/deviceResources.cu
            libs/usdGen/usdGen/gpu/imageSampler.cu
            libs/usdGen/usdGen/gpu/rbf.cu
            libs/usdGen/usdGen/gpu/curveGeometry.cu
            libs/usdGen/usdGen/gpu/curveSource.cu
            libs/usdGen/usdGen/gpu/scatterGrow.cu
            libs/usdGen/usdGen/gpu/curveGrow.cu
            libs/usdGen/usdGen/gpu/expressionContext.cu
            libs/usdGen/usdGen/gpu/deformCurves.cu
            libs/usdGen/usdGen/gpu/styleOps.cu
            libs/usdGen/usdGen/gpu/width.cu
            libs/usdGen/usdGen/gpu/widthBlend.cu
            libs/usdGen/usdGen/gpu/widthOverlapWitness.cu
            libs/usdGen/usdGen/gpu/surfaceBinding.cu
            libs/usdGen/usdGen/gpu/length.cu
            libs/usdGen/usdGen/gpu/curveCompaction.cu
            libs/usdGen/usdGen/gpu/curveResample.cu
            libs/usdGen/usdGen/gpu/namedChannelTopology.cu
            libs/usdGen/usdGen/gpu/picking.cu
            libs/usdGen/usdGen/gpu/pointOverride.cu
            libs/usdGen/usdGen/gpu/topology.cu
            libs/usdGen/usdGen/gpu/nonWidthCompare.cu
            libs/usdGen/usdGen/gpu/curveIndices.cu
            libs/usdGen/usdGen/gpu/curveTiles.cu
            libs/usdGen/usdGen/gpu/curveTileBounds.cu
            libs/usdGen/usdGen/gpu/rootFrames.cu
            libs/usdGen/usdGen/gpu/rootFrameGather.cu
            libs/usdGen/usdGen/gpu/cudaRetirement.cu
            libs/usdGen/usdGen/gpu/noise.cu
            libs/usdGen/usdGen/gpu/expression.cu)
        target_include_directories(usdGenGpu PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen>)
        # cuda_driver backs the cudaStreamGetDevice shim in gpu/cudaCompat.h
        # on toolkits older than 12.8.
        target_link_libraries(usdGenGpu PUBLIC CUDA::cudart CUDA::cuda_driver CUDA::cusolver usdGenExecutionResources)
        target_compile_definitions(usdGen PRIVATE USDGEN_ENABLE_CUDA)
        target_link_libraries(usdGen PRIVATE usdGenGpu)
        target_compile_definitions(usdGenImaging PUBLIC USDGEN_HAS_CUDA_GL_INTEROP)
        target_link_libraries(usdGenImaging PUBLIC hdSt PRIVATE usdGenGpu hgiGL)
        set_target_properties(usdGenGpu PROPERTIES
            POSITION_INDEPENDENT_CODE ON CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}")
        # Tonic CUDA lane (plan/17 P0): the tessellation kernel joins
        # usdGenTonic, and the model/transport compile their device mirrors.
        target_sources(usdGenTonic PRIVATE
            libs/usdGenTonic/usdGenTonic/tonicKernels.cu)
        target_compile_definitions(usdGenTonic PUBLIC USDGEN_TONIC_HAS_CUDA)
        target_link_libraries(usdGenTonic PRIVATE usdGenGpu CUDA::cudart)
        set_target_properties(usdGenTonic PROPERTIES
            CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(WIN32)
    # The current test harnesses use POSIX process and loader APIs (setenv,
    # execv, dlopen, and ELF symbol inspection).  Keep the production targets
    # buildable with MSVC while those harnesses remain Unix-only.
    option(USDGEN_BUILD_TESTS "Build the usdGen test and benchmark targets" OFF)
else()
    option(USDGEN_BUILD_TESTS "Build the usdGen test and benchmark targets" ON)
endif()


if(USDGEN_BUILD_TESTS)
enable_testing()

# Some harnesses are inherently Unix-only: the POSIX process/loader/ELF tests
# (execv/setenv/LD_LIBRARY_PATH, ELF symbol inspection) and the EGL/Storm GPU
# tests (dlopen + a headless EGL device) include unistd.h/dlfcn.h, so MSVC
# cannot compile them, and the bash-script checks need a POSIX shell. Define
# the executable only where those APIs exist; on other hosts register an honest
# ctest skip so the test-name and gate-label set stays identical across
# platforms instead of breaking the build. Portable targets keep calling
# add_test() directly.
function(usdgen_add_unix_only_test name)
    if(WIN32)
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -E echo
            "SKIP ${name} (Unix-only harness: POSIX process/loader, shell, or EGL/Storm)")
        set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP")
    else()
        add_test(NAME ${name} COMMAND ${ARGN})
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Test environment plumbing (cross-platform)
#
# CTest's ENVIRONMENT property is the wrong tool for path lists, and on
# Windows it is actively wrong twice over:
#
#   * ENVIRONMENT *replaces* a variable. The host loader variable there is
#     PATH itself, so "PATH=<usd>/lib" strips the inherited system PATH from
#     the test process instead of extending it.
#   * ENVIRONMENT is a CMake list, i.e. ';'-separated, and ';' is also the
#     native path-list separator on Windows. A multi-directory value silently
#     splits into extra list entries, so the variable is truncated after its
#     first directory and the remaining directories become junk entries.
#
# ENVIRONMENT_MODIFICATION (CMake >= 3.22) avoids both: it edits the
# inherited value rather than replacing it, and path_list_* joins with the
# host's native separator, so each operation carries exactly one directory
# and no value ever embeds a ';'.
# ---------------------------------------------------------------------------

# Emit one ENVIRONMENT_MODIFICATION operation per directory.
function(usdgen_test_path_list_ops out_var name op)
    set(_ops "")
    foreach(_dir IN LISTS ARGN)
        list(APPEND _ops "${name}=${op}:${_dir}")
    endforeach()
    set(${out_var} "${_ops}" PARENT_SCOPE)
endfunction()

# PXR_PLUGINPATH_NAME is rebuilt from scratch ("set:" on the first directory)
# so a value already exported in the developer's shell cannot take precedence
# over the build-tree plugin resources. The rest append in declaration order,
# which is the order the plugInfo ordering contract (gate SI-5) asserts.
function(usdgen_test_pluginpath_ops out_var)
    set(_ops "")
    foreach(_dir IN LISTS ARGN)
        if(_ops STREQUAL "")
            list(APPEND _ops "PXR_PLUGINPATH_NAME=set:${_dir}")
        else()
            list(APPEND _ops "PXR_PLUGINPATH_NAME=path_list_append:${_dir}")
        endif()
    endforeach()
    set(${out_var} "${_ops}" PARENT_SCOPE)
endfunction()

# Directories a test binary must be able to load shared libraries from.
# Windows has no RPATH, so PATH is the only mechanism there; ELF/Mach-O builds
# already resolve the build tree through CMake's build RPATH, but naming the
# OpenUSD prefix is required on every host, so one list serves all three.
# OpenUSD's Windows install splits its runtime in two: the usd_*.dll core
# lands in lib/ while the bundled third-party DLLs (tbb, embree, ...) land in
# bin/, so both directories are needed there.
set(USDGEN_TEST_RUNTIME_DIRS "${USD_INSTALL_DIR}/lib")
if(WIN32)
    list(APPEND USDGEN_TEST_RUNTIME_DIRS "${USD_INSTALL_DIR}/bin")
endif()
list(APPEND USDGEN_TEST_RUNTIME_DIRS "$<TARGET_FILE_DIR:usdGen>")

add_executable(testUsdGenExecutionPipeline tests/testUsdGenExecutionPipeline.cpp)
target_link_libraries(testUsdGenExecutionPipeline PRIVATE usdGen)
add_test(NAME testUsdGenExecutionPipeline COMMAND testUsdGenExecutionPipeline)
set_tests_properties(testUsdGenExecutionPipeline PROPERTIES LABELS "T1;scheduling;sessions" TIMEOUT 30)
add_executable(testUsdGenExecutionSequenceWindow tests/testUsdGenExecutionSequenceWindow.cpp)
target_link_libraries(testUsdGenExecutionSequenceWindow PRIVATE usdGen)
add_test(NAME testUsdGenExecutionSequenceWindow COMMAND testUsdGenExecutionSequenceWindow)
set_tests_properties(testUsdGenExecutionSequenceWindow PROPERTIES LABELS "T0;scheduling;resources" TIMEOUT 30)
add_executable(testUsdGenExecutionTaskGraph tests/testUsdGenExecutionTaskGraph.cpp)
target_link_libraries(testUsdGenExecutionTaskGraph PRIVATE usdGen)
add_test(NAME testUsdGenExecutionTaskGraph COMMAND testUsdGenExecutionTaskGraph)
set_tests_properties(testUsdGenExecutionTaskGraph PROPERTIES LABELS "T1;scheduling" TIMEOUT 30)
add_executable(testUsdGenExecutionCache tests/testUsdGenExecutionCache.cpp)
target_link_libraries(testUsdGenExecutionCache PRIVATE usdGen)
add_test(NAME testUsdGenExecutionCache COMMAND testUsdGenExecutionCache)
set_tests_properties(testUsdGenExecutionCache PROPERTIES LABELS "T1;scheduling" TIMEOUT 30)
add_executable(testUsdGenExecutionBackend tests/testUsdGenExecutionBackend.cpp)
target_link_libraries(testUsdGenExecutionBackend PRIVATE usdGen)
if(USDGEN_ENABLE_CUDA)
    target_compile_definitions(testUsdGenExecutionBackend PRIVATE USDGEN_ENABLE_CUDA)
endif()
add_test(NAME testUsdGenExecutionBackend COMMAND testUsdGenExecutionBackend)
set_tests_properties(testUsdGenExecutionBackend PROPERTIES LABELS "T0;backend" TIMEOUT 30)
add_executable(testUsdGenExecutionBackendNeutral
    tests/testUsdGenExecutionBackendNeutral.cpp)
target_link_libraries(testUsdGenExecutionBackendNeutral PRIVATE usdGen)
add_test(NAME testUsdGenExecutionBackendNeutral
    COMMAND testUsdGenExecutionBackendNeutral)
set_tests_properties(testUsdGenExecutionBackendNeutral
    PROPERTIES LABELS "T0;backend" TIMEOUT 30)
add_executable(testUsdGenExecutionTaskGraphStress tests/testUsdGenExecutionTaskGraphStress.cpp)
target_link_libraries(testUsdGenExecutionTaskGraphStress PRIVATE usdGen)
add_test(NAME testUsdGenExecutionTaskGraphStress COMMAND testUsdGenExecutionTaskGraphStress)
set_tests_properties(testUsdGenExecutionTaskGraphStress PROPERTIES LABELS "T1;scheduling" TIMEOUT 45)
add_executable(testUsdGenExecutionResources tests/testUsdGenExecutionResources.cpp)
target_link_libraries(testUsdGenExecutionResources PRIVATE usdGenExecutionResources)
add_test(NAME testUsdGenExecutionResources COMMAND testUsdGenExecutionResources)
set_tests_properties(testUsdGenExecutionResources PROPERTIES LABELS "T0;resources" TIMEOUT 30)
add_executable(testUsdGenExecutionRetirement tests/testUsdGenExecutionRetirement.cpp)
target_link_libraries(testUsdGenExecutionRetirement PRIVATE usdGenExecutionResources)
add_test(NAME testUsdGenExecutionRetirement COMMAND testUsdGenExecutionRetirement)
set_tests_properties(testUsdGenExecutionRetirement PROPERTIES LABELS "T0;resources;retirement" TIMEOUT 30)
add_executable(testUsdGenAsyncSession tests/testUsdGenAsyncSession.cpp)
target_link_libraries(testUsdGenAsyncSession PRIVATE usdGen)
add_test(NAME testUsdGenAsyncSession COMMAND testUsdGenAsyncSession)
set_tests_properties(testUsdGenAsyncSession PROPERTIES LABELS "T1;scheduling;sessions" TIMEOUT 60)
add_executable(testUsdGenAsyncImagingSession tests/testUsdGenAsyncImagingSession.cpp)
target_link_libraries(testUsdGenAsyncImagingSession PRIVATE usdGenImaging usdGen)
target_compile_definitions(testUsdGenAsyncImagingSession PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenAsyncImagingSession COMMAND testUsdGenAsyncImagingSession)
set_tests_properties(testUsdGenAsyncImagingSession PROPERTIES LABELS "T1;scheduling;sessions;imaging" TIMEOUT 60)
add_executable(testUsdGenSessionStoreOwner tests/testUsdGenSessionStoreOwner.cpp)
target_link_libraries(testUsdGenSessionStoreOwner PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenSessionStoreOwner COMMAND testUsdGenSessionStoreOwner)
set_tests_properties(testUsdGenSessionStoreOwner PROPERTIES LABELS "T1;scheduling;sessions;imaging" TIMEOUT 60 RUN_SERIAL TRUE)
add_test(NAME testUsdGenSessionStoreExit COMMAND testUsdGenSessionStoreOwner --pending-exit)
set_tests_properties(testUsdGenSessionStoreExit PROPERTIES LABELS "T1;scheduling;sessions;imaging" TIMEOUT 60)
add_executable(testUsdGenRegistryOwners tests/testUsdGenRegistryOwners.cpp)
target_link_libraries(testUsdGenRegistryOwners PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenRegistryOwners COMMAND testUsdGenRegistryOwners)
set_tests_properties(testUsdGenRegistryOwners PROPERTIES LABELS "T1;scheduling;imaging" TIMEOUT 60)
add_executable(testUsdGenSceneOwner tests/testUsdGenSceneOwner.cpp)
add_executable(testUsdGenDirtyRouterOrder tests/testUsdGenDirtyRouterOrder.cpp)
target_link_libraries(testUsdGenDirtyRouterOrder PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenDirtyRouterOrder COMMAND testUsdGenDirtyRouterOrder)
set_tests_properties(testUsdGenDirtyRouterOrder PROPERTIES
    LABELS "T1;scheduling;imaging;dirty-router;order" TIMEOUT 60)
add_executable(testUsdGenDirtyRouterPaint tests/testUsdGenDirtyRouterPaint.cpp)
target_link_libraries(testUsdGenDirtyRouterPaint PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenDirtyRouterPaint COMMAND testUsdGenDirtyRouterPaint)
set_tests_properties(testUsdGenDirtyRouterPaint PROPERTIES
    LABELS "T1;scheduling;imaging;dirty-router;paint" TIMEOUT 60)
add_executable(testUsdGenDirtyRouterDensity tests/testUsdGenDirtyRouterDensity.cpp)
target_link_libraries(testUsdGenDirtyRouterDensity PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenDirtyRouterDensity COMMAND testUsdGenDirtyRouterDensity)
set_tests_properties(testUsdGenDirtyRouterDensity PROPERTIES
    LABELS "T1;scheduling;imaging;dirty-router;density" TIMEOUT 60)
add_executable(testUsdGenIncrementalCapture tests/testUsdGenIncrementalCapture.cpp)
add_executable(testUsdGenCaptureCache tests/testUsdGenCaptureCache.cpp)
add_executable(testUsdGenAsyncScenePublication tests/testUsdGenAsyncScenePublication.cpp)
target_link_libraries(testUsdGenAsyncScenePublication PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenAsyncScenePublication COMMAND testUsdGenAsyncScenePublication)
set_tests_properties(testUsdGenAsyncScenePublication PROPERTIES
    LABELS "T1;scheduling;imaging;async-publication" TIMEOUT 60)
target_link_libraries(testUsdGenCaptureCache PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenCaptureCache COMMAND testUsdGenCaptureCache)
set_tests_properties(testUsdGenCaptureCache PROPERTIES
    LABELS "T1;scheduling;imaging;incremental" TIMEOUT 60)
target_link_libraries(testUsdGenIncrementalCapture PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenIncrementalCapture COMMAND testUsdGenIncrementalCapture)
set_tests_properties(testUsdGenIncrementalCapture PROPERTIES
    LABELS "T1;scheduling;imaging;incremental" TIMEOUT 60)
add_executable(testUsdGenSceneExit tests/testUsdGenSceneExit.cpp)
target_link_libraries(testUsdGenSceneExit PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenSceneExit COMMAND testUsdGenSceneExit)
set_tests_properties(testUsdGenSceneExit PROPERTIES LABELS "T1;scheduling;imaging;scene-owner" TIMEOUT 60)
target_link_libraries(testUsdGenSceneOwner PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenSceneOwner COMMAND testUsdGenSceneOwner)
set_tests_properties(testUsdGenSceneOwner PROPERTIES
    LABELS "T1;scheduling;imaging;scene-owner" TIMEOUT 60)
add_executable(testUsdGenDeviceGeneration tests/testUsdGenDeviceGeneration.cpp)
target_link_libraries(testUsdGenDeviceGeneration PRIVATE usdGen)
add_test(NAME testUsdGenDeviceGeneration COMMAND testUsdGenDeviceGeneration)
set_tests_properties(testUsdGenDeviceGeneration PROPERTIES LABELS "T0;device;sessions")
add_executable(testUsdGenImageSampler tests/testUsdGenImageSampler.cpp)
target_link_libraries(testUsdGenImageSampler PRIVATE usdGen)
add_test(NAME testUsdGenImageSampler COMMAND testUsdGenImageSampler)
set_tests_properties(testUsdGenImageSampler PROPERTIES LABELS "T0;image;sampler")
if(USDGEN_ENABLE_CUDA)
    add_executable(testUsdGenCudaCurveGrowFailure tests/testUsdGenCudaCurveGrowFailure.cpp)
    target_compile_definitions(testUsdGenCudaCurveGrowFailure PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaCurveGrowFailure PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaCurveGrowFailure COMMAND testUsdGenCudaCurveGrowFailure)
    add_test(NAME testUsdGenCudaCurveGrowNamedFailure COMMAND testUsdGenCudaCurveGrowFailure --named)
    add_test(NAME testUsdGenCudaCurveGrowNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native)
    add_test(NAME testUsdGenCudaCurveGrowNamedNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native)
    add_test(NAME testUsdGenCudaCurveGrowSourceTerminalFailure COMMAND testUsdGenCudaCurveGrowFailure --install --source-terminal)
    add_test(NAME testUsdGenCudaCurveGrowSourceTerminalNamedFailure COMMAND testUsdGenCudaCurveGrowFailure --named --source-terminal)
    add_test(NAME testUsdGenCudaCurveGrowSourceTerminalNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native --source-terminal)
    add_test(NAME testUsdGenCudaCurveGrowSourceTerminalNamedNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native --source-terminal)
    add_test(NAME testUsdGenCudaGrowLengthFailure COMMAND testUsdGenCudaCurveGrowFailure --install --length)
    add_test(NAME testUsdGenCudaGrowLengthCountsFailure COMMAND testUsdGenCudaCurveGrowFailure --named --length)
    add_test(NAME testUsdGenCudaGrowLengthNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native --length)
    add_test(NAME testUsdGenCudaGrowLengthCountsNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native --length)
    add_test(NAME testUsdGenCudaMixedGrowLengthFailure COMMAND testUsdGenCudaCurveGrowFailure --install --mixed)
    add_test(NAME testUsdGenCudaMixedGrowLengthCountsFailure COMMAND testUsdGenCudaCurveGrowFailure --named --mixed)
    add_test(NAME testUsdGenCudaMixedGrowLengthNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native --mixed)
    add_test(NAME testUsdGenCudaMixedGrowLengthCountsNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native --mixed)
    add_test(NAME testUsdGenCudaMultiGrowFailure COMMAND testUsdGenCudaCurveGrowFailure --install --multi-grow)
    add_test(NAME testUsdGenCudaMultiGrowNamedFailure COMMAND testUsdGenCudaCurveGrowFailure --named --multi-grow)
    add_test(NAME testUsdGenCudaMultiGrowNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native --multi-grow)
    add_test(NAME testUsdGenCudaMultiGrowNamedNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native --multi-grow)
    add_test(NAME testUsdGenCudaNoiseGrowFailure COMMAND testUsdGenCudaCurveGrowFailure --install --noise-grow)
    add_test(NAME testUsdGenCudaNoiseGrowNamedFailure COMMAND testUsdGenCudaCurveGrowFailure --named --noise-grow)
    add_test(NAME testUsdGenCudaNoiseGrowNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native --noise-grow)
    add_test(NAME testUsdGenCudaNoiseGrowNamedNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native --noise-grow)
    add_test(NAME testUsdGenCudaCulledNoiseGrowFailure COMMAND testUsdGenCudaCurveGrowFailure --install --culled-noise-grow)
    add_test(NAME testUsdGenCudaCulledNoiseGrowNamedFailure COMMAND testUsdGenCudaCurveGrowFailure --named --culled-noise-grow)
    add_test(NAME testUsdGenCudaCulledNoiseGrowNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --native --culled-noise-grow)
    add_test(NAME testUsdGenCudaCulledNoiseGrowNamedNativeFailure COMMAND testUsdGenCudaCurveGrowFailure --named-native --culled-noise-grow)
    set_tests_properties(testUsdGenCudaCurveGrowFailure testUsdGenCudaCurveGrowNamedFailure
        testUsdGenCudaCurveGrowNativeFailure testUsdGenCudaCurveGrowNamedNativeFailure
        testUsdGenCudaCurveGrowSourceTerminalFailure testUsdGenCudaCurveGrowSourceTerminalNamedFailure
        testUsdGenCudaCurveGrowSourceTerminalNativeFailure testUsdGenCudaCurveGrowSourceTerminalNamedNativeFailure
        testUsdGenCudaGrowLengthFailure testUsdGenCudaGrowLengthCountsFailure
        testUsdGenCudaGrowLengthNativeFailure testUsdGenCudaGrowLengthCountsNativeFailure
        testUsdGenCudaMixedGrowLengthFailure testUsdGenCudaMixedGrowLengthCountsFailure
        testUsdGenCudaMixedGrowLengthNativeFailure testUsdGenCudaMixedGrowLengthCountsNativeFailure
        testUsdGenCudaMultiGrowFailure testUsdGenCudaMultiGrowNamedFailure
        testUsdGenCudaMultiGrowNativeFailure testUsdGenCudaMultiGrowNamedNativeFailure
        testUsdGenCudaNoiseGrowFailure testUsdGenCudaNoiseGrowNamedFailure
        testUsdGenCudaNoiseGrowNativeFailure testUsdGenCudaNoiseGrowNamedNativeFailure
        testUsdGenCudaCulledNoiseGrowFailure testUsdGenCudaCulledNoiseGrowNamedFailure
        testUsdGenCudaCulledNoiseGrowNativeFailure testUsdGenCudaCulledNoiseGrowNamedNativeFailure PROPERTIES
        LABELS "T0;cuda;topology;curve-grow;cow;quarantine" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaCurveGrowSession tests/testUsdGenCudaCurveGrowSession.cpp)
    target_compile_definitions(testUsdGenCudaCurveGrowSession PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaCurveGrowSession PRIVATE usdGen usdGenGpu usdGenMath usdGen_seexpr)
    add_test(NAME testUsdGenCudaCurveGrowSession COMMAND testUsdGenCudaCurveGrowSession)
    set_tests_properties(testUsdGenCudaCurveGrowSession PROPERTIES
        LABELS "T0;cuda;topology;curve-grow;cow;session" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaCurveGrowGeneration tests/testUsdGenCudaCurveGrowGeneration.cpp)
    target_link_libraries(testUsdGenCudaCurveGrowGeneration PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaCurveGrowGeneration COMMAND testUsdGenCudaCurveGrowGeneration)
    set_tests_properties(testUsdGenCudaCurveGrowGeneration PROPERTIES
        LABELS "T0;cuda;topology;curve-grow;cow;generation" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaCurveGrow tests/testUsdGenCudaCurveGrow.cu)
    target_link_libraries(testUsdGenCudaCurveGrow PRIVATE usdGenGpu usdGen)
    target_include_directories(testUsdGenCudaCurveGrow PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenMath")
    add_test(NAME testUsdGenCudaCurveGrow COMMAND testUsdGenCudaCurveGrow)
    add_test(NAME testUsdGenCudaCurveGrowQuarantine COMMAND testUsdGenCudaCurveGrow --quarantine)
    set_tests_properties(testUsdGenCudaCurveGrowQuarantine PROPERTIES
        LABELS "T0;cuda;topology;curve-grow;quarantine;cow" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    set_tests_properties(testUsdGenCudaCurveGrow PROPERTIES
        LABELS "T0;cuda;topology;curve-grow;cow" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaScatterGrow tests/testUsdGenCudaScatterGrow.cu)
    target_link_libraries(testUsdGenCudaScatterGrow PRIVATE usdGenGpu usdGen)
    target_include_directories(testUsdGenCudaScatterGrow PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenMath")
    add_test(NAME testUsdGenCudaScatterGrow COMMAND testUsdGenCudaScatterGrow)
    set_tests_properties(testUsdGenCudaScatterGrow PROPERTIES
        LABELS "T0;cuda;topology;scatter-grow" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaScatterGrowPreflight tests/testUsdGenCudaScatterGrowPreflight.cu)
    target_link_libraries(testUsdGenCudaScatterGrowPreflight PRIVATE usdGenGpu usdGen)
    target_include_directories(testUsdGenCudaScatterGrowPreflight PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenMath")
    add_test(NAME testUsdGenCudaScatterGrowPreflight COMMAND testUsdGenCudaScatterGrowPreflight)
    add_test(NAME testUsdGenCudaScatterGrowHostQuarantine
        COMMAND testUsdGenCudaScatterGrowPreflight --quarantine-host)
    set_tests_properties(testUsdGenCudaScatterGrowHostQuarantine PROPERTIES
        LABELS "T0;cuda;topology;scatter-grow;quarantine" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    set_tests_properties(testUsdGenCudaScatterGrowPreflight PROPERTIES
        LABELS "T0;cuda;topology;scatter-grow" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaScatterInput tests/testUsdGenCudaScatterInput.cpp)
    add_executable(testUsdGenCudaScatterGrowSession tests/testUsdGenCudaScatterGrowSession.cpp)
    add_executable(testUsdGenCudaScatterGrowWidthDag tests/testUsdGenCudaScatterGrowWidthDag.cpp)
    add_executable(testUsdGenCudaScatterGrowValueDag tests/testUsdGenCudaScatterGrowValueDag.cpp)
    add_executable(testUsdGenCudaCpuSourceParity tests/testUsdGenCpuCurveSource.cpp)
    add_executable(testUsdGenCudaSurfaceBindingCache tests/testUsdGenSurfaceBindingCache.cpp)
    target_compile_definitions(testUsdGenCudaSurfaceBindingCache PRIVATE USDGEN_TEST_CUDA_BINDING_CACHE)
    target_link_libraries(testUsdGenCudaSurfaceBindingCache PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaSurfaceBindingCache COMMAND testUsdGenCudaSurfaceBindingCache)
    set_tests_properties(testUsdGenCudaSurfaceBindingCache PROPERTIES
        LABELS "T1;cuda;sources;cache;cow" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaSurfaceRootFrameParity tests/testUsdGenSurfaceRootFrames.cpp)
    target_compile_definitions(testUsdGenCudaSurfaceRootFrameParity PRIVATE USDGEN_TEST_CUDA_ROOT_FRAME_PARITY)
    target_link_libraries(testUsdGenCudaSurfaceRootFrameParity PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaSurfaceRootFrameParity COMMAND testUsdGenCudaSurfaceRootFrameParity)
    set_tests_properties(testUsdGenCudaSurfaceRootFrameParity PROPERTIES
        LABELS "T0;cuda;sources;root-frames" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    target_compile_definitions(testUsdGenCudaCpuSourceParity PRIVATE USDGEN_TEST_CUDA_SOURCE_PARITY)
    target_link_libraries(testUsdGenCudaCpuSourceParity PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaCpuSourceParity COMMAND testUsdGenCudaCpuSourceParity)
    set_tests_properties(testUsdGenCudaCpuSourceParity PROPERTIES
        LABELS "T1;cuda;sources;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    target_link_libraries(testUsdGenCudaScatterGrowWidthDag PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaScatterGrowWidthDag COMMAND testUsdGenCudaScatterGrowWidthDag)
    set_tests_properties(testUsdGenCudaScatterGrowWidthDag PROPERTIES
        LABELS "T1;cuda;scatter-grow;dag;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    target_link_libraries(testUsdGenCudaScatterGrowValueDag PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaScatterGrowValueDag COMMAND testUsdGenCudaScatterGrowValueDag)
    set_tests_properties(testUsdGenCudaScatterGrowValueDag PROPERTIES
        LABELS "T1;cuda;scatter-grow;dag;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_test(NAME testUsdGenCudaScatterGrowRbfDag COMMAND testUsdGenCudaScatterGrowValueDag --rbf)
    set_tests_properties(testUsdGenCudaScatterGrowRbfDag PROPERTIES
        LABELS "T1;cuda;scatter-grow;dag;cow;rbf;transaction" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    target_link_libraries(testUsdGenCudaScatterGrowSession PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaScatterGrowSession COMMAND testUsdGenCudaScatterGrowSession)
    add_test(NAME testUsdGenCudaScatterGrowSourceInstallFailure
        COMMAND testUsdGenCudaScatterGrowSession --source-install-failure)
    add_test(NAME testUsdGenCudaScatterGrowSourceNativeFailure
        COMMAND testUsdGenCudaScatterGrowSession --source-native-failure)
    set_tests_properties(testUsdGenCudaScatterGrowSourceInstallFailure
        testUsdGenCudaScatterGrowSourceNativeFailure PROPERTIES
        LABELS "T1;cuda;scatter-grow;quarantine" TIMEOUT 30 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    set_tests_properties(testUsdGenCudaScatterGrowSession PROPERTIES
        LABELS "T1;cuda;scatter-grow;session;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    target_link_libraries(testUsdGenCudaScatterInput PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaScatterInput COMMAND testUsdGenCudaScatterInput)
    set_tests_properties(testUsdGenCudaScatterInput PROPERTIES
        LABELS "T0;cuda;topology;scatter-input" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaImageSampler tests/testUsdGenCudaImageSampler.cu)
    target_link_libraries(testUsdGenCudaImageSampler PRIVATE usdGenGpu usdGen)
    add_test(NAME testUsdGenCudaImageSampler COMMAND testUsdGenCudaImageSampler)
    set_tests_properties(testUsdGenCudaImageSampler PROPERTIES
        LABELS "T0;cuda;image;sampler" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaExecutionResources tests/testUsdGenCudaExecutionResources.cpp)
    target_link_libraries(testUsdGenCudaExecutionResources PRIVATE usdGenGpu)
    add_test(NAME testUsdGenCudaExecutionResources COMMAND testUsdGenCudaExecutionResources)
    set_tests_properties(testUsdGenCudaExecutionResources PROPERTIES
        LABELS "T0;cuda;resources" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaRetirement tests/testUsdGenCudaRetirement.cpp)
    target_link_libraries(testUsdGenCudaRetirement PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaRetirement COMMAND testUsdGenCudaRetirement)
    set_tests_properties(testUsdGenCudaRetirement PROPERTIES
        LABELS "T0;cuda;resources;retirement" TIMEOUT 30 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_test(NAME testUsdGenCudaRetirementExit COMMAND testUsdGenCudaRetirement --exit-retire)
    set_tests_properties(testUsdGenCudaRetirementExit PROPERTIES
        LABELS "T1;cuda;resources;retirement;process-exit" TIMEOUT 30 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    # The optional oracle uses the actual native Storm index builder. Its
    # private header is not installed with the SDK; never add this source-tree
    # dependency to production targets or installed interfaces.
    set(USDGEN_OPENUSD_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../OpenUSD" CACHE PATH
        "Matching OpenUSD source tree for native Storm index oracle tests")
    add_executable(testUsdGenCudaCurveIndices tests/testUsdGenCudaCurveIndices.cpp)
    target_link_libraries(testUsdGenCudaCurveIndices PRIVATE usdGenGpu hdSt)
    if(EXISTS "${USDGEN_OPENUSD_SOURCE_DIR}/pxr/imaging/hdSt/basisCurvesTopology.h")
        target_include_directories(testUsdGenCudaCurveIndices SYSTEM PRIVATE "${USDGEN_OPENUSD_SOURCE_DIR}")
        target_compile_definitions(testUsdGenCudaCurveIndices PRIVATE USDGEN_HAS_STORM_INDEX_ORACLE)
    endif()
    add_test(NAME testUsdGenCudaCurveIndices COMMAND testUsdGenCudaCurveIndices)
    add_test(NAME testUsdGenCudaCurveIndicesStorm COMMAND testUsdGenCudaCurveIndices --native)
    set_tests_properties(testUsdGenCudaCurveIndices testUsdGenCudaCurveIndicesStorm PROPERTIES
        LABELS "T1;cuda;topology;interop" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    set_tests_properties(testUsdGenCudaCurveIndicesStorm PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(testUsdGenCudaCurveTiles tests/testUsdGenCudaCurveTiles.cpp)
    target_link_libraries(testUsdGenCudaCurveTiles PRIVATE usdGenGpu)
    add_test(NAME testUsdGenCudaCurveTiles COMMAND testUsdGenCudaCurveTiles)
    set_tests_properties(testUsdGenCudaCurveTiles PROPERTIES
        LABELS "T1;cuda;topology;tiles" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaCurveResample tests/testUsdGenCudaCurveResample.cpp)
    target_link_libraries(testUsdGenCudaCurveResample PRIVATE usdGenGpu)
    add_test(NAME testUsdGenCudaCurveResample COMMAND testUsdGenCudaCurveResample)
    set_tests_properties(testUsdGenCudaCurveResample PROPERTIES
        LABELS "T1;cuda;topology;resample" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaCurveTileBounds tests/testUsdGenCudaCurveTileBounds.cpp)
    target_link_libraries(testUsdGenCudaCurveTileBounds PRIVATE usdGenGpu)
    add_test(NAME testUsdGenCudaCurveTileBounds COMMAND testUsdGenCudaCurveTileBounds)
    set_tests_properties(testUsdGenCudaCurveTileBounds PROPERTIES
        LABELS "T1;cuda;topology;tiles" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaTileLease tests/testUsdGenCudaTileLease.cpp)
    target_compile_definitions(testUsdGenCudaTileLease PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaTileLease PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaTileLease COMMAND testUsdGenCudaTileLease)
    set_tests_properties(testUsdGenCudaTileLease PROPERTIES
        LABELS "T1;cuda;tilelease" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaTileExecution tests/testUsdGenCudaTileExecution.cpp)
    target_compile_definitions(testUsdGenCudaTileExecution PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaTileExecution PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaTileExecution COMMAND testUsdGenCudaTileExecution)
    set_tests_properties(testUsdGenCudaTileExecution PROPERTIES
        LABELS "T1;cuda;tiles;sessions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaDevicePresentation tests/testUsdGenCudaDevicePresentation.cpp)
    target_compile_definitions(testUsdGenCudaDevicePresentation PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaDevicePresentation PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaDevicePresentation COMMAND testUsdGenCudaDevicePresentation)
    set_tests_properties(testUsdGenCudaDevicePresentation PROPERTIES
        LABELS "T1;cuda;sessions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    if(NOT WIN32)
        add_executable(testUsdGenCudaGlInterop tests/testUsdGenCudaGlInterop.cpp)
        target_compile_definitions(testUsdGenCudaGlInterop PRIVATE USDGEN_ENABLE_CUDA)
        target_include_directories(testUsdGenCudaGlInterop PRIVATE ${PROJECT_SOURCE_DIR}/usdGenShaders/test)
        target_link_libraries(testUsdGenCudaGlInterop PRIVATE usdGenImaging usdGenGpu hdSt hgiGL garch ${CMAKE_DL_LIBS})
    endif()
    usdgen_add_unix_only_test(testUsdGenCudaGlInterop testUsdGenCudaGlInterop)
    set_tests_properties(testUsdGenCudaGlInterop PROPERTIES LABELS "T2;cuda;interop" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_executable(testUsdGenCudaExecutionQueue tests/testUsdGenCudaExecutionQueue.cpp)
    target_link_libraries(testUsdGenCudaExecutionQueue PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaExecutionQueue COMMAND testUsdGenCudaExecutionQueue)
    set_tests_properties(testUsdGenCudaExecutionQueue PROPERTIES LABELS "T1;cuda;scheduling;rbf" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaExecutionPlan tests/testUsdGenCudaExecutionPlan.cpp)
    target_link_libraries(testUsdGenCudaExecutionPlan PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaExecutionPlan COMMAND testUsdGenCudaExecutionPlan)
    set_tests_properties(testUsdGenCudaExecutionPlan PROPERTIES
        LABELS "T0;cuda;scheduling;metadata" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaWidthDag tests/testUsdGenCudaWidthDag.cpp)
    target_compile_definitions(testUsdGenCudaWidthDag PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaWidthDag PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaWidthDag COMMAND testUsdGenCudaWidthDag)
    set_tests_properties(testUsdGenCudaWidthDag PROPERTIES
        LABELS "T1;cuda;scheduling;dag" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaNamedChannels tests/testUsdGenCudaNamedChannels.cpp)
    target_compile_definitions(testUsdGenCudaNamedChannels PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaNamedChannels PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaNamedChannels COMMAND testUsdGenCudaNamedChannels)
    set_tests_properties(testUsdGenCudaNamedChannels PROPERTIES
        LABELS "T1;cuda;resources;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaNamedChannelTopology tests/testUsdGenCudaNamedChannelTopology.cu)
    target_link_libraries(testUsdGenCudaNamedChannelTopology PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaNamedChannelTopology COMMAND testUsdGenCudaNamedChannelTopology)
    set_tests_properties(testUsdGenCudaNamedChannelTopology PROPERTIES
        LABELS "T1;cuda;topology;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenCudaRbfTransaction tests/testUsdGenCudaRbfTransaction.cpp)
    target_compile_definitions(testUsdGenCudaRbfTransaction PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaRbfTransaction PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaRbfTransaction COMMAND testUsdGenCudaRbfTransaction)
    add_test(NAME testUsdGenCudaRbfValueDag COMMAND testUsdGenCudaRbfTransaction --value-dag)
    set_tests_properties(testUsdGenCudaRbfValueDag PROPERTIES
        LABELS "T1;cuda;rbf;transaction;dag;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    set_tests_properties(testUsdGenCudaRbfTransaction PROPERTIES
        LABELS "T1;cuda;rbf;transaction" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaExecutionStages tests/testUsdGenCudaExecutionStages.cpp)
    target_compile_definitions(testUsdGenCudaExecutionStages PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaExecutionStages PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaExecutionStages COMMAND testUsdGenCudaExecutionStages)
    set_tests_properties(testUsdGenCudaExecutionStages PROPERTIES LABELS "T1;cuda;scheduling;rbf" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaTools tests/testUsdGenCudaTools.cpp)
    target_link_libraries(testUsdGenCudaTools PRIVATE usdGenImaging usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaTools COMMAND testUsdGenCudaTools)
    set_tests_properties(testUsdGenCudaTools PROPERTIES LABELS "T1;cuda;tools;sessions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaParameters tests/testUsdGenCudaParameters.cpp)
    target_compile_definitions(testUsdGenCudaParameters PRIVATE USDGEN_ENABLE_CUDA)
    set_target_properties(testUsdGenCudaParameters PROPERTIES USDGEN_KEEP_ASSERTS ON)
    target_link_libraries(testUsdGenCudaParameters PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaParameters COMMAND testUsdGenCudaParameters)
    set_tests_properties(testUsdGenCudaParameters PROPERTIES LABELS "T1;cuda;expressions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaSurfaceInput tests/testUsdGenCudaSurfaceInput.cpp)
    target_compile_definitions(testUsdGenCudaSurfaceInput PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaSurfaceInput PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaSurfaceInput COMMAND testUsdGenCudaSurfaceInput)
    set_tests_properties(testUsdGenCudaSurfaceInput PROPERTIES LABELS "T0;cuda;sources")
    add_executable(testUsdGenCudaRbfSession tests/testUsdGenCudaRbfSession.cpp)
    target_link_libraries(testUsdGenCudaRbfSession PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaRbfSession COMMAND testUsdGenCudaRbfSession)
    set_tests_properties(testUsdGenCudaRbfSession PROPERTIES LABELS "T1;cuda;rbf;sessions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaLengthSession tests/testUsdGenCudaLengthSession.cpp)
    target_link_libraries(testUsdGenCudaLengthSession PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaLengthSession COMMAND testUsdGenCudaLengthSession)
    set_tests_properties(testUsdGenCudaLengthSession PROPERTIES LABELS "T1;cuda;operators;sessions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaSession tests/testUsdGenCudaSession.cpp)
    target_link_libraries(testUsdGenCudaSession PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaSession COMMAND testUsdGenCudaSession)
    set_tests_properties(testUsdGenCudaSession PROPERTIES LABELS "T1;cuda;sessions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaSessionTaskGraph tests/testUsdGenCudaSessionTaskGraph.cpp)
    target_link_libraries(testUsdGenCudaSessionTaskGraph PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaSessionTaskGraph COMMAND testUsdGenCudaSessionTaskGraph)
    set_tests_properties(testUsdGenCudaSessionTaskGraph PROPERTIES LABELS "T1;cuda;sessions;scheduling" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaSourceInput tests/testUsdGenCudaSourceInput.cpp)
    target_link_libraries(testUsdGenCudaSourceInput PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaSourceInput COMMAND testUsdGenCudaSourceInput)
    set_tests_properties(testUsdGenCudaSourceInput PROPERTIES LABELS "T0;cuda;sources")
    foreach(_cuda_test CurveSource ExpressionContext Width SurfaceBinding Length CurveCompaction Picking PointOverride Topology)
        add_executable(testUsdGenCuda${_cuda_test} tests/testUsdGenCuda${_cuda_test}.cu)
        target_link_libraries(testUsdGenCuda${_cuda_test} PRIVATE usdGenGpu usdGen)
        set_target_properties(testUsdGenCuda${_cuda_test} PROPERTIES USDGEN_KEEP_ASSERTS ON)
        add_test(NAME testUsdGenCuda${_cuda_test} COMMAND testUsdGenCuda${_cuda_test})
        set_tests_properties(testUsdGenCuda${_cuda_test} PROPERTIES
            LABELS "T1;cuda" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    endforeach()
    add_executable(testUsdGenCudaNonWidthCompare tests/testUsdGenCudaNonWidthCompare.cu)
    target_link_libraries(testUsdGenCudaNonWidthCompare PRIVATE usdGenGpu usdGen)
    set_target_properties(testUsdGenCudaNonWidthCompare PROPERTIES USDGEN_KEEP_ASSERTS ON)
    add_test(NAME testUsdGenCudaNonWidthCompare COMMAND testUsdGenCudaNonWidthCompare)
    set_tests_properties(testUsdGenCudaNonWidthCompare PROPERTIES
        LABELS "T1;cuda;dag;cow;topology" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    foreach(_cuda_test Rbf Deform)
        add_executable(testUsdGenCuda${_cuda_test} tests/testUsdGenCuda${_cuda_test}.cu)
        target_link_libraries(testUsdGenCuda${_cuda_test} PRIVATE usdGenGpu)
        # These CUDA oracles use assert for both execution and checks.
        # Release's -DNDEBUG must never turn them into false-green no-ops.
        set_target_properties(testUsdGenCuda${_cuda_test} PROPERTIES USDGEN_KEEP_ASSERTS ON)
        add_test(NAME testUsdGenCuda${_cuda_test} COMMAND testUsdGenCuda${_cuda_test})
        set_tests_properties(testUsdGenCuda${_cuda_test} PROPERTIES
            LABELS "T1;cuda;rbf" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    endforeach()
    add_executable(testUsdGenCudaStyle tests/testUsdGenCudaStyle.cu)
    target_link_libraries(testUsdGenCudaStyle PRIVATE usdGenGpu)
    add_test(NAME testUsdGenCudaStyle COMMAND testUsdGenCudaStyle)
    set_tests_properties(testUsdGenCudaStyle PROPERTIES
        LABELS "T1;cuda;operators" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaNoise tests/testUsdGenCudaNoise.cpp)
    # The oracle calls the explicitly-instantiated SeExpr 3->3 double FBM;
    # CUDA itself is deliberately float and built with global --fmad=false.
    target_link_libraries(testUsdGenCudaNoise PRIVATE usdGenGpu usdGenMath usdGen_seexpr)
    add_test(NAME testUsdGenCudaNoise COMMAND testUsdGenCudaNoise)
    set_tests_properties(testUsdGenCudaNoise PROPERTIES
        LABELS "T1;cuda;operators;noise" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaNoiseSession tests/testUsdGenCudaNoiseSession.cpp)
    target_link_libraries(testUsdGenCudaNoiseSession PRIVATE usdGenGpu usdGen)
    add_test(NAME testUsdGenCudaNoiseSession COMMAND testUsdGenCudaNoiseSession)
    set_tests_properties(testUsdGenCudaNoiseSession PROPERTIES
        LABELS "T1;cuda;operators;noise;session;cow" TIMEOUT 120
        RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaRootFrames tests/testUsdGenCudaRootFrames.cpp)
    target_link_libraries(testUsdGenCudaRootFrames PRIVATE usdGenGpu gf)
    add_test(NAME testUsdGenCudaRootFrames COMMAND testUsdGenCudaRootFrames)
    set_tests_properties(testUsdGenCudaRootFrames PROPERTIES
        LABELS "T1;cuda;root-frames" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaRootFrameGather tests/testUsdGenCudaRootFrameGather.cpp)
    target_link_libraries(testUsdGenCudaRootFrameGather PRIVATE usdGenGpu)
    add_test(NAME testUsdGenCudaRootFrameGather COMMAND testUsdGenCudaRootFrameGather)
    set_tests_properties(testUsdGenCudaRootFrameGather PROPERTIES
        LABELS "T1;cuda;root-frames" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaExpression tests/testUsdGenCudaExpression.cpp
        tests/usdGenSeExprOracle.cpp)
    set_source_files_properties(tests/usdGenSeExprOracle.cpp PROPERTIES
        COMPILE_DEFINITIONS "SeExpr2=UsdGenSeExprFrontend;SeExprInternal2=UsdGenSeExprFrontendInternal"
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/seexprFrontend/SeExpr2")
    target_link_libraries(testUsdGenCudaExpression PRIVATE usdGenGpu usdGen usdGenExprFrontend)
    add_test(NAME testUsdGenCudaExpression COMMAND testUsdGenCudaExpression)
    set_tests_properties(testUsdGenCudaExpression PROPERTIES
        LABELS "T1;cuda;expressions" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    # Bit-for-bit CPU/CUDA parity over the shared IR interpreter.
    add_executable(testUsdGenCudaExpressionParity tests/testUsdGenCudaExpressionParity.cpp)
    target_link_libraries(testUsdGenCudaExpressionParity PRIVATE usdGenGpu usdGen usdGenExprFrontend)
    add_test(NAME testUsdGenCudaExpressionParity COMMAND testUsdGenCudaExpressionParity)
    set_tests_properties(testUsdGenCudaExpressionParity PROPERTIES
        LABELS "T1;cuda;expressions;parity" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# M0: build-rule tests (gate B-1) — see cmake/CheckNoStage*.cmake
set(USDGEN_FORBIDDEN_LIB_REGEX "^libusd_(usd|hd|hgi|glf|garch)")
set(USDGEN_FORBIDDEN_INC_REGEX
    "pxr/(usd|imaging|usdImaging)/(usd|hd|hgi|glf|garch)[A-Za-z0-9_]*\\/")

find_program(USDGEN_CXXFILT c++filt)

foreach(_t usdGen usdGenMath usdGenTestUtils)
    # Gate B-1 link rule inspects ELF magic and runs objdump/nm; Unix-only.
    usdgen_add_unix_only_test(testUsdGenLinkRule_${_t}
        ${CMAKE_COMMAND}
            -DTARGET_FILE=$<TARGET_FILE:${_t}>
            -DOBJDUMP=${CMAKE_OBJDUMP}
            -DCXXFILT=${USDGEN_CXXFILT}
            -DFORBIDDEN=${USDGEN_FORBIDDEN_LIB_REGEX}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageLink.cmake)
endforeach()
# All three link-rule tests are the B-1 gate (core libs must not link the
# OpenUSD stage family). Label every foreach-generated test so the canonical
# `ctest -L '^T[01]$'` selector covers all of them (sol C-3, gatekeeper close).
foreach(_t usdGen usdGenMath usdGenTestUtils)
    set_tests_properties(testUsdGenLinkRule_${_t}
        PROPERTIES LABELS "T0;build;gate:B-1")
endforeach()

add_test(NAME testUsdGenIncludeRule
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DFORBIDDEN=${USDGEN_FORBIDDEN_INC_REGEX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageInclude.cmake)
set_tests_properties(testUsdGenIncludeRule
    PROPERTIES LABELS "T0;build;gate:B-1")

# ELF symbol-table inspection (nm); Unix-only.
usdgen_add_unix_only_test(testUsdGenNoThirdPartyExports
    ${CMAKE_COMMAND}
        -DTARGET_FILE=$<TARGET_FILE:usdGen>
        -DCXXFILT=${USDGEN_CXXFILT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoThirdPartyExports.cmake)
# sol X-1: this is a B-1 gate check and must carry the gate:B-1 label so
# `ctest -L '^gate:B-1$'` selects all five B-1 tests.
set_tests_properties(testUsdGenNoThirdPartyExports
    PROPERTIES LABELS "T0;build;gate:B-1")

# sol S-1/S-2: negative/positive fixtures proving the B-1 gates actually fail
# on forbidden input and pass on clean input (no silent false passes).
usdgen_add_unix_only_test(testUsdGenLinkRuleNegativeFixture
    ${CMAKE_COMMAND}
        -DTARGET_FILE=${USD_INSTALL_DIR}/lib/libusd_hd.so
        -DOBJDUMP=${CMAKE_OBJDUMP}
        -DCXXFILT=${USDGEN_CXXFILT}
        -DFORBIDDEN=^libusd_tf
        -DEXPECT=fail
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateLinkDriver.cmake)
usdgen_add_unix_only_test(testUsdGenLinkRulePositiveFixture
    ${CMAKE_COMMAND}
        -DTARGET_FILE=$<TARGET_FILE:usdGen>
        -DOBJDUMP=${CMAKE_OBJDUMP}
        -DCXXFILT=${USDGEN_CXXFILT}
        -DFORBIDDEN=${USDGEN_FORBIDDEN_LIB_REGEX}
        -DEXPECT=pass
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateLinkDriver.cmake)
add_test(NAME testUsdGenIncludeRuleNegativeFixture
    COMMAND ${CMAKE_COMMAND}
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateIncludeForbidden
        -DFORBIDDEN=${USDGEN_FORBIDDEN_INC_REGEX}
        -DEXPECT=fail
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateIncludeDriver.cmake)
add_test(NAME testUsdGenIncludeRulePositiveFixture
    COMMAND ${CMAKE_COMMAND}
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateIncludeClean
        -DFORBIDDEN=${USDGEN_FORBIDDEN_INC_REGEX}
        -DEXPECT=pass
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateIncludeDriver.cmake)
# Note: the fixture tests verify the gate *mechanism* but are not themselves
# gate checks, so they carry only T0;build. This keeps
# `ctest -L '^gate:B-1$'` selecting exactly the five B-1 gate checks
# (sol X-1; plan 10-build-dependencies-testing.md §1.4).
foreach(_f testUsdGenLinkRuleNegativeFixture testUsdGenLinkRulePositiveFixture
           testUsdGenIncludeRuleNegativeFixture testUsdGenIncludeRulePositiveFixture)
    set_tests_properties(${_f}
        PROPERTIES LABELS "T0;build")
endforeach()

# M2: staging-rule tests (gate B-2) — the Hydra-sourced staging fence
# (13-codebase-alignment.md §7 V2-8). Mirrors the B-1 gate pattern: an
# include-regex half over the fenced staging TUs plus an nm
# undefined-symbol half over their compiled objects, with negative/positive
# fixtures proving the mechanism bites both ways. Fenced TUs are the
# Hydra-builder and _CommitNow staging path (09 §5.1 B-2); the renamed
# stage builder (BuildGraphDescFromStage, V2-11) lives outside this list
# by design as the fenced-off T0/offline path.
set(USDGEN_FORBIDDEN_STAGING_INC_REGEX
    "pxr/usd/(usd/(stage|prim|attribute|relationship|primRange)\\.h|usdGeom/|usdShade/)")
set(USDGEN_FORBIDDEN_STAGING_SYM_REGEX
    "Usd(Stage|Prim|Attribute|Relationship|Geom|Shade)")
find_program(USDGEN_NM nm)
set(_usdgen_staging_tus
    "libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilder.h|libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilder.cpp|libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.h|libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.cpp")
set(_usdgen_staging_objs
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/usdGenImaging.dir/libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilder.cpp.o|${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/usdGenImaging.dir/libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.cpp.o")
usdgen_add_unix_only_test(testUsdGenStagingRule
    ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DSTAGING_TUS=${_usdgen_staging_tus}
        -DOBJECTS=${_usdgen_staging_objs}
        -DFORBIDDEN_INC=${USDGEN_FORBIDDEN_STAGING_INC_REGEX}
        -DFORBIDDEN_SYM=${USDGEN_FORBIDDEN_STAGING_SYM_REGEX}
        -DNM=${USDGEN_NM}
        -DEXPECT=pass
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageStaging.cmake)
set_tests_properties(testUsdGenStagingRule
    PROPERTIES LABELS "T0;build;gate:B-2")
add_test(NAME testUsdGenStagingRuleNegativeFixture
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateStagingForbidden
        -DSTAGING_TUS=staging_bad.cpp
        -DOBJECTS=
        -DFORBIDDEN_INC=${USDGEN_FORBIDDEN_STAGING_INC_REGEX}
        -DFORBIDDEN_SYM=${USDGEN_FORBIDDEN_STAGING_SYM_REGEX}
        -DNM=${USDGEN_NM}
        -DEXPECT=fail
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageStaging.cmake)
add_test(NAME testUsdGenStagingRulePositiveFixture
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateStagingClean
        -DSTAGING_TUS=staging_clean.cpp
        -DOBJECTS=
        -DFORBIDDEN_INC=${USDGEN_FORBIDDEN_STAGING_INC_REGEX}
        -DFORBIDDEN_SYM=${USDGEN_FORBIDDEN_STAGING_SYM_REGEX}
        -DNM=${USDGEN_NM}
        -DEXPECT=pass
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageStaging.cmake)
# Fixture tests verify the gate *mechanism*, not the tree: T0;build only,
# so `ctest -L '^gate:B-2$'` selects exactly testUsdGenStagingRule.
foreach(_f testUsdGenStagingRuleNegativeFixture testUsdGenStagingRulePositiveFixture)
    set_tests_properties(${_f}
        PROPERTIES LABELS "T0;build")
endforeach()

# M0: T1 tests
# The plugin resource dirs (build tree) must be on the plugin search path so the
# codeless schema types and the plugInfo entries are discoverable.
set(USDGEN_TEST_PLUGINPATH
    "${_usdgen_schema_resources};${_usdgen_imaging_pluginfo_dir}")
set(USDGEN_TEST_PLUGINPATH "${_usdgen_schema_resources};${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenImaging/resources")

# sol S-3/X-2 fixture: a synthetic renderer-level HdSceneIndexPlugin carrying
# the plugInfo tag "hdPrman:motionBlur" so the chain test can prove usdGen's
# plugInfo ordering contract ("before: [hdPrman:motionBlur]") without
# RenderMan installed. Loaded purely through PXR_PLUGINPATH_NAME.
add_library(fakeHdPrmanPlugin SHARED tests/fixtures/fakeHdPrmanPlugin.cpp)
set_target_properties(fakeHdPrmanPlugin PROPERTIES
    CXX_VISIBILITY_PRESET default)
target_link_libraries(fakeHdPrmanPlugin PRIVATE hd hdsi tf arch pxOsd)
set(_fake_prman_resources "${CMAKE_CURRENT_BINARY_DIR}/usd/fakeHdPrman/resources")
set(_fake_prman_pluginfo "${_fake_prman_resources}/plugInfo.json")
file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fakeHdPrman/resources/plugInfo.json.in"
    _fake_prman_pluginfo_content)
file(GENERATE OUTPUT "${_fake_prman_pluginfo}"
    CONTENT "${_fake_prman_pluginfo_content}")

add_executable(testUsdGenChainOrder tests/testUsdGenChainOrder.cpp)
# Deliberately does NOT link usdGenImaging: the usdGen (and fixture) plugins
# are discovered via PXR_PLUGINPATH_NAME and dlopen'd by the registry during
# the test, so a TfErrorMark inside main catches any coding error emitted
# while the plugin libraries load (sol S-4/X-8 regression guard).
target_link_libraries(testUsdGenChainOrder PRIVATE usdImaging hdsi hd usd tf arch)
add_test(NAME testUsdGenChainOrder COMMAND testUsdGenChainOrder)
# N-4: this is the plugInfo load-order check, i.e. gate SI-5 (09 §5.2);
# the gate label belongs on the registration.
usdgen_test_pluginpath_ops(_usdgen_chain_order_env
    "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
    "${_usdgen_tonic_resources}" "${_fake_prman_resources}"
    "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
# The loader path is applied to every test by the sweep at the end of this
# block, so only the plugin search path is per-test here.
set_tests_properties(testUsdGenChainOrder PROPERTIES
    LABELS "T1;gate:SI-5"
    ENVIRONMENT_MODIFICATION "${_usdgen_chain_order_env}")

add_executable(testUsdGenPluginDiscovery tests/testUsdGenPluginDiscovery.cpp)
target_link_libraries(testUsdGenPluginDiscovery PRIVATE usdGenImaging usdGenTestUtilsHd)
add_test(NAME testUsdGenPluginDiscovery
    COMMAND testUsdGenPluginDiscovery
        ${_usdgen_schema_resources}
        ${_usdgen_imaging_resources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes/scene_empty_groom.usda)
usdgen_test_pluginpath_ops(_usdgen_plugin_discovery_env
    "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
    "${USD_INSTALL_DIR}/plugin/usd")
set_tests_properties(testUsdGenPluginDiscovery PROPERTIES
    LABELS "T1"
    ENVIRONMENT_MODIFICATION "${_usdgen_plugin_discovery_env}")

# ---------------------------------------------------------------------------
# M1 tests (plan 11 §2.2; names/labels from 10-build-dependencies-testing.md
# §5.6 + §6.5; re-baselined by plan/13-codebase-alignment.md §1: all sources
# are implemented — no "M1-STUB" string remains under tests/ or libs/. The
# SKIP_RETURN_CODE 77 / SKIP_REGULAR_EXPRESSION "M1-STUB" properties below are
# retained only as the honest-skip convention (EGL-gated Storm tests exit 77
# without a GPU); they must never read as a failure. CTest names, tiers and
# gate labels are frozen here (docs/m1/interfaces.md §8).
# ---------------------------------------------------------------------------
# M1 plugin search path: M0 schema + imaging resources plus the usdGenShaders
# resources (Sdr discovery plugin lands in usdGenImaging; 10 §4.4).
# M-13: the dirs must not be joined into one ENVIRONMENT value. Doing so put
# CMake's ';' list separator inside the value ("PXR_PLUGINPATH_NAME=a;b;c=d"),
# which cut the plugin path after the first dir and mis-split the USD dirs.
# Joining with an explicit ':' only moved the bug to Windows, where the native
# separator *is* ';'. One ENVIRONMENT_MODIFICATION op per dir is separator-free
# and therefore correct on every host.
set(_usdgen_m1_pluginpath_dirs
    "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
    "${_usdgen_shader_resources}"
    "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
usdgen_test_pluginpath_ops(_usdgen_m1_env ${_usdgen_m1_pluginpath_dirs})

# M1 T0 engine tests (no Hydra, no stage — gate B-1 keeps them clean).
foreach(_t testUsdGenGraph testUsdGenKernelDeterminism testUsdGenWidth
           testUsdGenCpuFanout testUsdGenCpuReferenceMap testUsdGenCurveRagged
           testUsdGenCpuCurveSource testUsdGenLengthRaggedLayout testUsdGenSurfaceRootFrames
           testUsdGenSurfaceRootBindings testUsdGenSurfaceRootBindingBoundaries
           testUsdGenSurfaceBindingCache testUsdGenScatterDensity testUsdGenLimitScatter)
    add_executable(${_t} tests/${_t}.cpp)
    target_link_libraries(${_t} PRIVATE usdGenTestUtils)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endforeach()
set_tests_properties(testUsdGenGraph PROPERTIES LABELS "T0;gate:E-6")
set_tests_properties(testUsdGenKernelDeterminism PROPERTIES LABELS "T0;gate:E-8")
set_tests_properties(testUsdGenWidth PROPERTIES LABELS "T0")
set_tests_properties(testUsdGenCpuFanout PROPERTIES LABELS "T0;scheduling;dag")
set_tests_properties(testUsdGenCpuReferenceMap PROPERTIES LABELS "T0;scheduling;reference;map")
set_tests_properties(testUsdGenCurveRagged PROPERTIES LABELS "T0;topology;cow;sources")
set_tests_properties(testUsdGenCpuCurveSource PROPERTIES LABELS "T0;topology;cow;sources")
set_tests_properties(testUsdGenLengthRaggedLayout PROPERTIES LABELS "T0;topology;cow;operators")
set_tests_properties(testUsdGenSurfaceRootFrames PROPERTIES LABELS "T0;sources;root-frames")
set_tests_properties(testUsdGenSurfaceRootBindings PROPERTIES LABELS "T0;sources;root-bindings;cow")
set_tests_properties(testUsdGenSurfaceRootBindingBoundaries PROPERTIES LABELS "T0;sources;root-bindings")
set_tests_properties(testUsdGenSurfaceBindingCache PROPERTIES LABELS "T0;sources;cache;cow")
set_tests_properties(testUsdGenScatterDensity PROPERTIES LABELS "T0;sources;scatter;density")

# M1 T0 perf benches (E-1/E-7 via --threads; E-2 sparse; carried in from
# prototypes/data-plane-benchmark, 11 §2.2).
foreach(_t benchUsdGenChain benchUsdGenSparse)
    add_executable(${_t} tests/perf/${_t}.cpp)
    target_link_libraries(${_t} PRIVATE usdGenTestUtils)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endforeach()
set_tests_properties(benchUsdGenChain PROPERTIES LABELS "T0;perf;gate:E-1;gate:E-7")
set_tests_properties(benchUsdGenSparse PROPERTIES LABELS "T0;perf;gate:E-2")
add_executable(benchUsdGenSurfaceBindings tests/perf/benchUsdGenSurfaceBindings.cpp)
target_link_libraries(benchUsdGenSurfaceBindings PRIVATE usdGenTestUtils)
add_test(NAME benchUsdGenSurfaceBindings COMMAND benchUsdGenSurfaceBindings)
set_tests_properties(benchUsdGenSurfaceBindings PROPERTIES
    LABELS "T0;perf;sources;root-bindings;cache"
    TIMEOUT 600 SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

# M1 T1 imaging tests (scene index over the real UsdImaging chain).
foreach(_t testUsdGenTileContract testUsdGenStockStormContract testUsdGenInvalidation testUsdGenScheduling
           testUsdGenSnapshotRace testUsdGenPopulation testUsdGenAdapter testUsdGenScenePublication
           testUsdGenRestAdapter testUsdGenCurveRestAdapter testUsdGenContracts
           testUsdGenExpressionPrimConnection testUsdGenViewportOverlay testUsdGenFurOcclusion
           testUsdGenSourceColor testUsdGenLookPrimvars
           testUsdGenInstance testUsdGenReferenceSource testUsdGenPaintLengthRecook
           testUsdGenDormantGroom)
    add_executable(${_t} tests/${_t}.cpp)
    target_link_libraries(${_t} PRIVATE usdGenTestUtilsHd)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES
        ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endforeach()
set_tests_properties(testUsdGenTileContract PROPERTIES LABELS "T1;gate:SI-1")
set_tests_properties(testUsdGenStockStormContract PROPERTIES
    LABELS "T1;stock-hydra;unpatched-openusd")
set_tests_properties(testUsdGenFurOcclusion PROPERTIES LABELS "T1;fur;shadows")
set_tests_properties(testUsdGenSourceColor PROPERTIES LABELS "T1;look;sources")
set_tests_properties(testUsdGenReferenceSource PROPERTIES LABELS "T1;sources;reference")
set_tests_properties(testUsdGenLookPrimvars PROPERTIES LABELS "T1;look;primvars")
set_tests_properties(testUsdGenPaintLengthRecook PROPERTIES
    LABELS "T1;imaging;paint;brush" TIMEOUT 120)
set_tests_properties(testUsdGenScenePublication PROPERTIES
    LABELS "T1;scheduling;imaging;scene-owner" TIMEOUT 60)
set_tests_properties(testUsdGenDormantGroom PROPERTIES
    LABELS "T1;scheduling;imaging;scene-owner" TIMEOUT 60)
set_tests_properties(testUsdGenInvalidation PROPERTIES LABELS "T1;gate:SI-2")

add_test(NAME testUsdGenUnpatchedOpenUsd
    COMMAND ${CMAKE_COMMAND}
        -DUSDGEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/tests/testUsdGenUnpatchedOpenUsd.cmake)
set_tests_properties(testUsdGenUnpatchedOpenUsd PROPERTIES
    LABELS "T0;unpatched-openusd")
add_test(NAME testUsdGenUnpatchedOpenUsdNegative
    COMMAND ${CMAKE_COMMAND}
        -DUSDGEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/tests/testUsdGenUnpatchedOpenUsdNegative.cmake)
set_tests_properties(testUsdGenUnpatchedOpenUsdNegative PROPERTIES
    LABELS "T0;unpatched-openusd")

# ---- P4 / SI-10: testUsdGenSessions registration ----
# Two indices on one session ⇒ identical generation, prim set and frame for one commit.
# Adds testUsdGenSessions as a T1 gate:SI-10 test (imaging lane).
# Placement: after the existing testUsdGenInvalidation block (~line 446).

add_executable(testUsdGenSessions
    tests/testUsdGenSessions.cpp
    )
target_link_libraries(testUsdGenSessions
    PUBLIC usdGen
    PUBLIC usdGenImaging
)
set_target_properties(testUsdGenSessions PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
)

add_test(NAME testUsdGenSessions
    COMMAND testUsdGenSessions
)
set_tests_properties(testUsdGenSessions
    PROPERTIES
    LABELS "T1;gate:SI-10"
    TIMEOUT 120
)

# ---- SI-12: testUsdGenHydraParity registration ----
# Hydra/stage builder parity on G1–G4 (imaging lane, V2-11 item 4): both
# builders must produce an identical UsdGenGraphDesc before the stage path
# is unplugged. Fixture dir arrives as USDGEN_TEST_FIXTURE_DIR (absolute
# source path, mirrors the bench --scene pattern); plugin discovery comes
# from _usdgen_m1_env like every adapter-dependent T1 test.
add_executable(testUsdGenHydraParity
    tests/testUsdGenHydraParity.cpp
)
target_link_libraries(testUsdGenHydraParity
    PUBLIC usdGenTestUtilsHd
    PUBLIC usdGenImaging
)
target_compile_definitions(testUsdGenHydraParity PRIVATE
    USDGEN_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures")
set_target_properties(testUsdGenHydraParity PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
)
add_test(NAME testUsdGenHydraParity
    COMMAND testUsdGenHydraParity
)
set_tests_properties(testUsdGenHydraParity
    PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}"
    LABELS "T1;gate:SI-12"
    TIMEOUT 300
    SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
)

# GeomSubset targets wherever usdGen needs a mesh (02 §2.20, R15): both
# builders, Scatter restriction/union/diagnostics, subset paint and
# geoSampler inputs, cooked on the CPU lane from an authored stage.
add_executable(testUsdGenGeomSubset
    tests/testUsdGenGeomSubset.cpp
)
target_link_libraries(testUsdGenGeomSubset
    PUBLIC usdGenTestUtilsHd
    PUBLIC usdGenImaging
)
set_target_properties(testUsdGenGeomSubset PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON
)
add_test(NAME testUsdGenGeomSubset
    COMMAND testUsdGenGeomSubset
)
set_tests_properties(testUsdGenGeomSubset
    PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}"
    LABELS "T1"
    TIMEOUT 300
)

add_executable(testUsdGenHierarchy tests/testUsdGenHierarchy.cpp)
target_link_libraries(testUsdGenHierarchy PRIVATE usdGenTestUtilsHd)
add_test(NAME testUsdGenHierarchy COMMAND testUsdGenHierarchy)
set_tests_properties(testUsdGenHierarchy PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;hierarchy" TIMEOUT 120)

add_executable(testUsdGenEnabledToggle tests/testUsdGenEnabledToggle.cpp)
target_link_libraries(testUsdGenEnabledToggle PRIVATE usdGenTestUtilsHd)
add_test(NAME testUsdGenEnabledToggle COMMAND testUsdGenEnabledToggle)
set_tests_properties(testUsdGenEnabledToggle PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;imaging;scheduling" TIMEOUT 120)

# The shipped root-to-tip width example, cooked on the CPU reference lane.
add_executable(testUsdGenExpressionWidthExample tests/testUsdGenExpressionWidthExample.cpp)
target_link_libraries(testUsdGenExpressionWidthExample PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenExpressionWidthExample PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenExpressionWidthExample COMMAND testUsdGenExpressionWidthExample)
set_tests_properties(testUsdGenExpressionWidthExample PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;expressions;examples" TIMEOUT 120)

# The guide-interpolation and Ptex clump examples, including dirty
# propagation from sampled geometry and map prims through the scene index.
# Reads the baked examples/maps/*.ptx.
add_executable(testUsdGenGroomExamples tests/testUsdGenGroomExamples.cpp)
target_link_libraries(testUsdGenGroomExamples PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenGroomExamples PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenGroomExamples COMMAND testUsdGenGroomExamples)
set_tests_properties(testUsdGenGroomExamples PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;expressions;examples;maps" TIMEOUT 300)

# UsdGenDeform on the CPU lane: hair deformed by animated curves (RBF).
add_executable(testUsdGenRbfDeform tests/testUsdGenRbfDeform.cpp)
target_link_libraries(testUsdGenRbfDeform PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenRbfDeform PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenRbfDeform COMMAND testUsdGenRbfDeform)
set_tests_properties(testUsdGenRbfDeform PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;examples;deform" TIMEOUT 300)

# usdGen:preview:* — the viewport value preview through the groom scene index.
add_executable(testUsdGenValuePreview tests/testUsdGenValuePreview.cpp)
target_link_libraries(testUsdGenValuePreview PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenValuePreview PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenValuePreview COMMAND testUsdGenValuePreview)
set_tests_properties(testUsdGenValuePreview PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;expressions;examples;preview" TIMEOUT 300)

add_executable(testUsdGenExpressionTransport tests/testUsdGenExpressionTransport.cpp)
target_link_libraries(testUsdGenExpressionTransport PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenExpressionTransport PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenExpressionTransport COMMAND testUsdGenExpressionTransport)
set_tests_properties(testUsdGenExpressionTransport PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;expressions" TIMEOUT 120)

add_executable(testUsdGenSessionIsolation tests/testUsdGenSessionIsolation.cpp)
target_link_libraries(testUsdGenSessionIsolation PRIVATE usdGenImaging)
add_test(NAME testUsdGenSessionIsolation COMMAND testUsdGenSessionIsolation)
set_tests_properties(testUsdGenSessionIsolation PROPERTIES
    LABELS "T1;sessions" TIMEOUT 120)

foreach(_expr_test ExpressionContext ExpressionBindings ExpressionFrontend ExpressionIr ExpressionCpuEval)
    add_executable(testUsdGen${_expr_test} tests/testUsdGen${_expr_test}.cpp)
    target_link_libraries(testUsdGen${_expr_test} PRIVATE usdGen)
    add_test(NAME testUsdGen${_expr_test} COMMAND testUsdGen${_expr_test})
    set_tests_properties(testUsdGen${_expr_test} PROPERTIES
        LABELS "T0;expressions" TIMEOUT 120)
endforeach()
# geoSampler()/ptex(): links usdGen_ptex itself to author its fixture .ptx.
add_executable(testUsdGenExpressionSamplers tests/testUsdGenExpressionSamplers.cpp)
target_link_libraries(testUsdGenExpressionSamplers PRIVATE usdGen usdGen_ptex)
add_test(NAME testUsdGenExpressionSamplers COMMAND testUsdGenExpressionSamplers)
set_tests_properties(testUsdGenExpressionSamplers PROPERTIES
    LABELS "T0;expressions;maps" TIMEOUT 120)
# Curl/Bend/Wave/Part stylers plus the primitive-domain Grow lift: registry
# contracts, expression-target validation and deterministic CPU cooks.
add_executable(testUsdGenCurlBendWavePart tests/testUsdGenCurlBendWavePart.cpp)
target_link_libraries(testUsdGenCurlBendWavePart PRIVATE usdGen)
add_test(NAME testUsdGenCurlBendWavePart COMMAND testUsdGenCurlBendWavePart)
set_tests_properties(testUsdGenCurlBendWavePart PROPERTIES
    LABELS "T0;operators;expressions" TIMEOUT 120)
# Direction/Smooth/Resample/Scale/Straighten/Displace stylers: same shape,
# one target per operator.
# SculptLayer/Wind/ExprOp/Freeze/Collide stylers: same shape, appended to
# the operator foreach above.
foreach(_styler_test Direction Smooth Resample Scale Straighten Displace SculptLayer Wind ExprOp Freeze Collide)
    add_executable(testUsdGen${_styler_test} tests/testUsdGen${_styler_test}.cpp)
    target_link_libraries(testUsdGen${_styler_test} PRIVATE usdGen)
    add_test(NAME testUsdGen${_styler_test} COMMAND testUsdGen${_styler_test})
    set_tests_properties(testUsdGen${_styler_test} PROPERTIES
        LABELS "T0;operators;expressions" TIMEOUT 120)
endforeach()
# testUsdGenScale recomputes UsdGenDraw01 expectations; usdGenMath is PRIVATE
# to usdGen (N-7), so the test links it explicitly (CudaCurveGrowSession
# precedent).
target_link_libraries(testUsdGenScale PRIVATE usdGenMath)
# Ptex face ids/coordinates and the sampler. Links usdGen_ptex itself so the
# test can author its fixture .ptx with PtexWriter.
add_executable(testUsdGenPtexMap tests/testUsdGenPtexMap.cpp)
target_link_libraries(testUsdGenPtexMap PRIVATE usdGen usdGen_ptex)
add_test(NAME testUsdGenPtexMap COMMAND testUsdGenPtexMap)
set_tests_properties(testUsdGenPtexMap PROPERTIES LABELS "T0;maps" TIMEOUT 120)
# Paintable per-surface attribute maps + stroke accumulation (the host-groomer
# brush model over per-face storage). Engine-only, no USD dependency.
add_executable(testUsdGenAttributeMap tests/testUsdGenAttributeMap.cpp)
target_link_libraries(testUsdGenAttributeMap PRIVATE usdGen)
add_test(NAME testUsdGenAttributeMap COMMAND testUsdGenAttributeMap)
set_tests_properties(testUsdGenAttributeMap PROPERTIES LABELS "T0;maps" TIMEOUT 120)
# Painted attribute maps: the .ptx bake and the async bake-to-stage worker.
# Engine-only (threads + filesystem, no USD/Hydra), like the map it bakes.
add_executable(testUsdGenAttributeBake tests/testUsdGenAttributeBake.cpp)
target_link_libraries(testUsdGenAttributeBake PRIVATE usdGen)
add_test(NAME testUsdGenAttributeBake COMMAND testUsdGenAttributeBake)
set_tests_properties(testUsdGenAttributeBake PROPERTIES LABELS "T1;maps" TIMEOUT 120)
# Authored maps into groom cooking/instancing: keep/drop + prototype index
# per strand root, CPU-only and separate from Tonic's region-map flow.
add_executable(testUsdGenAttributeInstance tests/testUsdGenAttributeInstance.cpp)
target_link_libraries(testUsdGenAttributeInstance PRIVATE usdGen)
add_test(NAME testUsdGenAttributeInstance COMMAND testUsdGenAttributeInstance)
set_tests_properties(testUsdGenAttributeInstance PROPERTIES LABELS "T0;maps" TIMEOUT 120)
# The live paint-preview overlay: app -> Hydra displayColor for the brushed
# surface and the groom, over the real UsdImaging chain.
add_executable(testUsdGenAttributePreview tests/testUsdGenAttributePreview.cpp)
target_link_libraries(testUsdGenAttributePreview PRIVATE usdGenTestUtilsHd)
add_test(NAME testUsdGenAttributePreview COMMAND testUsdGenAttributePreview)
set_tests_properties(testUsdGenAttributePreview PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;maps;preview" TIMEOUT 120)
# The attribute-brush C ABI: pick/footprint, dab kernel with hardness and
# erase, the cross-face corner smooth, commit == working, the 100k-face
# per-move cost, and the global preview overlay.
add_executable(testUsdGenBrushApi tests/testUsdGenBrushApi.cpp)
target_link_libraries(testUsdGenBrushApi PRIVATE usdGenTestUtilsHd)
add_test(NAME testUsdGenBrushApi COMMAND testUsdGenBrushApi)
set_tests_properties(testUsdGenBrushApi PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;maps;preview;brush" TIMEOUT 120)
add_executable(testUsdGenSurfaceCageInterpolate tests/testUsdGenSurfaceCageInterpolate.cpp)
target_link_libraries(testUsdGenSurfaceCageInterpolate PRIVATE usdGen usdGen_ptex)
add_test(NAME testUsdGenSurfaceCageInterpolate COMMAND testUsdGenSurfaceCageInterpolate)
set_tests_properties(testUsdGenSurfaceCageInterpolate PROPERTIES
    LABELS "T0;maps;sources" TIMEOUT 120)
# The function library against Disney's own SeExpr2. This is the only test that
# links the vendored frontend archive directly: expressions/exprMath.h claims to
# reproduce SeExpr2's builtins for the device, and the claim is only worth
# something if it is checked against the original. The include directory is
# SYSTEM so the vendored headers keep their own warning level, and the SeExpr2
# namespace rename must match the one usdGenExprFrontend was compiled with.
add_executable(testUsdGenSeExprOracle tests/testUsdGenSeExprOracle.cpp)
target_link_libraries(testUsdGenSeExprOracle PRIVATE usdGen usdGenExprFrontend)
target_include_directories(testUsdGenSeExprOracle SYSTEM PRIVATE "${_usdgen_frontend_dir}")
target_compile_definitions(testUsdGenSeExprOracle PRIVATE
    SeExpr2=UsdGenSeExprFrontend SeExprInternal2=UsdGenSeExprFrontendInternal
    USDGEN_SEEXPR_SANDBOX)
if(WIN32)
    target_compile_definitions(testUsdGenSeExprOracle PRIVATE SEEXPR_WIN32)
endif()
add_test(NAME testUsdGenSeExprOracle COMMAND testUsdGenSeExprOracle)
set_tests_properties(testUsdGenSeExprOracle PROPERTIES
    LABELS "T0;expressions;parity" TIMEOUT 120)
# Unix-only: the harness is a bash script that sources bin/_env.sh, which is
# POSIX shell and resolves a Linux interpreter path. Registered unconditionally
# this invoked WSL's bash on Windows and failed on a 120 s timeout rather than
# reporting anything about the schema.
usdgen_add_unix_only_test(testUsdGenExpressionSchema
    bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/checks/run_expression_schema.sh")
set_tests_properties(testUsdGenExpressionSchema PROPERTIES
    LABELS "T1;expressions;schema" TIMEOUT 120
    ENVIRONMENT_MODIFICATION "GENBUILD=set:${CMAKE_CURRENT_BINARY_DIR}"
    SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
    SKIP_REGULAR_EXPRESSION "SKIP")

# Note: testUsdGenInvalidation already carries T1;gate:SI-2 (set at line ~446).
# The SI-2 overlay extension (bare primvars locator, section 7) is appended
# within that same test and shares its executable; no new CMake target needed.
set_tests_properties(testUsdGenScheduling PROPERTIES LABELS "T1;gate:SI-3")
set_tests_properties(testUsdGenSnapshotRace PROPERTIES LABELS "T1;gate:SI-4")
set_tests_properties(testUsdGenPopulation PROPERTIES LABELS "T1;gate:SI-6" TIMEOUT 120)
set_tests_properties(testUsdGenAdapter PROPERTIES LABELS "T1;gate:SI-7")
set_tests_properties(testUsdGenRestAdapter PROPERTIES LABELS "T1;gate:SI-7")
set_tests_properties(testUsdGenCurveRestAdapter PROPERTIES LABELS "T1;gate:SI-7")
if(USDGEN_ENABLE_CUDA)
    add_executable(testUsdGenCudaHierarchy tests/testUsdGenCudaHierarchy.cpp)
    target_link_libraries(testUsdGenCudaHierarchy PRIVATE usdGenTestUtilsHd usdGenGpu)
    target_compile_definitions(testUsdGenCudaHierarchy PRIVATE USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME testUsdGenCudaHierarchy COMMAND testUsdGenCudaHierarchy)
    set_tests_properties(testUsdGenCudaHierarchy PROPERTIES
        ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" LABELS "T1;cuda;hierarchy" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
endif()
# testUsdGenAutoApply (gate:SI-8) tested UsdGenMaskAPI auto-apply; the mask
# feature (mask.cpp/h, maskParams.h) and its test were removed together, so
# there is no longer a target here to attach a LABELS override to.
set_tests_properties(testUsdGenContracts PROPERTIES LABELS "T1")

# N-5 (09 §5.2, 10 §5.6): schema regeneration-drift guard — the checked-in
# plugin/usdGenSchema/resources must stay byte-identical to a fresh
# usdGenSchema run over libs/usdGenSchema/schema.usda (bin/gen_schema.sh is
# the reference recipe; the N-2 plugInfo post-processing drift is what this
# catches in CI). The source has landed (plan/13 §1); the else() echo-stub
# branch below is retained only for missing sources (never a fake pass).
if(WIN32)
    # The drift guard runs bin/gen_schema.sh (a POSIX set -eu/mktemp/cmp recipe)
    # through popen, so it is Unix-only.
    usdgen_add_unix_only_test(testUsdGenSchemaUpToDate testUsdGenSchemaUpToDate)
    set(_usdgen_schema_uptodate_props SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/testUsdGenSchemaUpToDate.cpp")
    add_executable(testUsdGenSchemaUpToDate tests/testUsdGenSchemaUpToDate.cpp)
    target_link_libraries(testUsdGenSchemaUpToDate PRIVATE usdGenSchema usd tf arch)
    target_compile_definitions(testUsdGenSchemaUpToDate PRIVATE
        USDGEN_TEST_SCHEMA_USDA="${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGenSchema/schema.usda"
        USDGEN_TEST_SCHEMA_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources"
        USDGEN_TEST_USD_GEN_SCHEMA_TOOL="${USD_INSTALL_DIR}/bin/usdGenSchema")
    add_test(NAME testUsdGenSchemaUpToDate COMMAND testUsdGenSchemaUpToDate)
    set(_usdgen_schema_uptodate_props SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
else()
    add_test(NAME testUsdGenSchemaUpToDate
        COMMAND ${CMAKE_COMMAND} -E echo
            "M1-STUB testUsdGenSchemaUpToDate (tests/testUsdGenSchemaUpToDate.cpp pending wave 3)")
    set(_usdgen_schema_uptodate_props SKIP_REGULAR_EXPRESSION "M1-STUB")
endif()
set_tests_properties(testUsdGenSchemaUpToDate PROPERTIES
    LABELS "T1" ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" ${_usdgen_schema_uptodate_props})

# usdGenTools C ABI (usdGenImaging/usdGenToolsApi.h): the compile/list entry
# points the usdview expression editor calls through ctypes. Pure C ABI over
# the engine frontend, so it is an ordinary T1 with no stage and no Hydra.
add_executable(testUsdGenToolsApi tests/testUsdGenToolsApi.cpp)
target_link_libraries(testUsdGenToolsApi PRIVATE usdGenImaging)
add_test(NAME testUsdGenToolsApi COMMAND testUsdGenToolsApi)
set_tests_properties(testUsdGenToolsApi PROPERTIES
    LABELS "T1;tools;expressions" TIMEOUT 60)

# usdGenTonic scene-index publication + C ABI (plan/17 section 6, plan/18
# section 2.6). Portable T1s (no stage, no renderer, no Unix-only APIs): the
# index test drives UsdGenTonicSceneIndex over an empty retained scene index
# and asserts the registry join, the per-level publication and leaf-exact
# dirties; the API test drives tonicApi.h, including the V0 registry and
# level-display entries with no index attached.
add_executable(testUsdGenTonicIndex tests/testUsdGenTonicIndex.cpp)
target_link_libraries(testUsdGenTonicIndex PRIVATE usdGenTonic hd)
add_test(NAME testUsdGenTonicIndex COMMAND testUsdGenTonicIndex)
set_tests_properties(testUsdGenTonicIndex PROPERTIES
    ENVIRONMENT "USDGENTONIC_TEST_TUBE=1"
    LABELS "T1;tonic" TIMEOUT 120)
# The opt-out the gate record_usd.ps1 relies on: with USDGENTONIC_TEST_TUBE=0
# (and by default — the binary clears the ambient variable first, so this
# pins the default itself) the index constructs empty (no scaffolding in
# headless frames) and still publishes the model's levels on Tonic_Activate.
add_test(NAME testUsdGenTonicIndexNoTestTube
    COMMAND testUsdGenTonicIndex --no-test-tube)
set_tests_properties(testUsdGenTonicIndexNoTestTube PROPERTIES
    ENVIRONMENT "USDGENTONIC_TEST_TUBE=0"
    LABELS "T1;tonic" TIMEOUT 120)
add_executable(testUsdGenTonicApi tests/testUsdGenTonicApi.cpp)
target_link_libraries(testUsdGenTonicApi PRIVATE usdGenTonic)
add_test(NAME testUsdGenTonicApi COMMAND testUsdGenTonicApi)
set_tests_properties(testUsdGenTonicApi PROPERTIES
    LABELS "T1;tonic" TIMEOUT 60)

# usdGenTonic P1 (plan/17 section 6): the asynchronous commit pipeline -
# worker layer builds, idle swaps, coalescing, cancellation,
# no-swap-during-gesture, the partial-transfer fallback, hydrate round-trip,
# the GuideInterpolate fill-in, Save groom, and gate TN-4 (reference-scale
# swap <= 5 ms). Needs the schema plugin for the typed groom prims, so it
# carries the same environment as every adapter-dependent T1 test.
add_executable(testUsdGenTonicCommit tests/testUsdGenTonicCommit.cpp)
target_link_libraries(testUsdGenTonicCommit PRIVATE usdGenTonic usdGenImaging usd usdGeom sdf)
add_test(NAME testUsdGenTonicCommit COMMAND testUsdGenTonicCommit)
set_tests_properties(testUsdGenTonicCommit PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}"
    LABELS "T1;tonic;gate:TN-4" TIMEOUT 300
    SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

# Sparse Output: cage guides and a PTex ownership map feed runtime
# CurveSource interpolation -> Width, evaluated by the real engine.
add_executable(testUsdGenTonicOutput tests/testUsdGenTonicOutput.cpp)
target_link_libraries(testUsdGenTonicOutput PRIVATE usdGenTonic usdGenTestUtilsHd)
add_test(NAME testUsdGenTonicOutput COMMAND testUsdGenTonicOutput)
set_tests_properties(testUsdGenTonicOutput PROPERTIES
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}"
    LABELS "T1;tonic;output" TIMEOUT 300
    SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

# P2 scalp graph + region bake (plan/17). T0: K1/K2/K3 CPU twins, G1 graph
# ops, the model graph layer and the P2 C ABI (TN-6 parity runs inside when
# the build has CUDA and the host has a device).
add_executable(testUsdGenTonicGraph tests/testUsdGenTonicGraph.cpp)
target_link_libraries(testUsdGenTonicGraph PRIVATE usdGenTonic)
if(USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenTonicGraph PRIVATE CUDA::cudart)
endif()
add_test(NAME testUsdGenTonicGraph COMMAND testUsdGenTonicGraph)
set_tests_properties(testUsdGenTonicGraph PROPERTIES
    LABELS "T0;tonic" TIMEOUT 300)

# P3 tubes + fill (plan/17). T0: K4/K5/K8-K11 CPU twins, the model tube/fill
# layers, auto-tube, the P3 C ABI (TN-6 parity runs inside when the build
# has CUDA and the host has a device).
add_executable(testUsdGenTonicTubes tests/testUsdGenTonicTubes.cpp)
target_link_libraries(testUsdGenTonicTubes PRIVATE usdGenTonic)
if(USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenTonicTubes PRIVATE CUDA::cudart)
endif()
add_test(NAME testUsdGenTonicTubes COMMAND testUsdGenTonicTubes)
set_tests_properties(testUsdGenTonicTubes PROPERTIES
    LABELS "T0;tonic" TIMEOUT 300)

# Exact sub-face graph regions: guide roots and final K9/K10 guide starts
# must follow closed polygons rather than the coarse face map's centroid.
add_executable(testUsdGenTonicSubface tests/testUsdGenTonicSubface.cpp)
target_link_libraries(testUsdGenTonicSubface PRIVATE usdGenTonic)
add_test(NAME testUsdGenTonicSubface COMMAND testUsdGenTonicSubface)
set_tests_properties(testUsdGenTonicSubface PROPERTIES
    LABELS "T0;tonic" TIMEOUT 300)

# Region-rooted tubes retain their support-plane frame at section zero while
# later center CV edits bend the shaft.  This verifies the final host mesh,
# snapshot/hydrate, and the unpinned legacy K4 path.
add_executable(testUsdGenTonicRootFrame tests/testUsdGenTonicRootFrame.cpp)
target_link_libraries(testUsdGenTonicRootFrame PRIVATE usdGenTonic)
add_test(NAME testUsdGenTonicRootFrame COMMAND testUsdGenTonicRootFrame)
set_tests_properties(testUsdGenTonicRootFrame PROPERTIES
    LABELS "T0;tonic" TIMEOUT 300)

# Kernel registry + per-move chain smoke (plan/17 section 8). T0: the section
# 4.2 move chain on the CPU twins, determinism, P4 param validation, the
# K6/K7/K14 twins, and the TN-6 parity of every hierarchy device lane over
# the section 7 reference fanout. A CUDA build with no device runs the CPU
# half and then exits 77: TN-6 is what this test is for, so an unprovable
# run reports SKIPPED rather than passing quietly. A CPU-only build makes no
# parity claim and exits 0.
add_executable(testUsdGenTonicKernels tests/testUsdGenTonicKernels.cpp)
target_link_libraries(testUsdGenTonicKernels PRIVATE usdGenTonic)
if(USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenTonicKernels PRIVATE CUDA::cudart)
endif()
add_test(NAME testUsdGenTonicKernels COMMAND testUsdGenTonicKernels)
set_tests_properties(testUsdGenTonicKernels PROPERTIES
    LABELS "T0;tonic" TIMEOUT 300
    SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

# P4 hierarchy core (plan/17 section 8). T0: K14 subdivide/merge (Hausdorff
# union bar, bit-exact round-trip via the persistent-parent hint,
# determinism, edge mode) + K7 averaging (idempotence, propagation).
add_executable(testUsdGenTonicHierarchy tests/testUsdGenTonicHierarchy.cpp)
target_link_libraries(testUsdGenTonicHierarchy PRIVATE usdGenTonic)
if(USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenTonicHierarchy PRIVATE CUDA::cudart)
endif()
add_test(NAME testUsdGenTonicHierarchy COMMAND testUsdGenTonicHierarchy)
set_tests_properties(testUsdGenTonicHierarchy PROPERTIES
    LABELS "T0;tonic" TIMEOUT 300)

# T1: the versioned .ptx through the real ptex() expression (per-strand
# equality with the live primvar), the bake worker, the swap, and the
# graph/primvar/map commit round-trip.
add_executable(testUsdGenTonicRegionBake tests/testUsdGenTonicRegionBake.cpp)
target_link_libraries(testUsdGenTonicRegionBake PRIVATE
    usdGenTonic usdGen usdGen_ptex usd sdf usdGeom)
add_test(NAME testUsdGenTonicRegionBake COMMAND testUsdGenTonicRegionBake)
set_tests_properties(testUsdGenTonicRegionBake PROPERTIES
    LABELS "T1;tonic" TIMEOUT 600)


# usdGenTools SeExpr editor: drives usdview through testusdview against the
# expression example. Needs the prefix's usdview (a Python script) and a
# python that imports its pxr, so it registers only where both exist, and it
# opens a window, so it is T2 (not part of the T0|T1 run).
# The interpreter must be the one that imports the prefix's pxr: PY in the
# environment (the usdRig convention), else the shebang testusdview itself
# carries, else whatever is on PATH. On Windows a plain find_program would
# pick the Store's python3.exe alias, which cannot load these modules.
set(_usdgen_test_python "$ENV{PY}")
if(NOT _usdgen_test_python AND EXISTS "${USD_INSTALL_DIR}/bin/testusdview")
    file(STRINGS "${USD_INSTALL_DIR}/bin/testusdview" _usdgen_testusdview_shebang LIMIT_COUNT 1)
    string(REGEX REPLACE "^#![ \t]*" "" _usdgen_shebang_python "${_usdgen_testusdview_shebang}")
    if(EXISTS "${_usdgen_shebang_python}")
        set(_usdgen_test_python "${_usdgen_shebang_python}")
    endif()
endif()
if(NOT _usdgen_test_python)
    find_program(_usdgen_test_python NAMES python3 python NO_CACHE)
endif()
if(_usdgen_test_python AND EXISTS "${USD_INSTALL_DIR}/bin/testusdview")
    add_test(NAME testUsdGenToolsExprEditor
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/testenv/testUsdGenToolsExprEditor.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/expression-width-plane.usda")
    usdgen_test_pluginpath_ops(_usdgen_tools_plugin_env
        "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
        "${_usdgen_shader_resources}" "${_usdgen_tools_resources}"
        "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
    # Both python package layouts OpenUSD installs on Windows/POSIX, plus
    # build/python where the usdGenTools package is staged. Last, the
    # nanousd scrub (bin/launch_env/sitecustomize.py): the last prepend
    # lands first on PYTHONPATH, so the scrub's sitecustomize wins and a
    # nanousd editable install cannot shadow the prefix's pxr. A no-op
    # where nanousd is absent.
    usdgen_test_path_list_ops(_usdgen_tools_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_BINARY_DIR}/python"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")
    set_tests_properties(testUsdGenToolsExprEditor PROPERTIES
        LABELS "T2;tools;usdview" TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tools_plugin_env};${_usdgen_tools_python_env}")

    # Brush tool through REAL mouse events (plan/08 section 2.1 exit):
    # bind a UsdGenPaintMap to /Plane, press-drag-release over the
    # viewport, the session preview repaints while the drag is live, and
    # release bakes the paint primvar into the edit target. T3: opens a
    # window through testusdview like the Tonic mode scripts.
    add_test(NAME testUsdviewBrushStroke
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/testenv/testUsdviewBrushStroke.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/test-plane.usda")
    set_tests_properties(testUsdviewBrushStroke PROPERTIES
        LABELS "T3;tools;brush;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tools_plugin_env};${_usdgen_tools_python_env}")

    # Description setup through REAL mouse events: the palette's Setup
    # defines a ready-to-cook description (Scatter -> Grow Ops), paint-to
    # binds its Maps PaintMap, the cursor ring tracks the hover, and a
    # stroke bakes into the Maps primvar. The extra FAIL tokens are the
    # engine errors an operator-less description used to emit per frame.
    add_test(NAME testUsdviewBrushSetup
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/testenv/testUsdviewBrushSetup.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/test-plane.usda")
    set_tests_properties(testUsdviewBrushSetup PROPERTIES
        LABELS "T3;tools;brush;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback|operatorOrder|no operator nodes|CaptureGraphDescFromHydra|commit rejected"
        ENVIRONMENT_MODIFICATION "${_usdgen_tools_plugin_env};${_usdgen_tools_python_env}")
endif()

# Graph mode under testusdview. T3: opens a window, needs the staged
# usdGenTonicTools package (same build/python) and the usdGenTonic DLL
# (USDGENTONIC_DLL, defaulting to the build tree).
#
# testUsdviewTonicGraph drives REAL mouse and key events over the StageView
# through the V2 viewport controller (plan/18 V2 exit). The ABI-level
# scripts beside it -- testTonicAbi* -- call the C entry points directly and
# never open the tool; they keep their coverage under an honest name
# (plan/18 section 4 "T3"), and the modes whose loops land in V4/V5 have
# only the ABI script until then.
if(_usdgen_test_python AND EXISTS "${USD_INSTALL_DIR}/bin/testusdview")
    add_test(NAME testUsdviewTonicGraph
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicGraph.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    usdgen_test_pluginpath_ops(_usdgen_tonic_plugin_env
        "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
        "${_usdgen_shader_resources}"
        "${_usdgen_tonic_resources}" "${_usdgen_tonic_tools_resources}"
        "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
    # bin/launch_env last so its sitecustomize lands first on PYTHONPATH and
    # scrubs a nanousd editable install's pxr shadow (no-op otherwise).
    usdgen_test_path_list_ops(_usdgen_tonic_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_BINARY_DIR}/python"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")
    set_tests_properties(testUsdviewTonicGraph PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # The graph model through the C ABI: two adjoining regions, welded, with
    # the shared boundary checked (plan/17 P2 exit). No viewport.
    add_test(NAME testTonicAbiGraph
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicAbiGraph.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testTonicAbiGraph PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # P3 Tube+Fill modes through the C ABI (same stage + environment).
    add_test(NAME testTonicAbiTube
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicAbiTube.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testTonicAbiTube PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # P4 Hierarchy walk L1 -> L3 and back (Part B self-skips until the P4 C
    # entries land, then runs strict).
    add_test(NAME testTonicAbiLevels
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicAbiLevels.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testTonicAbiLevels PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TN-1/TN-2 probes + TN-3/TN-4/TN-7 skip-harness (plan/17 section 7 stub).
    add_test(NAME testUsdviewTonicPerf
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicPerf.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicPerf PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # V0 publication (plan/18 section 2.1/2.2 exit): one model created
    # through the C ABI, edited, activated and published, then asserted
    # against what Hydra ended up with -- the tubes/L1 and tubes/L2 prims
    # exist with the face counts the model's census predicts, and a pick
    # over the viewport hits them.
    add_test(NAME testUsdviewTonicPublish
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicPublish.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicPublish PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT "USDGENTONIC_TEST_TUBE=1"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # V5 Hierarchy mode through REAL mouse and key events (plan/18 V5
    # exit): select, Shift+D, Ctrl+Down/Up, Shift+M, Ctrl+Z, asserted
    # against what the scene index published per level.
    add_test(NAME testUsdviewTonicLevels
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicLevels.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicLevels PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # V5 Sculpt mode: the brush ring under a real cursor, a real grab
    # stroke that keeps its arc length, and Escape restoring bit-exactly.
    add_test(NAME testUsdviewTonicSculpt
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicSculpt.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicSculpt PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # V4 Tube + Fill through the real controller (plan/18 V4 exit): a real
    # stroke leaves a tube stub behind it (G14), a real click selects a
    # center CV and a real drag moves the published centers, Ctrl+Z puts
    # them back, the Fill panel's density reaches the guides, and TN-1 is
    # re-measured over 50 real mouse moves.
    add_test(NAME testUsdviewTonicTube
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicTube.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicTube PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # Real Qt input acceptance for hover, click and Shift-marquee selection.
    # This stays separate from the Tube workflow so selection routing and
    # StageView rollover ownership have a focused failure report.
    add_test(NAME testUsdviewTonicSelection
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicSelection.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicSelection PROPERTIES
        LABELS "T3;tonic;tools;usdview;selection" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # An artist-level hierarchy edit: subdivide in the dock, select one
    # displayed child component, drag its manipulator, and prove the other
    # child descriptors and world points remain untouched through commit/undo.
    add_test(NAME testUsdviewTonicIsolatedEditing
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicIsolatedEditing.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicIsolatedEditing PROPERTIES
        LABELS "T3;tonic;tools;usdview;selection;hierarchy" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TN-3 pick accuracy: K11 vs StageView.pick at 100 pixels (P6 exit).
    add_test(NAME testUsdviewTonicPick
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicPick.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicPick PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # The full interactive workflow on the plane fixture, ending in 4
    # connected tubes: a graph stroke grows the stub, Shift+D grows three
    # children, double-click drills to them, a child tip CV takes the
    # gizmo for a drag (undo/redo), a sculpt stroke moves the second
    # child, the Fill panel feeds the guides, and the commit lands nested
    # on the stage.
    add_test(NAME testUsdviewTonicWorkflow
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicWorkflow.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicWorkflow PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # Commit-enabled Output through the real viewport controller.  The
    # script owns the panel workflow; this mirrors Workflow's fixture and
    # staged plugin/Python/DLL environment.
    add_test(NAME testUsdviewTonicOutput
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicOutput.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicOutput PROPERTIES
        LABELS "T3;tonic;tools;usdview;output" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # Click-by-click Graph regions on a single quad: Bind Geometry through
    # the dock, shared Ptex density UI, CV drafts, both closure paths, and
    # the one-step region+auto-tube undo/redo transaction.
    add_test(NAME testUsdviewTonicCvRegions
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicCvRegions.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-single-quad.usda")
    set_tests_properties(testUsdviewTonicCvRegions PROPERTIES
        LABELS "T3;tonic;tools;usdview;cv-regions" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # SS-01: File > Reopen keeps the live model (graph, undo stack and the
    # committed groom); a stage without the scalp keeps the session
    # detached with a missing-scalp warning instead of re-binding.
    add_test(NAME testUsdviewTonicReattach
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicReattach.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-single-quad.usda")
    set_tests_properties(testUsdviewTonicReattach PROPERTIES
        LABELS "T3;tonic;tools;usdview;session" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # SS-03: a dock save with "add to the scene" sublayers the groom in the
    # root layer, survives File > Reopen and comes back through Resume
    # groom; binding over a saved groom asks Resume / Start new / Cancel.
    # The script works on a temp copy of the scene (it saves the root layer).
    add_test(NAME testUsdviewTonicFileCommands
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicFileCommands.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-single-quad.usda")
    set_tests_properties(testUsdviewTonicFileCommands PROPERTIES
        LABELS "T3;tonic;tools;usdview;session" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # Graph Reposition through actual StageView drags: shared CV and edge
    # motion stays live in both incident regions, preserves stable topology
    # and root descriptors, and owns one undo / Escape transaction.
    add_test(NAME testUsdviewTonicReposition
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicReposition.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-single-quad.usda")
    set_tests_properties(testUsdviewTonicReposition PROPERTIES
        LABELS "T3;tonic;tools;usdview;graph;reposition" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TN-7 bake isolation (plan/17 section 7): the TN-1 drag re-measured
    # with a full bake of the REFERENCE SCALP in flight, on the reference
    # scene rather than the 4x4 test scalp -- the point of the gate is
    # that a real bake's worker thread and lowest-priority CUDA stream
    # take nothing from the gesture. The script proves the bake was still
    # running when the drag ended, so a bake that finished early fails
    # instead of passing vacuously.
    add_test(NAME testUsdviewTonicBakeIsolation
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicBakeIsolation.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-reference.usda")
    set_tests_properties(testUsdviewTonicBakeIsolation PROPERTIES
        LABELS "T3;tonic;tools;usdview;gate:TN-7" TIMEOUT 600
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # DK-05 artist status strip: the sync pill follows a real drag's commit
    # (Committing... then Synced), a selection publish stays calm, Relax and
    # an undo past the bottom reach the dock's message area, and every
    # Display control writes its state field.
    add_test(NAME testUsdviewTonicDock
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicDock.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicDock PROPERTIES
        LABELS "T3;tonic;tools;usdview;dock" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TS-04 artist walkthrough: one session start to finish with real input
    # (menu open, gated dock, picker bind, two regions, Tube gizmo/tweak/
    # Escape/undo, Fill density + ramp, Hierarchy subdivide/merge/re-split,
    # Sculpt, Output build, dock Save, warnings row, dock close/reopen, the
    # Synced pill); every CONFUSING line of the 2026-09-23 walkthrough is a
    # hard check.
    add_test(NAME testUsdviewTonicArtistWalkthrough
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicArtistWalkthrough.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-single-quad.usda")
    set_tests_properties(testUsdviewTonicArtistWalkthrough PROPERTIES
        LABELS "T3;tonic;tools;usdview;walkthrough" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TS-02 hotkey sweep: every documented key in every mode through real
    # QtTest key events (modes, sub-modes, F8-F11, Q/W/E/R, [ ], L, + = -,
    # P, J/X holds mid-drag, Escape cancel-only, Delete and its gate,
    # Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z / Shift+Z, Ctrl+A / Ctrl+Shift+A /
    # Ctrl+I, Hierarchy level keys, Ctrl+Shift+S).
    add_test(NAME testUsdviewTonicHotkeys
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicHotkeys.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicHotkeys PROPERTIES
        LABELS "T3;tonic;tools;usdview;hotkeys" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TS-02 navigation and focus: Alt camera drags over an active scalp,
    # host navigation presets (the Display group's Navigation combo), wheel
    # mid-drag, one render per camera move, no usdview rollover/pick/menu,
    # focus back from a dock field, dock close/reopen, F framing.
    add_test(NAME testUsdviewTonicNavigation
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicNavigation.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicNavigation PROPERTIES
        LABELS "T3;tonic;tools;usdview;navigation" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TS-03 mode coverage through real clicks and keys: the Graph click
    # tools (Connect/Link/Unweld/Delete, Shift+W/U), Fill (tube pick,
    # density row, length-ramp drag), Hierarchy ops (double-click,
    # Group/Make persistent, Merge selected, Solo, locks) and the Tube
    # actions (Relax, Match surface, Snap root). Shared fixture:
    # testenv/tonicT3Groom.py.
    add_test(NAME testUsdviewTonicGraphTools
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicGraphTools.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicGraphTools PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    add_test(NAME testUsdviewTonicFill
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicFill.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicFill PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    add_test(NAME testUsdviewTonicHierarchyOps
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicHierarchyOps.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicHierarchyOps PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    add_test(NAME testUsdviewTonicTubeActions
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdviewTonicTubeActions.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testUsdviewTonicTubeActions PROPERTIES
        LABELS "T3;tonic;tools;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
    # TN-5 through the REAL controller (plan/18 V7): 30 minutes of QtTest
    # mouse and key gestures across every mode. T4 like its ABI sibling --
    # a wall-clock test, run with `ctest -L T4`, never by the default
    # suite (`ctest -R Tonic -LE T4`).
    add_test(NAME testTonicSoakController
        COMMAND "${_usdgen_test_python}" "${USD_INSTALL_DIR}/bin/testusdview"
            --testScript "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicSoakController.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/tonic-graph-scalp.usda")
    set_tests_properties(testTonicSoakController PROPERTIES
        LABELS "T4;tonic;tools;usdview;gate:TN-5" TIMEOUT 2400
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT "USDGENTONIC_SOAK_MINUTES=30;USDGENTONIC_SOAK_SEED=7"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
endif()

# Tonic render goldens (plan/18 section 4 "Image", G15). Headless like
# testUsdGenStormLook -- bin/record_tonic.py makes its own offscreen GL
# context -- but unlike usdrecord it hydrates the committed groom into a
# live TonicModel first, so the tubes, level colours, center curves and
# CV dots the scene index publishes are in the frame. Same plugin path
# and PYTHONPATH as the T3 scripts, plus the DLL directory.
if(_usdgen_test_python AND EXISTS "${USD_INSTALL_DIR}/bin/testusdview")
    add_test(NAME testTonicGolden
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicGolden.py")
    set_tests_properties(testTonicGolden PROPERTIES
        LABELS "T2;tonic;tools;golden" TIMEOUT 900
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonic_plugin_env};${_usdgen_tonic_python_env};PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenTonic>")
endif()

# The Qt-free half of the same plugin -- the ctypes binding, the text helpers
# behind the literal controls, and the connection authoring -- runs in a plain
# python process, so it is a T1 and needs no usdview. It shares the editor
# test's interpreter and environment, minus the window.
if(_usdgen_test_python)
    add_test(NAME testUsdGenToolsExprApi
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/testenv/testUsdGenToolsExprApi.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/expression-width-plane.usda")
    usdgen_test_pluginpath_ops(_usdgen_toolsapi_plugin_env
        "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
        "${_usdgen_shader_resources}" "${_usdgen_tools_resources}"
        "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
    # bin/launch_env last: the last prepend lands first on PYTHONPATH, so
    # its sitecustomize scrubs a nanousd editable install's pxr shadow
    # before the test imports (a no-op where nanousd is absent).
    usdgen_test_path_list_ops(_usdgen_toolsapi_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_BINARY_DIR}/python"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")
    set_tests_properties(testUsdGenToolsExprApi PROPERTIES
        LABELS "T1;tools;expressions" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_toolsapi_plugin_env};${_usdgen_toolsapi_python_env}")

    # The Qt-free half of the brush tool -- the map/stroke twin, camera
    # maths, CPU surface pick, bind/bake authoring, session preview, the
    # gesture loop and the panel descriptors -- in a plain python process.
    add_test(NAME testUsdGenToolsBrush
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/testenv/testUsdGenToolsBrush.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/test-plane.usda")
    set_tests_properties(testUsdGenToolsBrush PROPERTIES
        LABELS "T1;tools;brush" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_toolsapi_plugin_env};${_usdgen_toolsapi_python_env}")

    # The Qt-free UI half of the brush (state, loop cadence, palette and
    # panel descriptors, hotkey logic) in the same plain interpreter.
    add_test(NAME testUsdGenToolsBrushUI
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/testenv/testUsdGenToolsBrushUI.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/test-plane.usda")
    set_tests_properties(testUsdGenToolsBrushUI PROPERTIES
        LABELS "T1;tools;brush" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_toolsapi_plugin_env};${_usdgen_toolsapi_python_env}")
endif()

# The Qt-free half of usdGenTonicTools -- modes, tool state, math kernels --
# runs in a plain python process (numpy only), so it is a T1 and needs no
# usdview. It shares the expression tests' interpreter.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsMath
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsMath.py")
    set_tests_properties(testUsdGenTonicToolsMath PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free Graph-mode half -- stroke kernels, snap conversion, HUD text,
# sub-modes -- runs in the same plain interpreter.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsGraph
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsGraph.py")
    set_tests_properties(testUsdGenTonicToolsGraph PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free half of the V2 viewport controller -- camera projection, the
# plan/18 section 3.4 key table, the gizmo state machine, and GraphLoop
# driven over a fake session that records every C call. No pxr, no DLL, no
# numpy, so it runs in the barest interpreter of the lot.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsLoops
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsLoops.py")
    set_tests_properties(testUsdGenTonicToolsLoops PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free half of the V4 loops (plan/18 V4): TubeLoop and FillLoop over
# a fake session -- the pick-once contract, one undo bracket per drag, the
# world-delta-to-section-chart conversion, the soft-selection anchor rule,
# and Fill's preview/full density contract. Same bare interpreter.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsLoopsTube
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsLoopsTube.py")
    set_tests_properties(testUsdGenTonicToolsLoopsTube PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The vendored RigExec gizmo maths (tonic_gizmo_parity G08/G09/G11/G16/G20/
# G24): AxisDragParameter's ratio-scaled floor, MayaScaleFactor, the step and
# world-grid snaps, the per-tool settings model and the World/Screen/Tube
# frames, ported from usdRig's test_gizmo_screen/snap/settings. Pure maths.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsGizmoScreen
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsGizmoScreen.py")
    set_tests_properties(testUsdGenTonicToolsGizmoScreen PROPERTIES
        LABELS "T0;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free half of the V5 loops (plan/18 V5): HierarchyLoop's actions and
# their undo brackets, the drawn-edge subdivide, SculptLoop's one shaped
# stroke per move, and the fallback ladder stepped on synthetic timings.
# Same bare interpreter as its V2 sibling, whose recording fakes it reuses.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsLoopsHier
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsLoopsHier.py")
    set_tests_properties(testUsdGenTonicToolsLoopsHier PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The TN-5 soak (plan/17 section 7): 30 minutes of scripted editing over a
# reference-fanout model. Registered so it can be RUN -- `ctest -L T4` --
# but never by the default suite: it is a wall-clock test, which is what
# the T4 label means here (the audit's G15 row: "TN-5 soak not registered
# in ctest"). --smoke cuts it to a minute for a spot check.
if(_usdgen_test_python)
    add_test(NAME testTonicSoak
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicSoak.py"
            --minutes 30)
    # The soak imports the prefix's pxr through tonicLib; bin/launch_env
    # first so its sitecustomize scrubs a nanousd editable install's shadow.
    usdgen_test_path_list_ops(_usdgen_tonicsoak_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")
    set_tests_properties(testTonicSoak PROPERTIES
        LABELS "T4;tonic;tools;gate:TN-5" TIMEOUT 2400
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT "USDGENTONIC_DLL=$<TARGET_FILE:usdGenTonic>"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonicsoak_python_env}"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    # The smoke is 60 seconds and DOES run in the default suite, so it is
    # not T4: T4 here means "wall clock, excluded by -LE T4", and the two
    # 30-minute soaks (testTonicSoak, testTonicSoakController) are the
    # only tests that earn it.
    add_test(NAME testTonicSoakSmoke
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testTonicSoak.py"
            --smoke)
    set_tests_properties(testTonicSoakSmoke PROPERTIES
        LABELS "T1;tonic;tools;gate:TN-5" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT "USDGENTONIC_DLL=$<TARGET_FILE:usdGenTonic>"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonicsoak_python_env}"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free Tube/Fill-mode half -- gizmo previews, fill counts, ramps,
# sub-modes -- runs in the same plain interpreter.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsTube
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsTube.py")
    set_tests_properties(testUsdGenTonicToolsTube PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free Hierarchy/Sculpt-mode half -- level focus, breadcrumb, locks,
# visibility, subdivide/merge params, brush falloff -- same plain interpreter.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsHierarchy
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsHierarchy.py")
    set_tests_properties(testUsdGenTonicToolsHierarchy PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The Qt-free workspace-dock half (plan/18 V3): tonicPanels' per-mode
# parameter/action descriptors and tonicHud's status-strip/warnings
# formatting, exercised against a fake session (no ctypes DLL, no Qt) --
# same plain interpreter as its siblings above.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsPanels
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsPanels.py")
    set_tests_properties(testUsdGenTonicToolsPanels PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# Icon loader + name-table coverage (IC-01): every name tonicModes.ICONS /
# tonicPanels.ACTION_ICONS references resolves, when a PNG exists for it,
# to a square RGBA glyph tonicIcons can tint. Plain python + PIL, no Qt.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsIcons
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsIcons.py")
    set_tests_properties(testUsdGenTonicToolsIcons PROPERTIES
        LABELS "T1;tonic;tools;icons" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# DOC-01: docs/tonic-tool.md names every hotkey the tool binds -- the
# tonicModes shelf tables, HotkeyAction, the dock's HINTS lines and the
# viewport's J/X holds and +/- keys, all read from the code. Plain python,
# no pxr or Qt.
if(_usdgen_test_python)
    add_test(NAME testTonicDocsHotkeys
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/checks/check_tonic_docs.py")
    set_tests_properties(testTonicDocsHotkeys PROPERTIES
        LABELS "T0;tonic;tools;docs" TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback")
endif()

# TS-01: the Qt-free half of tonicT3.py, the shared T3 driver module --
# typeKey's key table against tonicViewport.keyName's own source, and the
# check/info/statusRecorder helpers -- same plain interpreter as its
# siblings above (no pxr, no Qt).
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsT3Helpers
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsT3Helpers.py")
    set_tests_properties(testUsdGenTonicToolsT3Helpers PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# The P5 bridge half -- center-curve export, swept/curve L3 import, the
# geodesic checks -- runs headless but needs pxr (an in-memory stage) and
# the usdGenTonic DLL, so unlike its Qt-free siblings it takes the USD
# python environment plus USDGENTONIC_DLL. Part B is strict here (the
# variable's presence is what makes it strict) and a skip for direct
# runs without them.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsBridge
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsBridge.py")
    usdgen_test_path_list_ops(_usdgen_tonicbridge_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")
    set_tests_properties(testUsdGenTonicToolsBridge PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT "USDGENTONIC_DLL=$<TARGET_FILE:usdGenTonic>"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonicbridge_python_env}")
endif()

# The V0b stage-contract binding (plan/18 V0b): tonicLibStage.py over the
# real DLL -- the per-tube operations, the per-tube fill params, and a
# commit/hydrate round trip through the binding. Same environment as the
# bridge test above (USD python + USDGENTONIC_DLL), same skip for direct
# runs without them.
if(_usdgen_test_python)
    add_test(NAME testUsdGenTonicToolsStage
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsStage.py")
    usdgen_test_path_list_ops(_usdgen_tonicstage_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")
    set_tests_properties(testUsdGenTonicToolsStage PROPERTIES
        LABELS "T1;tonic;tools" TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT "USDGENTONIC_DLL=$<TARGET_FILE:usdGenTonic>"
        ENVIRONMENT_MODIFICATION "${_usdgen_tonicstage_python_env}"
        SKIP_RETURN_CODE "77")
endif()

# ---------------------------------------------------------------------------
# Strand hair checks (plan/16-ue-hair-parity.md WS1). Three python checks,
# all cross-platform, all sharing the interpreter the expression tests found.
#
#   testUsdGenHairEnergy        integrates the ported BSDF over the sphere on
#                               the CPU and asserts it creates no energy. No
#                               pxr, no GL -- numpy only.
#   testUsdGenHairStrandsDefs   Sdr parse + registry discovery of the two new
#                               defs, and that the three C5 defs survive. The
#                               counterpart of usdGenShaders/test/checkC5.py,
#                               kept separate so C5 keeps asserting exactly
#                               three files with exactly 21 inputs.
#   testUsdGenHairStrandsRender a real Storm draw: differential checks that
#                               the glslfx COMPILED (a failed compile falls
#                               back silently) and that lights, the baked hair
#                               count and coverage all reach the pixels.
#
# Each exits 77 when its prerequisites are missing, which ctest reports as a
# skip rather than a failure.
# ---------------------------------------------------------------------------
if(_usdgen_test_python)
    usdgen_test_pluginpath_ops(_usdgen_hair_plugin_env
        "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
        "${_usdgen_shader_resources}"
        "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
    usdgen_test_path_list_ops(_usdgen_hair_python_env PYTHONPATH path_list_prepend
        "${USD_INSTALL_DIR}/lib/python" "${USD_INSTALL_DIR}/Lib/site-packages"
        "${CMAKE_CURRENT_BINARY_DIR}/python"
        "${CMAKE_CURRENT_SOURCE_DIR}/bin/launch_env")

    add_test(NAME testUsdGenHairEnergy
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test/check_hair_energy.py")
    set_tests_properties(testUsdGenHairEnergy PROPERTIES
        LABELS "T1;shaders;gate:L-2" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

    # fur_gen.py MoonRay fallback (docs/moonray-fur.md): the generated
    # PointInstancer fur must carry a universal UsdPreviewSurface terminal
    # with a displayColor primvar reader beside its Storm glslfx terminal.
    # Stdlib only, no pxr -- nothing to skip on.
    add_test(NAME testUsdGenFurGenMaterial
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/checks/check_fur_gen_material.py")
    set_tests_properties(testUsdGenFurGenMaterial PROPERTIES
        LABELS "T1;fur" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback")

    # launch_usdview.ps1 MoonRay wiring (docs/moonray-fur.md): the
    # launcher must place the MoonRay build's hd_moonray plugInfos ahead
    # of the OpenUSD prefix, its bin/ ahead of the prefix on PATH, and
    # set ARRAS_SESSION_PATH/RDL2_DSO_PATH for interactive rendering.
    # Skips (77) off Windows, without powershell, or without a MoonRay
    # build beside the repo.
    add_test(NAME testUsdGenUsdviewEnv
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/checks/check_usdview_env.py")
    set_tests_properties(testUsdGenUsdviewEnv PROPERTIES
        LABELS "T1;usdview" TIMEOUT 300
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

    add_test(NAME testUsdGenHairStrandsDefs
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test/check_hair_strands_defs.py")
    set_tests_properties(testUsdGenHairStrandsDefs PROPERTIES
        LABELS "T1;shaders;gate:L-2" TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_hair_plugin_env};${_usdgen_hair_python_env}"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

    add_test(NAME testUsdGenScalpShadowDef
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test/check_scalp_shadow.py"
            --no-render)
    set_tests_properties(testUsdGenScalpShadowDef PROPERTIES
        LABELS "T1;shaders;shadows" TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_hair_plugin_env};${_usdgen_hair_python_env}"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

    # GPU-bound, so T2 and RUN_SERIAL like the other Storm tests.
    add_test(NAME testUsdGenScalpShadowRender
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test/check_scalp_shadow.py"
            --usdrecord "${USD_INSTALL_DIR}/bin/usdrecord")
    set_tests_properties(testUsdGenScalpShadowRender PROPERTIES
        LABELS "T2;shaders;shadows" TIMEOUT 600 RUN_SERIAL TRUE
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_hair_plugin_env};${_usdgen_hair_python_env}"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

    # GPU-bound, so T2 and RUN_SERIAL like the other Storm tests.
    add_test(NAME testUsdGenHairStrandsRender
        COMMAND "${_usdgen_test_python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test/check_hair_strands_render.py"
            --usdrecord "${USD_INSTALL_DIR}/bin/usdrecord")
    set_tests_properties(testUsdGenHairStrandsRender PROPERTIES
        LABELS "T2;shaders;gate:L-2" TIMEOUT 600 RUN_SERIAL TRUE
        FAIL_REGULAR_EXPRESSION "FAIL:|Traceback"
        ENVIRONMENT_MODIFICATION "${_usdgen_hair_plugin_env};${_usdgen_hair_python_env}"
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endif()

# M1 T2 Storm tests (EGL harness; the S-8/S-9/L-1 PW decisions get confirmed
# on shipped code here, 11 §2.2 exit-gate table).
if(NOT WIN32)
    add_executable(testUsdGenStormLook tests/testUsdGenStormLook.cpp)
    target_link_libraries(testUsdGenStormLook PRIVATE usdGenTestUtilsHd)
    target_include_directories(testUsdGenStormLook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test)
endif()
usdgen_add_unix_only_test(testUsdGenStormLook testUsdGenStormLook ${PROJECT_SOURCE_DIR})
set_tests_properties(testUsdGenStormLook PROPERTIES
    LABELS "T2;gate:L-2" ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

if(NOT WIN32)
    add_executable(benchUsdGenStorm tests/perf/benchUsdGenStorm.cpp)
    target_link_libraries(benchUsdGenStorm PRIVATE usdGenTestUtilsHd)
    target_include_directories(benchUsdGenStorm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test)
endif()
usdgen_add_unix_only_test(benchUsdGenStorm benchUsdGenStorm
    --scene ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/g3_head100k.usda
    --static --refine 2)
# --scene is required (bare bench exits 2): the S-1 scene is the G3
# canonical groom at refineLevel 2 (the S-1 gate condition, 09 §5.3).
# The fixture is generated (usdGenFixtures below; gitignored build
# artifact like the binaries); a missing file fails loudly in OpenScene,
# never silently. HD_ENABLE_PERFLOG=1 feeds the drawBatches/drawCalls/
# itemsDrawn counters the S-5/S-6/S-12 assertions read.
set_tests_properties(benchUsdGenStorm PROPERTIES
    LABELS "T2;perf;gate:S-1;gate:S-5;gate:S-6;gate:S-12"
    ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}" ENVIRONMENT "HD_ENABLE_PERFLOG=1" SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

# Groom fixtures G1-G5 (S-1/S-5/S-6/S-12/E-5 scenes): pure-text output of
# usd/fixtures/make_fixtures.py (stdlib only, no pxr), so T2 stays
# self-contained wherever python3 exists. Regeneration is byte-identical
# (short decimals, fixed emission order); the files stay gitignored.
if(NOT WIN32)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/g3_head100k.usda
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/make_fixtures.py
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/make_fixtures.py
        COMMENT "Generating usdGen groom fixtures G1-G5 (gitignored)"
        VERBATIM)
    add_custom_target(usdGenFixtures ALL
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/g3_head100k.usda)
    add_dependencies(benchUsdGenStorm usdGenFixtures)
endif()

# N-4 (09 §5.3/§5.4, 10 §5.6 + §6.5): the remaining M1 Storm decision gates.
# S-8 (tangent strategy PW decision) needs both the variant A/B timing
# comparison (`testUsdGenStormTangent`) and the
# HDST_ENABLE_HGI_RESOURCE_GENERATION=1 compile (`testUsdGenStormHgiResource`);
# S-9 is the refineLevel live switch, L-1 the preview-material resolution.
# All sources except testUsdGenStormSurgery have landed (plan/13 §1, §3.3);
# the else() echo branch below is retained for missing sources so
# `ctest -N -L 'gate:'` still resolves every M1 gate. GPU-bound tests:
# RUN_SERIAL keeps one Storm context per ctest slot.
set(_usdgen_storm_labels_testUsdGenStormTangent  "T2;gate:S-8")
set(_usdgen_storm_labels_testUsdGenStormHgiResource "T2;gate:S-8")
set(_usdgen_storm_labels_testUsdGenStormRefine   "T2;gate:S-9")
set(_usdgen_storm_labels_testUsdGenStormMaterial "T2;gate:L-1")
set(_usdgen_storm_labels_testUsdGenStormSurgery  "T2")
foreach(_t testUsdGenStormTangent testUsdGenStormHgiResource
           testUsdGenStormRefine  testUsdGenStormMaterial
           testUsdGenStormSurgery)
    if(NOT WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_t}.cpp")
        add_executable(${_t} tests/${_t}.cpp)
        target_link_libraries(${_t} PRIVATE usdGenTestUtilsHd)
        target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test)
        set(_usdgen_storm_cmd ${_t} ${PROJECT_SOURCE_DIR})
        set(_usdgen_storm_props SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    elseif(WIN32)
        set(_usdgen_storm_cmd
            ${CMAKE_COMMAND} -E echo "SKIP ${_t} (Unix-only EGL/Storm harness)")
        set(_usdgen_storm_props SKIP_REGULAR_EXPRESSION "SKIP")
    else()
        set(_usdgen_storm_cmd
            ${CMAKE_COMMAND} -E echo "M1-STUB ${_t} (pending wave-3 source)")
        set(_usdgen_storm_props SKIP_REGULAR_EXPRESSION "M1-STUB")
    endif()
    add_test(NAME ${_t} COMMAND ${_usdgen_storm_cmd})
    set_tests_properties(${_t} PROPERTIES
        LABELS "${_usdgen_storm_labels_${_t}}"
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "${_usdgen_m1_env}"
        ${_usdgen_storm_props})
endforeach()


# ---------------------------------------------------------------------------
# M0 install-tree tests (plan §6.2, sol S-5/X-4)
#
# Both tests read the install prefix from the USDGEN_INSTALL_PREFIX env var.
# When unset they print a note and return 0 (clean skip). CI sets
# USDGEN_INSTALL_PREFIX to the scratch prefix after running cmake --install.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    add_executable(testUsdGenInstallTree tests/testUsdGenInstallTree.cpp)
    target_link_libraries(testUsdGenInstallTree PRIVATE
        usd usdGeom tf arch gf vt sdf trace)
    target_compile_definitions(testUsdGenInstallTree PRIVATE
        USDGEN_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes/scene_empty_groom.usda"
        USDGEN_TEST_PLUGIN_DIR="${USDGEN_INSTALL_PLUGINDIR}"
        USDGEN_TEST_LIB_DIR="${USDGEN_INSTALL_LIBDIR}"
        USDGEN_TEST_CMAKE_DIR="${USDGEN_INSTALL_CMAKEDIR}"
        USDGEN_TEST_USD_INSTALL_DIR="${USD_INSTALL_DIR}")

    add_executable(testUsdGenConsumer tests/testUsdGenConsumer.cpp)
    target_compile_definitions(testUsdGenConsumer PRIVATE
        USDGEN_TEST_CONSUMER_SRC="${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer"
        USDGEN_TEST_USD_INSTALL_DIR="${USD_INSTALL_DIR}"
        USDGEN_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}")
endif()
# Both assert the .so/.a install layout and drive POSIX execv/setenv or a
# POSIX shell, so they cannot be built for Windows.
usdgen_add_unix_only_test(testUsdGenInstallTree testUsdGenInstallTree)
set_tests_properties(testUsdGenInstallTree PROPERTIES
    LABELS "T0;install")
usdgen_add_unix_only_test(testUsdGenConsumer testUsdGenConsumer)
set_tests_properties(testUsdGenConsumer PROPERTIES
    LABELS "T0;install")

# ---------------------------------------------------------------------------
# Loader search path for every registered test.
#
# This is a sweep rather than a per-registration property because *all* tests
# need it, including the T0 engine tests that carry no other environment: they
# link usdGen, which is a shared library, and OpenUSD's libraries come along
# with it. On Windows, where there is no RPATH, omitting this is what made the
# whole suite abort with STATUS_DLL_NOT_FOUND (0xc0000135) before the test body
# ever ran. Sweeping the registered list also keeps tests added later correct
# without every registration having to remember.
# ---------------------------------------------------------------------------
usdgen_test_path_list_ops(_usdgen_runtime_ops
    "${USDGEN_RUNTIME_LIBRARY_PATH_ENV}" path_list_prepend
    ${USDGEN_TEST_RUNTIME_DIRS})
get_property(_usdgen_registered_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _usdgen_registered_tests)
    set_property(TEST ${_t} APPEND PROPERTY
        ENVIRONMENT_MODIFICATION "${_usdgen_runtime_ops}")
endforeach()

endif() # USDGEN_BUILD_TESTS

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
# N-7 (plan §1.2/§1.6): only the shipped targets are installed and
# exported. usdGenMath, usdGen_seexpr and nanoflann are private dependency
# archives (linked hidden, --exclude-libs,ALL, symbols verified out of the
# shipped objects) — installing them would put libusdGenMath.a /
# libusdGen_seexpr.a into lib/ and drag their unprefixed INTERFACE include
# roots into the usdGenTargets export set.
install(TARGETS usdGenExecutionResources usdGen usdGenImaging usdGenSchema usdGenShaders usdGenTonic
    EXPORT usdGenTargets
    RUNTIME DESTINATION "${USDGEN_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${USDGEN_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${USDGEN_INSTALL_LIBDIR}")


# Schema + imaging plugin resources.
#
# The codeless schema declares implementsComputeExtent on UsdGenDescription, so
# Plug must be able to dlopen the registering library. A "resource" plugin is
# treated as already-loaded and never dlopen'd, so the build-tree copy is
# emitted as a "library" plugin carrying a LibraryPath and the
# implementsComputeExtent flag (mirrors usdRig, CMakeLists ~410-458). The
# checked-in copy under plugin/ stays data-only; generation happens in the
# "Build-tree plugin resources" section above so one file serves both the build
# tree and the install tree.
install(FILES
    "${_usdgen_schema_pluginfo}"
    "${_usdgen_schema_resources}/generatedSchema.usda"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenSchema/resources")

set(_usdgen_imaging_pluginfo
    "${_usdgen_imaging_resources}/plugInfo.json")
install(FILES "${_usdgen_imaging_pluginfo}"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenImaging/resources")

# usdGenShaders plugin resources (10 §3.4 install tree; C5 contract).
# shaderDefs.usda lives inside shaders/ and the .glslfx files are its siblings,
# so the relative @./...@ citations resolve in the installed layout too.
install(FILES
    "${_usdgen_shader_pluginfo}"
    "${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/resources/hairLook.usda"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenShaders/resources")
# N-6: a trailing-slash source copies the *contents* of shaders/, so the
# destination must be .../resources/shaders — the old rule flattened
# shaderDefs.usda and the .glslfx files straight into resources/, where
# plugInfo's "ShaderResources": "shaders" never looks (Sdr discovery found
# zero shaders in the install tree).
install(DIRECTORY usdGenShaders/resources/shaders/
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenShaders/resources/shaders")

# usdGenTools usdview plugin: plugInfo beside the other plugin resources,
# the package where the install tree's python path already points.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTools/resources/plugInfo.json"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenTools/resources")
install(DIRECTORY plugin/usdGenTools/python/usdGenTools
    DESTINATION "${USDGEN_INSTALL_PYTHONDIR}"
    FILES_MATCHING PATTERN "*.py")
install(DIRECTORY plugin/usdGenTools/resources/expressions
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenTools/resources"
    FILES_MATCHING PATTERN "*.se")

# usdGenTonic scene index + usdGenTonicTools shelf (plan/17 P0).
install(FILES "${_usdgen_tonic_pluginfo}"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenTonic/resources")
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenTonicTools/resources/plugInfo.json"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenTonicTools/resources")
install(DIRECTORY plugin/usdGenTonicTools/python/usdGenTonicTools
    DESTINATION "${USDGEN_INSTALL_PYTHONDIR}"
    FILES_MATCHING PATTERN "*.py")
install(DIRECTORY plugin/usdGenTonicTools/resources/icons
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenTonicTools/resources")

# Export targets so the package config's include(usdGenTargets.cmake) resolves.
install(EXPORT usdGenTargets
    FILE usdGenTargets.cmake
    NAMESPACE usdGen::
    DESTINATION "${USDGEN_INSTALL_CMAKEDIR}")

# CMake package config
# sol S-5/X-4: PATH_VARS makes @PACKAGE_USDGEN_INSTALL_PLUGINDIR@ and
# @PACKAGE_USDGEN_INSTALL_LIBDIR@ expand to real installed paths in the
# generated usdGenConfig.cmake (previously set_and_check(""), which made
# the package unusable for consumers).
configure_package_config_file(
    cmake/usdGenConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/usdGenConfig.cmake"
    INSTALL_DESTINATION "${USDGEN_INSTALL_CMAKEDIR}"
    PATH_VARS USDGEN_INSTALL_PLUGINDIR USDGEN_INSTALL_LIBDIR)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/usdGenConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/usdGenConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/usdGenConfigVersion.cmake"
    DESTINATION "${USDGEN_INSTALL_CMAKEDIR}")

# Install public headers for the exported targets' $<INSTALL_INTERFACE:include>
# dirs. The build-tree include roots are:
#   usdGen        libs/usdGen          -> #include "usdGen/..."
#   usdGenSchema  libs/usdGenSchema    -> #include "usdGenSchema/..."
#   usdGenImaging libs/usdGenImaging   -> #include "usdGenImaging/..."
#   usdGenTonic   libs/usdGenTonic     -> #include "usdGenTonic/..."
# so the install mirrors the include spellings directly under
# <prefix>/include. Trailing source slashes + FILES_MATCHING ship headers
# only (sol S-6: the previous install(DIRECTORY) rules appended directory
# basenames a second time — include/usdGen/usdGen/*.h — and installed .cpp
# files; duplicate rules for schema/imaging are removed).
# N-7 (plan §1.2): usdGenMath is not an installed target, so no math headers
# are installed either — they stay in-tree until the public math headers
# land in M7; the old include/usdGenMath rule is gone.
install(DIRECTORY libs/usdGen/usdGen/
    DESTINATION include/usdGen
    FILES_MATCHING PATTERN "*.h"
    # The Session routing bridge deliberately carries cooker/private COW
    # mechanics and is not part of the installed adapter surface.
    PATTERN "sessionBackendExecutor.h" EXCLUDE)
install(DIRECTORY libs/usdGenSchema/usdGenSchema/
    DESTINATION include/usdGenSchema
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY libs/usdGenImaging/usdGenImaging/
    DESTINATION include/usdGenImaging
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY libs/usdGenTonic/usdGenTonic/
    DESTINATION include/usdGenTonic
    FILES_MATCHING PATTERN "*.h")

# NOTICE
install(FILES NOTICE DESTINATION "share/usdGen")
