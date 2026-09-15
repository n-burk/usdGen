cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)

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
    find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
endif()

# ZLIB + Threads are needed by usdGen_ptex (M4) but we find them now so the
# cache is complete.
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Compile hardening
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W4>)
else()
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wno-cpp>")
endif()

# $ORIGIN is ELF-only.  Use the native loader spelling where applicable and
# leave Windows without an RPATH.
if(UNIX AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
elseif(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
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
target_include_directories(nanoflann INTERFACE
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
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# GPU parity: Noise.cpp must not contract; the CUDA kernel pins its bit patterns with --fmad=false (mirrors usdGenMath).
# N-7 (plan §1.2): SeExpr2 headers are never installed — usdGen_seexpr only
# exists to compile Noise.cpp into the hidden archives, is never installed
# or exported, so no header rule is emitted (supersedes the sol S-6
# double-nesting fix; the INSTALL_INTERFACE root above is unused by design).

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
target_link_libraries(usdGenExprFrontend PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(usdGenExprFrontend PROPERTIES
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
endif()

# ---------------------------------------------------------------------------
# usdGen (SHARED — the engine core, no usd/usdImaging/hd linkage)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE USDGEN_CORE_SOURCES libs/usdGen/*.cpp)
# Native Vulkan owners are opt-in and must not leak SDK headers/runtime
# dependencies into CUDA-only or CPU-only core builds.
list(FILTER USDGEN_CORE_SOURCES EXCLUDE REGEX "/usdGen/vulkan/.*\\.cpp$")
# D4 (`13-codebase-alignment.md` §5): exprEval.cpp is M4 expression-lane WIP
# whose SeExpr2 engine headers are not vendored yet (see its BUILD NOTE) —
# the glob above would otherwise break the whole lib build. Excluded until
# the engine lands; its only consumer is the likewise-unwired
# tests/testUsdGenExpr.cpp. Revert this exclusion when wiring M4.
list(REMOVE_ITEM USDGEN_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/exprEval.cpp"
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
# usdGen is a SHARED plugin-host library: its public API (UsdGenSession,
# graph/tile types, ...) is consumed across the shared boundary by
# libusdGenImaging.so, so it must keep default visibility. Third-party code
# stays hidden: usdGenMath and usdGen_seexpr are compiled with
# CXX_VISIBILITY_PRESET hidden, and --exclude-libs,ALL keeps their symbols
# out of libusdGen.so's export table (gate B-1 NoThirdPartyExports).
set_target_properties(usdGen PROPERTIES
    VISIBILITY_INLINES_HIDDEN ON)
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
    CXX_VISIBILITY_PRESET default)  # plugins need exported symbols

# M1: the usdGenShaders SdrDiscoveryPlugin compiles into usdGenImaging; its
# resource dir is resolved at runtime by name (PlugRegistry lookup of
# "usdGenShaders"), so no per-target macro is needed (10-build-dependencies-
# testing.md §4.4 alternative).

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
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --fmad=false")
    # Configure-time arch probe: the GB10 dev host is sm_121; try sm_121
    # first, fall back to sm_120. A trivial kernel is compiled directly
    # with nvcc per arch (-gencode); first success wins.
    file(WRITE "${CMAKE_BINARY_DIR}/usdgen_cuda_arch_probe.cu"
        "__global__ void usdgen_cuda_arch_probe() {}\n")
    set(USDGEN_CUDA_ARCH "")
    foreach(_arch 121 120)
        execute_process(
            COMMAND "${CMAKE_CUDA_COMPILER}" -c
                    "-gencode=arch=compute_${_arch},code=sm_${_arch}"
                    "-o" "${CMAKE_BINARY_DIR}/usdgen_cuda_arch_probe.o"
                    "${CMAKE_BINARY_DIR}/usdgen_cuda_arch_probe.cu"
            RESULT_VARIABLE _usdgen_cuda_arch_rc
            OUTPUT_QUIET ERROR_QUIET)
        if(_usdgen_cuda_arch_rc EQUAL 0)
            set(USDGEN_CUDA_ARCH "${_arch}")
            break()
        endif()
    endforeach()
    if(NOT USDGEN_CUDA_ARCH)
        message(FATAL_ERROR "USE_GPU_NOISE=ON: no CUDA architecture in "
            "(sm_121, sm_120) compiles with ${CMAKE_CUDA_COMPILER}")
    endif()
    set(CMAKE_CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}" CACHE STRING
        "CUDA arch for the noise kernel (probed at configure time)" FORCE)
    # usdGen (line 117) predates the variable, so its empty default was
    # frozen at creation: pin the property explicitly.
    set_target_properties(usdGen PROPERTIES CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}")
    message(STATUS "usdGen GPU noise: CUDA ${CUDAToolkit_VERSION}, arch sm_${USDGEN_CUDA_ARCH}")
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
        target_link_libraries(usdGenGpu PUBLIC CUDA::cudart CUDA::cusolver usdGenExecutionResources)
        target_compile_definitions(usdGen PRIVATE USDGEN_ENABLE_CUDA)
        target_link_libraries(usdGen PRIVATE usdGenGpu)
        target_compile_definitions(usdGenImaging PUBLIC USDGEN_HAS_CUDA_GL_INTEROP)
        target_link_libraries(usdGenImaging PUBLIC hdSt PRIVATE usdGenGpu hgiGL)
        set_target_properties(usdGenGpu PROPERTIES
            POSITION_INDEPENDENT_CODE ON CUDA_ARCHITECTURES "${USDGEN_CUDA_ARCH}")
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

option(USDGEN_ENABLE_VULKAN_RUNTIME "Build the explicitly injected Vulkan runtime (no global backend registration)" OFF)
option(USDGEN_BUILD_VULKAN_TESTS "Build native Vulkan compute and injected-provider tests" OFF)
if(USDGEN_ENABLE_VULKAN_RUNTIME OR (USDGEN_BUILD_TESTS AND USDGEN_BUILD_VULKAN_TESTS))
    find_package(Vulkan 1.2 REQUIRED)
    # Native ownership foundation only; no Session backend registration yet.
    add_library(usdGenVulkanNative STATIC
        libs/usdGen/usdGen/vulkan/deviceContext.cpp
        libs/usdGen/usdGen/vulkan/chargedBuffer.cpp
        libs/usdGen/usdGen/vulkan/completionService.cpp
        libs/usdGen/usdGen/vulkan/sourceGeneration.cpp
        libs/usdGen/usdGen/vulkan/deviceGenerationAdapter.cpp
        libs/usdGen/usdGen/vulkan/publicationJob.cpp
        libs/usdGen/usdGen/vulkan/executionPlan.cpp
        libs/usdGen/usdGen/vulkan/planExecutor.cpp
        libs/usdGen/usdGen/vulkan/sessionProvider.cpp
        libs/usdGen/usdGen/vulkan/sourceWidthJob.cpp
        libs/usdGen/usdGen/vulkan/lengthScalePipeline.cpp
        libs/usdGen/usdGen/vulkan/lengthCompactionPipeline.cpp
        libs/usdGen/usdGen/vulkan/widthBlendPipeline.cpp
        libs/usdGen/usdGen/vulkan/nonWidthComparePipeline.cpp
        libs/usdGen/usdGen/vulkan/widthPipeline.cpp)
    target_link_libraries(usdGenVulkanNative PUBLIC Vulkan::Vulkan usdGenExecutionResources usdGen Threads::Threads)
    target_include_directories(usdGenVulkanNative PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen>"
        "$<INSTALL_INTERFACE:include>")
    set_target_properties(usdGenVulkanNative PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Canonical UUID registry must live in one DSO, never one static copy per consumer.
    add_library(usdGenVulkanFactory SHARED libs/usdGen/usdGen/vulkan/deviceFactory.cpp)
    target_link_libraries(usdGenVulkanFactory PUBLIC usdGenVulkanNative usdGen)
    set_target_properties(usdGenVulkanFactory PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    find_program(USDGEN_GLSLANG_EXECUTABLE NAMES glslangValidator glslang
        HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)
    find_program(USDGEN_SPIRV_VAL_EXECUTABLE NAMES spirv-val
        HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)
    set(_usdgen_vulkan_width_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/width.comp")
    set(_usdgen_vulkan_width_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/width.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_width_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_width_shader}" -o "${_usdgen_vulkan_width_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_width_spv}"
        DEPENDS "${_usdgen_vulkan_width_shader}" VERBATIM)
    set(_usdgen_vulkan_width_profile_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/widthProfile.comp")
    set(_usdgen_vulkan_width_profile_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/widthProfile.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_width_profile_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_width_profile_shader}" -o "${_usdgen_vulkan_width_profile_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_width_profile_spv}"
        DEPENDS "${_usdgen_vulkan_width_profile_shader}" VERBATIM)
    set(_usdgen_vulkan_length_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthScale.comp")
    set(_usdgen_vulkan_length_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthScale.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_length_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_length_shader}" -o "${_usdgen_vulkan_length_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_length_spv}"
        DEPENDS "${_usdgen_vulkan_length_shader}" VERBATIM)
    set(_usdgen_vulkan_blend_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/widthBlend.comp")
    set(_usdgen_vulkan_compaction_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthCompaction.comp")
    set(_usdgen_vulkan_compaction_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthCompaction.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_compaction_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_compaction_shader}" -o "${_usdgen_vulkan_compaction_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_compaction_spv}"
        DEPENDS "${_usdgen_vulkan_compaction_shader}" VERBATIM)
    set(_usdgen_vulkan_blend_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/widthBlend.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_blend_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_blend_shader}" -o "${_usdgen_vulkan_blend_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_blend_spv}"
        DEPENDS "${_usdgen_vulkan_blend_shader}" VERBATIM)
    set(_usdgen_vulkan_compare_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/nonWidthCompare.comp")
    set(_usdgen_vulkan_compare_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/nonWidthCompare.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_compare_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_compare_shader}" -o "${_usdgen_vulkan_compare_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_compare_spv}"
        DEPENDS "${_usdgen_vulkan_compare_shader}" VERBATIM)
    set(_usdgen_vulkan_set_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthSet.comp")
    set(_usdgen_vulkan_set_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthSet.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_set_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_set_shader}" -o "${_usdgen_vulkan_set_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_set_spv}"
        DEPENDS "${_usdgen_vulkan_set_shader}" VERBATIM)
    set(_usdgen_vulkan_cut_extend_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthCutExtend.comp")
    set(_usdgen_vulkan_cut_extend_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthCutExtend.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_cut_extend_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_cut_extend_shader}" -o "${_usdgen_vulkan_cut_extend_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_cut_extend_spv}"
        DEPENDS "${_usdgen_vulkan_cut_extend_shader}" VERBATIM)
    set(_usdgen_vulkan_reparam_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthReparam.comp")
    set(_usdgen_vulkan_reparam_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthReparam.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_reparam_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_reparam_shader}" -o "${_usdgen_vulkan_reparam_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_reparam_spv}"
        DEPENDS "${_usdgen_vulkan_reparam_shader}" VERBATIM)
    set(_usdgen_vulkan_minimum_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthMinimum.comp")
    set(_usdgen_vulkan_minimum_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthMinimum.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_minimum_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_minimum_shader}" -o "${_usdgen_vulkan_minimum_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_minimum_spv}"
        DEPENDS "${_usdgen_vulkan_minimum_shader}" VERBATIM)
    set(_usdgen_vulkan_literal_v1_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthLiteralV1.comp")
    set(_usdgen_vulkan_literal_v1_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthLiteralV1.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_literal_v1_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_literal_v1_shader}" -o "${_usdgen_vulkan_literal_v1_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_literal_v1_spv}"
        DEPENDS "${_usdgen_vulkan_literal_v1_shader}" VERBATIM)
    set(_usdgen_vulkan_envelope_v1_shader "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/lengthEnvelopeV1.comp")
    set(_usdgen_vulkan_envelope_v1_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/lengthEnvelopeV1.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_envelope_v1_spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/vulkan"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${_usdgen_vulkan_envelope_v1_shader}" -o "${_usdgen_vulkan_envelope_v1_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_envelope_v1_spv}"
        DEPENDS "${_usdgen_vulkan_envelope_v1_shader}"
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/usdGen/usdGen/vulkan/shaders/float32Envelope.glsl" VERBATIM)
    add_custom_target(usdGenVulkanShaders ALL DEPENDS "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_width_profile_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_compare_spv}" "${_usdgen_vulkan_compaction_spv}" "${_usdgen_vulkan_set_spv}" "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}" "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}" "${_usdgen_vulkan_envelope_v1_spv}")
endif()

if(USDGEN_BUILD_TESTS)
enable_testing()
if(USDGEN_BUILD_VULKAN_TESTS)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(testUsdGenVulkanFenceFd tests/testUsdGenVulkanFenceFd.cpp)
        target_link_libraries(testUsdGenVulkanFenceFd PRIVATE Vulkan::Vulkan)
        add_test(NAME testUsdGenVulkanFenceFd COMMAND testUsdGenVulkanFenceFd)
        set_tests_properties(testUsdGenVulkanFenceFd PROPERTIES
            LABELS "T1;vulkan;notification" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
            SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
            FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
        add_executable(testUsdGenVulkanSemaphoreFd tests/testUsdGenVulkanSemaphoreFd.cpp)
        target_link_libraries(testUsdGenVulkanSemaphoreFd PRIVATE Vulkan::Vulkan)
        add_test(NAME testUsdGenVulkanSemaphoreFd COMMAND testUsdGenVulkanSemaphoreFd)
        set_tests_properties(testUsdGenVulkanSemaphoreFd PROPERTIES
            LABELS "T1;vulkan;notification" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
            SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
            FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    endif()
    add_executable(testUsdGenVulkanSource tests/testUsdGenVulkanSource.cpp)
    add_executable(testUsdGenVulkanSourcePreparation tests/testUsdGenVulkanSourcePreparation.cpp)
    add_executable(testUsdGenVulkanExecutionPlan tests/testUsdGenVulkanExecutionPlan.cpp)
    target_link_libraries(testUsdGenVulkanExecutionPlan PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanExecutionPlan COMMAND testUsdGenVulkanExecutionPlan)
    set_tests_properties(testUsdGenVulkanExecutionPlan PROPERTIES LABELS "T1;vulkan;planning;metadata" TIMEOUT 60)
    add_executable(testUsdGenVulkanSourceCapture tests/testUsdGenVulkanSourceCapture.cpp)
    target_link_libraries(testUsdGenVulkanSourceCapture PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSourceCapture COMMAND testUsdGenVulkanSourceCapture)
    set_tests_properties(testUsdGenVulkanSourceCapture PROPERTIES
        LABELS "T1;vulkan;sources;root-bindings;cow" TIMEOUT 60)
    target_link_libraries(testUsdGenVulkanSourcePreparation PRIVATE usdGenVulkanNative usdGen)
    add_test(NAME testUsdGenVulkanSourcePreparation COMMAND testUsdGenVulkanSourcePreparation)
    set_tests_properties(testUsdGenVulkanSourcePreparation PROPERTIES
        LABELS "T1;vulkan;sources;cow" TIMEOUT 60)
    add_executable(testUsdGenVulkanCompletionService tests/testUsdGenVulkanCompletionService.cpp)
    target_link_libraries(testUsdGenVulkanCompletionService PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanCompletionService COMMAND testUsdGenVulkanCompletionService)
    add_test(NAME testUsdGenVulkanCompletionServiceClosePending COMMAND testUsdGenVulkanCompletionService --close-pending)
    add_test(NAME testUsdGenVulkanCompletionServiceOwnerUnavailable COMMAND testUsdGenVulkanCompletionService --owner-unavailable)
    add_test(NAME testUsdGenVulkanCompletionServiceIdleClose COMMAND testUsdGenVulkanCompletionService --idle-close)
    set_tests_properties(testUsdGenVulkanCompletionService testUsdGenVulkanCompletionServiceClosePending testUsdGenVulkanCompletionServiceOwnerUnavailable testUsdGenVulkanCompletionServiceIdleClose PROPERTIES
        LABELS "T1;vulkan;notification;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanTimelineNotification tests/testUsdGenVulkanTimelineNotification.cpp)
    target_link_libraries(testUsdGenVulkanTimelineNotification PRIVATE Vulkan::Vulkan Threads::Threads)
    add_test(NAME testUsdGenVulkanTimelineNotification COMMAND testUsdGenVulkanTimelineNotification)
    set_tests_properties(testUsdGenVulkanTimelineNotification PROPERTIES
        LABELS "T1;vulkan;notification" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanSourceStride tests/testUsdGenVulkanSourceStride.cpp)
    target_link_libraries(testUsdGenVulkanSourceStride PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSourceStride COMMAND testUsdGenVulkanSourceStride)
    set_tests_properties(testUsdGenVulkanSourceStride PROPERTIES
        LABELS "T1;vulkan;sources" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanSource PRIVATE usdGenVulkanNative)
    # Exact-value oracles must match unfused GPU evaluation order.
    target_compile_options(testUsdGenVulkanSource PRIVATE -ffp-contract=off)
    add_test(NAME testUsdGenVulkanSource COMMAND testUsdGenVulkanSource)
    add_test(NAME testUsdGenVulkanSourceQuarantine COMMAND testUsdGenVulkanSource --quarantine)
    set_tests_properties(testUsdGenVulkanSource testUsdGenVulkanSourceQuarantine PROPERTIES
        LABELS "T1;vulkan;cow;sources;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenVulkanFactory tests/testUsdGenVulkanFactory.cpp)
    target_link_libraries(testUsdGenVulkanFactory PRIVATE usdGenVulkanFactory)
    add_test(NAME testUsdGenVulkanFactory COMMAND testUsdGenVulkanFactory)
    set_tests_properties(testUsdGenVulkanFactory PROPERTIES
        LABELS "T1;vulkan;resources;identity" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenVulkanLifecycle tests/testUsdGenVulkanLifecycle.cpp)
    target_link_libraries(testUsdGenVulkanLifecycle PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLifecycle COMMAND testUsdGenVulkanLifecycle)
    add_test(NAME testUsdGenVulkanLifecycleQuarantine COMMAND testUsdGenVulkanLifecycle --quarantine)
    set_tests_properties(testUsdGenVulkanLifecycle testUsdGenVulkanLifecycleQuarantine PROPERTIES
        LABELS "T1;vulkan;resources;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenVulkanResources tests/testUsdGenVulkanResources.cpp)
    target_link_libraries(testUsdGenVulkanResources PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanResources COMMAND testUsdGenVulkanResources)
    add_test(NAME testUsdGenVulkanResourcesQuarantine COMMAND testUsdGenVulkanResources --quarantine)
    set_tests_properties(testUsdGenVulkanResources testUsdGenVulkanResourcesQuarantine PROPERTIES
        LABELS "T1;vulkan;cow;resources;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenVulkanWidth tests/testUsdGenVulkanWidth.cpp "${_usdgen_vulkan_width_spv}")
    add_executable(testUsdGenVulkanNonWidthCompare tests/testUsdGenVulkanNonWidthCompare.cpp "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_compare_spv}" "${_usdgen_vulkan_blend_spv}")
    target_link_libraries(testUsdGenVulkanNonWidthCompare PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanNonWidthCompare COMMAND testUsdGenVulkanNonWidthCompare "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_compare_spv}" "${_usdgen_vulkan_blend_spv}")
    set_tests_properties(testUsdGenVulkanNonWidthCompare PROPERTIES
        LABELS "T1;vulkan;cow;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanRootFrameCow tests/testUsdGenVulkanRootFrameCow.cpp
        "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_length_spv}"
        "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_compare_spv}")
    add_executable(testUsdGenVulkanLengthCompaction tests/testUsdGenVulkanLengthCompaction.cpp
        "${_usdgen_vulkan_compaction_spv}")
    add_executable(testUsdGenVulkanCullAdmission tests/testUsdGenVulkanCullAdmission.cpp
        "${_usdgen_vulkan_compaction_spv}")
    add_executable(testUsdGenVulkanLengthSet tests/testUsdGenVulkanLengthSet.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}")
    target_link_libraries(testUsdGenVulkanLengthSet PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthSet COMMAND testUsdGenVulkanLengthSet
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}")
    set_tests_properties(testUsdGenVulkanLengthSet PROPERTIES
        LABELS "T1;vulkan;cow;length-set;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanLengthCutExtend tests/testUsdGenVulkanLengthCutExtend.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}" "${_usdgen_vulkan_cut_extend_spv}")
    target_link_libraries(testUsdGenVulkanLengthCutExtend PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthCutExtend COMMAND testUsdGenVulkanLengthCutExtend
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}" "${_usdgen_vulkan_cut_extend_spv}")
    set_tests_properties(testUsdGenVulkanLengthCutExtend PROPERTIES
        LABELS "T1;vulkan;cow;length-cut-extend;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanLengthReparam tests/testUsdGenVulkanLengthReparam.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}")
    target_link_libraries(testUsdGenVulkanLengthReparam PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthReparam COMMAND testUsdGenVulkanLengthReparam
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}")
    set_tests_properties(testUsdGenVulkanLengthReparam PROPERTIES
        LABELS "T1;vulkan;cow;length-reparam;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanLengthMinimum tests/testUsdGenVulkanLengthMinimum.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}")
    target_link_libraries(testUsdGenVulkanLengthMinimum PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthMinimum COMMAND testUsdGenVulkanLengthMinimum
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}")
    set_tests_properties(testUsdGenVulkanLengthMinimum PROPERTIES
        LABELS "T1;vulkan;cow;length-minimum;operators;retirement;resources" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanLengthLiteralV1 tests/testUsdGenVulkanLengthLiteralV1.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}")
    target_link_libraries(testUsdGenVulkanLengthLiteralV1 PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthLiteralV1 COMMAND testUsdGenVulkanLengthLiteralV1
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}")
    set_tests_properties(testUsdGenVulkanLengthLiteralV1 PROPERTIES
        LABELS "T1;vulkan;cow;length-random;operators;retirement;resources" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanLengthEnvelopeV1 tests/testUsdGenVulkanLengthEnvelopeV1.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}" "${_usdgen_vulkan_envelope_v1_spv}")
    target_link_libraries(testUsdGenVulkanLengthEnvelopeV1 PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthEnvelopeV1 COMMAND testUsdGenVulkanLengthEnvelopeV1
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}" "${_usdgen_vulkan_envelope_v1_spv}")
    set_tests_properties(testUsdGenVulkanLengthEnvelopeV1 PROPERTIES
        LABELS "T1;vulkan;cow;length-envelope;operators;retirement;resources" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set(_usdgen_vulkan_arithmetic_spv "${CMAKE_CURRENT_BINARY_DIR}/vulkan/envelopeArithmetic.spv")
    add_custom_command(OUTPUT "${_usdgen_vulkan_arithmetic_spv}"
        COMMAND "${USDGEN_GLSLANG_EXECUTABLE}" -V --target-env vulkan1.2
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/envelopeArithmetic.comp"
            -o "${_usdgen_vulkan_arithmetic_spv}"
        COMMAND "${USDGEN_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${_usdgen_vulkan_arithmetic_spv}"
        DEPENDS tests/shaders/envelopeArithmetic.comp
            libs/usdGen/usdGen/vulkan/shaders/float32Envelope.glsl VERBATIM)
    add_executable(testUsdGenVulkanEnvelopeArithmetic tests/testUsdGenVulkanEnvelopeArithmetic.cpp
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}"
        "${_usdgen_vulkan_envelope_v1_spv}" "${_usdgen_vulkan_arithmetic_spv}")
    target_link_libraries(testUsdGenVulkanEnvelopeArithmetic PRIVATE usdGenVulkanNative)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(testUsdGenVulkanEnvelopeArithmetic PRIVATE -fno-fast-math -ffp-contract=off)
    endif()
    add_test(NAME testUsdGenVulkanEnvelopeArithmetic COMMAND testUsdGenVulkanEnvelopeArithmetic
        "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_set_spv}"
        "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}"
        "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}"
        "${_usdgen_vulkan_envelope_v1_spv}" "${_usdgen_vulkan_arithmetic_spv}")
    set_tests_properties(testUsdGenVulkanEnvelopeArithmetic PROPERTIES
        LABELS "T1;vulkan;length-envelope;numeric" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanCullAdmission PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanCullAdmission COMMAND testUsdGenVulkanCullAdmission
        "${_usdgen_vulkan_compaction_spv}")
    set_tests_properties(testUsdGenVulkanCullAdmission PROPERTIES
        LABELS "T1;vulkan;cow;topology;resources;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanLengthCompaction PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanLengthCompaction COMMAND testUsdGenVulkanLengthCompaction
        "${_usdgen_vulkan_compaction_spv}")
    set_tests_properties(testUsdGenVulkanLengthCompaction PROPERTIES
        LABELS "T1;vulkan;cow;topology;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanRootFrameCow PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanRootFrameCow COMMAND testUsdGenVulkanRootFrameCow
        "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_length_spv}"
        "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanRootFrameCow PROPERTIES
        LABELS "T1;vulkan;cow;root-frames;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_sources(testUsdGenVulkanSource PRIVATE "${_usdgen_vulkan_width_spv}")
    target_sources(testUsdGenVulkanSource PRIVATE "${_usdgen_vulkan_width_profile_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}")
    add_test(NAME testUsdGenVulkanSourceWidthCow COMMAND testUsdGenVulkanSource --cow "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanSourceWidthProfile COMMAND testUsdGenVulkanSource --profile "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_width_profile_spv}")
    add_test(NAME testUsdGenVulkanSourceLengthCow COMMAND testUsdGenVulkanSource "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}")
    add_test(NAME testUsdGenVulkanSourceWidthBlendCow COMMAND testUsdGenVulkanSource --blend "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_blend_spv}")
    set_tests_properties(testUsdGenVulkanSourceWidthProfile testUsdGenVulkanSourceLengthCow testUsdGenVulkanSourceWidthBlendCow PROPERTIES
        LABELS "T1;vulkan;cow;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanSourceAccounting tests/testUsdGenVulkanSourceAccounting.cpp "${_usdgen_vulkan_width_spv}")
    add_executable(testUsdGenVulkanDeviceGenerationAdapter tests/testUsdGenVulkanDeviceGenerationAdapter.cpp "${_usdgen_vulkan_width_spv}")
    target_link_libraries(testUsdGenVulkanDeviceGenerationAdapter PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanDeviceGenerationAdapter COMMAND testUsdGenVulkanDeviceGenerationAdapter "${_usdgen_vulkan_width_spv}")
    set_tests_properties(testUsdGenVulkanDeviceGenerationAdapter PROPERTIES
        LABELS "T1;vulkan;cow;generation;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanSourceWidthJob tests/testUsdGenVulkanSourceWidthJob.cpp "${_usdgen_vulkan_width_spv}")
    add_executable(testUsdGenVulkanSessionProvider tests/testUsdGenVulkanSessionProvider.cpp "${_usdgen_vulkan_width_spv}")
    add_executable(testUsdGenVulkanSession tests/testUsdGenVulkanSession.cpp "${_usdgen_vulkan_width_spv}")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_width_profile_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}")
    add_executable(testUsdGenVulkanSessionCoalescing tests/testUsdGenVulkanSessionCoalescing.cpp "${_usdgen_vulkan_width_spv}")
    add_executable(testUsdGenVulkanSessionContextLoss tests/testUsdGenVulkanSessionContextLoss.cpp "${_usdgen_vulkan_width_spv}")
    target_link_libraries(testUsdGenVulkanSessionContextLoss PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSessionContextLoss COMMAND testUsdGenVulkanSessionContextLoss "${_usdgen_vulkan_width_spv}")
    set_tests_properties(testUsdGenVulkanSessionContextLoss PROPERTIES
        LABELS "T1;vulkan;session;context-loss;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanSessionCoalescing PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSessionCoalescing COMMAND testUsdGenVulkanSessionCoalescing "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanSessionFollowerCancel COMMAND testUsdGenVulkanSessionCoalescing "${_usdgen_vulkan_width_spv}" --cancel-follower)
    set_tests_properties(testUsdGenVulkanSessionCoalescing testUsdGenVulkanSessionFollowerCancel PROPERTIES
        LABELS "T1;vulkan;session;coalescing;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanSession PRIVATE usdGenVulkanNative)
    # Exact-value oracles must match unfused GPU evaluation order.
    target_compile_options(testUsdGenVulkanSession PRIVATE -ffp-contract=off)
    add_test(NAME testUsdGenVulkanSession COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanSessionLostReturn COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --lost-return)
    add_test(NAME testUsdGenVulkanSessionAdmissionRollback COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --admission-rollback)
    add_test(NAME testUsdGenVulkanSessionProfile COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --profile "${_usdgen_vulkan_width_profile_spv}")
    add_test(NAME testUsdGenVulkanSessionLength COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}")
    add_test(NAME testUsdGenVulkanSessionLengthProfile COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --profile "${_usdgen_vulkan_width_profile_spv}" --length "${_usdgen_vulkan_length_spv}")
    add_test(NAME testUsdGenVulkanSessionWidthDag COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --dag "${_usdgen_vulkan_blend_spv}")
    add_test(NAME testUsdGenVulkanSessionLengthWidthDag COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --dag "${_usdgen_vulkan_blend_spv}" --length "${_usdgen_vulkan_length_spv}" --profile "${_usdgen_vulkan_width_profile_spv}")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionCrossOriginDag COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --dag "${_usdgen_vulkan_blend_spv}" --length "${_usdgen_vulkan_length_spv}" --profile "${_usdgen_vulkan_width_profile_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionNamedChannels COMMAND testUsdGenVulkanSession "${_usdgen_vulkan_width_spv}" --dag "${_usdgen_vulkan_blend_spv}" --length "${_usdgen_vulkan_length_spv}" --profile "${_usdgen_vulkan_width_profile_spv}" --compare "${_usdgen_vulkan_compare_spv}" --named)
    set_tests_properties(testUsdGenVulkanSessionNamedChannels PROPERTIES
        LABELS "T1;vulkan;cow;operators;retirement;named" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_test(NAME testUsdGenVulkanSessionRootBindings COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --named
        --length "${_usdgen_vulkan_length_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSessionRootBindings PROPERTIES
        LABELS "T1;vulkan;session;root-bindings;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_test(NAME testUsdGenVulkanSessionAffineSource COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --named --affine
        --length "${_usdgen_vulkan_length_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_compaction_spv}")
    add_test(NAME testUsdGenVulkanSessionLengthCull COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --cull "${_usdgen_vulkan_compaction_spv}")
    add_test(NAME testUsdGenVulkanSessionLengthCullRoots COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --cull "${_usdgen_vulkan_compaction_spv}")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_set_spv}")
    add_test(NAME testUsdGenVulkanSessionLengthSet COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}" --set "${_usdgen_vulkan_set_spv}")
    add_test(NAME testUsdGenVulkanSessionLengthSetRootDag COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --named
        --length "${_usdgen_vulkan_length_spv}" --set "${_usdgen_vulkan_set_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_cut_extend_spv}")
    add_test(NAME testUsdGenVulkanSessionCutExtend COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionRootCutExtend COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_reparam_spv}")
    add_test(NAME testUsdGenVulkanSessionReparam COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --cull "${_usdgen_vulkan_compaction_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionRootReparam COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --cull "${_usdgen_vulkan_compaction_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionMixedLength COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cull "${_usdgen_vulkan_compaction_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionMixedLengthRoots COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cull "${_usdgen_vulkan_compaction_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSessionMixedLength testUsdGenVulkanSessionMixedLengthRoots PROPERTIES
        LABELS "T1;vulkan;session;length-set;topology;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSessionLengthSet testUsdGenVulkanSessionLengthSetRootDag PROPERTIES
        LABELS "T1;vulkan;session;length-set;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSessionCutExtend testUsdGenVulkanSessionRootCutExtend PROPERTIES
        LABELS "T1;vulkan;session;length-cut-extend;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSessionReparam testUsdGenVulkanSessionRootReparam PROPERTIES
        LABELS "T1;vulkan;session;length-reparam;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_minimum_spv}")
    add_test(NAME testUsdGenVulkanSessionMinimum COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --minimum "${_usdgen_vulkan_minimum_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionRootMinimum COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --minimum "${_usdgen_vulkan_minimum_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSessionMinimum testUsdGenVulkanSessionRootMinimum PROPERTIES
        LABELS "T1;vulkan;session;length-minimum;cow;topology;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_literal_v1_spv}")
    add_test(NAME testUsdGenVulkanSessionLiteralV1 COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --minimum "${_usdgen_vulkan_minimum_spv}"
        --literal-v1 "${_usdgen_vulkan_literal_v1_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionRootLiteralV1 COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --minimum "${_usdgen_vulkan_minimum_spv}"
        --literal-v1 "${_usdgen_vulkan_literal_v1_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSessionLiteralV1 testUsdGenVulkanSessionRootLiteralV1 PROPERTIES
        LABELS "T1;vulkan;session;length-random;cow;topology;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_sources(testUsdGenVulkanSession PRIVATE "${_usdgen_vulkan_envelope_v1_spv}")
    add_test(NAME testUsdGenVulkanSessionEnvelopeV1 COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --minimum "${_usdgen_vulkan_minimum_spv}"
        --literal-v1 "${_usdgen_vulkan_literal_v1_spv}"
        --envelope-v1 "${_usdgen_vulkan_envelope_v1_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionRootEnvelopeV1 COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --length "${_usdgen_vulkan_length_spv}"
        --set "${_usdgen_vulkan_set_spv}" --cut "${_usdgen_vulkan_cut_extend_spv}"
        --reparam "${_usdgen_vulkan_reparam_spv}" --minimum "${_usdgen_vulkan_minimum_spv}"
        --literal-v1 "${_usdgen_vulkan_literal_v1_spv}"
        --envelope-v1 "${_usdgen_vulkan_envelope_v1_spv}"
        --cull "${_usdgen_vulkan_compaction_spv}" --dag "${_usdgen_vulkan_blend_spv}"
        --compare "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSessionEnvelopeV1 testUsdGenVulkanSessionRootEnvelopeV1 PROPERTIES
        LABELS "T1;vulkan;session;length-envelope;cow;topology;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_test(NAME testUsdGenVulkanSessionCullDag COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --cull "${_usdgen_vulkan_compaction_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSessionCullRootDag COMMAND testUsdGenVulkanSession
        "${_usdgen_vulkan_width_spv}" --roots --cull "${_usdgen_vulkan_compaction_spv}"
        --dag "${_usdgen_vulkan_blend_spv}" --compare "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSessionCullDag testUsdGenVulkanSessionCullRootDag PROPERTIES
        LABELS "T1;vulkan;session;dag;topology;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSessionLengthCull testUsdGenVulkanSessionLengthCullRoots PROPERTIES
        LABELS "T1;vulkan;session;topology;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSessionAffineSource PROPERTIES
        LABELS "T1;vulkan;session;root-bindings;affine;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSessionCrossOriginDag PROPERTIES
        LABELS "T1;vulkan;cow;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSession testUsdGenVulkanSessionLostReturn testUsdGenVulkanSessionAdmissionRollback testUsdGenVulkanSessionProfile testUsdGenVulkanSessionLength testUsdGenVulkanSessionLengthProfile testUsdGenVulkanSessionWidthDag testUsdGenVulkanSessionLengthWidthDag PROPERTIES
        LABELS "T1;vulkan;session;generation;cow;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanSessionProvider PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSessionProvider COMMAND testUsdGenVulkanSessionProvider "${_usdgen_vulkan_width_spv}")
    set_tests_properties(testUsdGenVulkanSessionProvider PROPERTIES
        LABELS "T1;vulkan;generation;retirement;provider" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanPublicationJob tests/testUsdGenVulkanPublicationJob.cpp "${_usdgen_vulkan_width_spv}")
    add_executable(testUsdGenVulkanPlanExecutor tests/testUsdGenVulkanPlanExecutor.cpp "${_usdgen_vulkan_width_spv}")
    target_sources(testUsdGenVulkanPlanExecutor PRIVATE "${_usdgen_vulkan_width_profile_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_set_spv}" "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}" "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}")
    target_link_libraries(testUsdGenVulkanPlanExecutor PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanPlanExecutor COMMAND testUsdGenVulkanPlanExecutor "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_width_profile_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_set_spv}" "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}" "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}")
    set_tests_properties(testUsdGenVulkanPlanExecutor PROPERTIES
        LABELS "T1;vulkan;sources;operators;generation;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanPublicationJob PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanPublicationJob COMMAND testUsdGenVulkanPublicationJob "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanPublicationQueuedCancel COMMAND testUsdGenVulkanPublicationJob "${_usdgen_vulkan_width_spv}" --queued-cancel)
    add_test(NAME testUsdGenVulkanPublicationPrepareReject COMMAND testUsdGenVulkanPublicationJob "${_usdgen_vulkan_width_spv}" --prepare-reject)
    add_test(NAME testUsdGenVulkanPublicationOwnerUnavailable COMMAND testUsdGenVulkanPublicationJob "${_usdgen_vulkan_width_spv}" --owner-unavailable)
    set_tests_properties(testUsdGenVulkanPublicationJob testUsdGenVulkanPublicationQueuedCancel
        testUsdGenVulkanPublicationPrepareReject testUsdGenVulkanPublicationOwnerUnavailable PROPERTIES
        LABELS "T1;vulkan;sources;generation;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanSourcePreparationLifecycle tests/testUsdGenVulkanSourcePreparationLifecycle.cpp "${_usdgen_vulkan_width_spv}")
    target_link_libraries(testUsdGenVulkanSourcePreparationLifecycle PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSourcePreparationCancel COMMAND testUsdGenVulkanSourcePreparationLifecycle "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanSourcePreparationReject COMMAND testUsdGenVulkanSourcePreparationLifecycle "${_usdgen_vulkan_width_spv}" --reject)
    add_test(NAME testUsdGenVulkanSourcePreparationOwnerUnavailable COMMAND testUsdGenVulkanSourcePreparationLifecycle "${_usdgen_vulkan_width_spv}" --owner-unavailable)
    set_tests_properties(testUsdGenVulkanSourcePreparationCancel testUsdGenVulkanSourcePreparationReject testUsdGenVulkanSourcePreparationOwnerUnavailable PROPERTIES
        LABELS "T1;vulkan;sources;retirement;resources" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanWidthAdmission tests/testUsdGenVulkanWidthAdmission.cpp "${_usdgen_vulkan_width_spv}")
    target_link_libraries(testUsdGenVulkanWidthAdmission PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanWidthAdmission COMMAND testUsdGenVulkanWidthAdmission "${_usdgen_vulkan_width_spv}")
    set_tests_properties(testUsdGenVulkanWidthAdmission PROPERTIES
        LABELS "T1;vulkan;notification;resources;cow" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_executable(testUsdGenVulkanSourceWidthAsync tests/testUsdGenVulkanSourceWidthAsync.cpp "${_usdgen_vulkan_width_spv}")
    target_link_libraries(testUsdGenVulkanSourceWidthAsync PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSourceWidthAsync COMMAND testUsdGenVulkanSourceWidthAsync "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanSourceWidthAsyncEarly COMMAND testUsdGenVulkanSourceWidthAsync "${_usdgen_vulkan_width_spv}" --early-notifications)
    add_test(NAME testUsdGenVulkanSourceWidthPrepared COMMAND testUsdGenVulkanSourceWidthAsync "${_usdgen_vulkan_width_spv}" --prepared)
    add_test(NAME testUsdGenVulkanSourceWidthPreparedEarly COMMAND testUsdGenVulkanSourceWidthAsync "${_usdgen_vulkan_width_spv}" --prepared --early-notifications)
    set_tests_properties(testUsdGenVulkanSourceWidthAsync testUsdGenVulkanSourceWidthAsyncEarly testUsdGenVulkanSourceWidthPrepared testUsdGenVulkanSourceWidthPreparedEarly PROPERTIES
        LABELS "T1;vulkan;notification;cow;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanSourceWidthJob PRIVATE usdGenVulkanNative)
    target_sources(testUsdGenVulkanSourceWidthJob PRIVATE "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_compaction_spv}")
    add_test(NAME testUsdGenVulkanSourceWidthJob COMMAND testUsdGenVulkanSourceWidthJob "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanSourceLengthWidthJob COMMAND testUsdGenVulkanSourceWidthJob "${_usdgen_vulkan_width_spv}" --length "${_usdgen_vulkan_length_spv}")
    add_test(NAME testUsdGenVulkanSourceCullCancel COMMAND testUsdGenVulkanSourceWidthJob
        "${_usdgen_vulkan_width_spv}" --cull "${_usdgen_vulkan_compaction_spv}")
    set_tests_properties(testUsdGenVulkanSourceCullCancel PROPERTIES
        LABELS "T1;vulkan;cow;topology;cancellation;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    add_test(NAME testUsdGenVulkanSourceWidthDagCancel COMMAND testUsdGenVulkanSourceWidthJob "${_usdgen_vulkan_width_spv}" --dag "${_usdgen_vulkan_blend_spv}")
    add_test(NAME testUsdGenVulkanDisabledWidth COMMAND testUsdGenVulkanSourceWidthJob "${_usdgen_vulkan_width_spv}" --disabled)
    set_tests_properties(testUsdGenVulkanDisabledWidth PROPERTIES
        LABELS "T1;vulkan;controls;passthrough" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_sources(testUsdGenVulkanSourceWidthJob PRIVATE "${_usdgen_vulkan_compare_spv}")
    add_test(NAME testUsdGenVulkanSourceWidthCrossCancel COMMAND testUsdGenVulkanSourceWidthJob "${_usdgen_vulkan_width_spv}" --cross "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_compare_spv}")
    set_tests_properties(testUsdGenVulkanSourceWidthCrossCancel PROPERTIES
        LABELS "T1;vulkan;cow;operators;retirement" RESOURCE_LOCK usdgen_gpu
        TIMEOUT 60 SKIP_RETURN_CODE 77 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSourceWidthJob testUsdGenVulkanSourceLengthWidthJob testUsdGenVulkanSourceWidthDagCancel PROPERTIES
        LABELS "T1;vulkan;sources;cow;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    target_link_libraries(testUsdGenVulkanSourceAccounting PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanSourceAccounting COMMAND testUsdGenVulkanSourceAccounting "${_usdgen_vulkan_width_spv}")
    set_tests_properties(testUsdGenVulkanSourceAccounting PROPERTIES
        LABELS "T1;vulkan;sources;cow;resources" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77"
        FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
    set_tests_properties(testUsdGenVulkanSourceWidthCow PROPERTIES
        LABELS "T1;vulkan;cow;sources;operators" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    add_executable(testUsdGenVulkanWidthPipeline tests/testUsdGenVulkanWidthPipeline.cpp "${_usdgen_vulkan_width_spv}")
    target_link_libraries(testUsdGenVulkanWidthPipeline PRIVATE usdGenVulkanNative)
    add_test(NAME testUsdGenVulkanWidthPipeline COMMAND testUsdGenVulkanWidthPipeline "${_usdgen_vulkan_width_spv}")
    add_test(NAME testUsdGenVulkanWidthPipelineQuarantine COMMAND testUsdGenVulkanWidthPipeline "${_usdgen_vulkan_width_spv}" --quarantine)
    set_tests_properties(testUsdGenVulkanWidthPipeline testUsdGenVulkanWidthPipelineQuarantine PROPERTIES
        LABELS "T1;vulkan;cow;operators;retirement" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    target_link_libraries(testUsdGenVulkanWidth PRIVATE Vulkan::Vulkan)
    if(NOT MSVC)
        target_compile_options(testUsdGenVulkanWidth PRIVATE -ffp-contract=off)
    endif()
    add_test(NAME testUsdGenVulkanWidth COMMAND testUsdGenVulkanWidth "${_usdgen_vulkan_width_spv}")
    set_tests_properties(testUsdGenVulkanWidth PROPERTIES
        LABELS "T1;vulkan;cow;operators" TIMEOUT 60 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    # A validation diagnostic must fail CTest even if the driver returns success.
    set_tests_properties(testUsdGenVulkanFactory testUsdGenVulkanLifecycle testUsdGenVulkanLifecycleQuarantine
        testUsdGenVulkanSource testUsdGenVulkanSourceQuarantine testUsdGenVulkanSourceWidthCow
        testUsdGenVulkanWidthPipeline testUsdGenVulkanWidthPipelineQuarantine
        testUsdGenVulkanResources testUsdGenVulkanResourcesQuarantine testUsdGenVulkanWidth
        PROPERTIES FAIL_REGULAR_EXPRESSION "Validation Error;VUID-;SYNC-HAZARD")
endif()
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
set_tests_properties(testUsdGenSessionStoreOwner PROPERTIES LABELS "T1;scheduling;sessions;imaging" TIMEOUT 60)
add_test(NAME testUsdGenSessionStoreExit COMMAND testUsdGenSessionStoreOwner --pending-exit)
set_tests_properties(testUsdGenSessionStoreExit PROPERTIES LABELS "T1;scheduling;sessions;imaging" TIMEOUT 60)
add_executable(testUsdGenRegistryOwners tests/testUsdGenRegistryOwners.cpp)
target_link_libraries(testUsdGenRegistryOwners PRIVATE usdGenImaging usdGen)
add_test(NAME testUsdGenRegistryOwners COMMAND testUsdGenRegistryOwners)
set_tests_properties(testUsdGenRegistryOwners PROPERTIES LABELS "T1;scheduling;imaging" TIMEOUT 60)
add_executable(testUsdGenSceneOwner tests/testUsdGenSceneOwner.cpp)
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
    add_executable(testUsdGenCudaGlInterop tests/testUsdGenCudaGlInterop.cpp)
    target_compile_definitions(testUsdGenCudaGlInterop PRIVATE USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenCudaGlInterop PRIVATE ${PROJECT_SOURCE_DIR}/usdGenShaders/test)
    target_link_libraries(testUsdGenCudaGlInterop PRIVATE usdGenImaging usdGenGpu hdSt hgiGL garch ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaGlInterop COMMAND testUsdGenCudaGlInterop)
    set_tests_properties(testUsdGenCudaGlInterop PROPERTIES LABELS "T2;cuda;interop" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_test(NAME testUsdGenCudaGlLifecycle COMMAND testUsdGenCudaGlInterop --lifecycle)
    set_tests_properties(testUsdGenCudaGlLifecycle PROPERTIES LABELS "T2;cuda;interop;lifecycle" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_test(NAME testUsdGenCudaGlTiles COMMAND testUsdGenCudaGlInterop --tiles)
    set_tests_properties(testUsdGenCudaGlTiles PROPERTIES LABELS "T2;cuda;interop;tiles" RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 120)
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
    add_test(NAME testUsdGenCudaRbfAggregateAdmission COMMAND testUsdGenCudaRbfTransaction --aggregate-admission)
    add_test(NAME testUsdGenCudaRbfDagAdmission COMMAND testUsdGenCudaRbfTransaction --aggregate-dag-admission)
    set_tests_properties(testUsdGenCudaRbfDagAdmission PROPERTIES
        LABELS "T1;cuda;rbf;transaction;dag;cow;resources" RESOURCE_LOCK usdgen_gpu TIMEOUT 120)
    add_test(NAME testUsdGenCudaRbfScatterDagAdmission COMMAND testUsdGenCudaRbfTransaction --aggregate-scatter-dag-admission)
    set_tests_properties(testUsdGenCudaRbfScatterDagAdmission PROPERTIES
        LABELS "T1;cuda;rbf;transaction;dag;cow;resources" RESOURCE_LOCK usdgen_gpu TIMEOUT 120)
    add_test(NAME testUsdGenCudaRbfTopologyDagAdmission COMMAND testUsdGenCudaRbfTransaction --aggregate-topology-dag-admission)
    set_tests_properties(testUsdGenCudaRbfTopologyDagAdmission PROPERTIES
        LABELS "T1;cuda;rbf;transaction;dag;cow;resources" RESOURCE_LOCK usdgen_gpu TIMEOUT 120)
    add_test(NAME testUsdGenCudaRbfParamMapAdmission COMMAND testUsdGenCudaRbfTransaction --aggregate-param-map-admission)
    set_tests_properties(testUsdGenCudaRbfParamMapAdmission PROPERTIES
        LABELS "T1;cuda;rbf;transaction;dag;cow;resources" RESOURCE_LOCK usdgen_gpu TIMEOUT 120)
    add_test(NAME testUsdGenCudaBlendOverlap COMMAND testUsdGenCudaRbfTransaction --blend-overlap)
    set_tests_properties(testUsdGenCudaBlendOverlap PROPERTIES
        LABELS "T1;cuda;overlap;transaction" TIMEOUT 180 RESOURCE_LOCK usdgen_gpu)
    add_test(NAME testUsdGenCudaMultiGroomAdmission COMMAND testUsdGenCudaRbfTransaction --multi-groom-admission)
    set_tests_properties(testUsdGenCudaMultiGroomAdmission PROPERTIES
        LABELS "T1;cuda;rbf;transaction;resources" TIMEOUT 180 RESOURCE_LOCK usdgen_gpu)
    set_tests_properties(testUsdGenCudaRbfAggregateAdmission PROPERTIES
        LABELS "T1;cuda;rbf;transaction;cow;resources" RESOURCE_LOCK usdgen_gpu TIMEOUT 120)
    set_tests_properties(testUsdGenCudaRbfValueDag PROPERTIES
        LABELS "T1;cuda;rbf;transaction;dag;cow" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    set_tests_properties(testUsdGenCudaRbfTransaction PROPERTIES
        LABELS "T1;cuda;rbf;transaction" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaCompositionMatrix tests/testUsdGenCudaCompositionMatrix.cpp)
    target_compile_definitions(testUsdGenCudaCompositionMatrix PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaCompositionMatrix PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaCompositionMatrix COMMAND testUsdGenCudaCompositionMatrix)
    set_tests_properties(testUsdGenCudaCompositionMatrix PROPERTIES
        LABELS "T1;cuda;composition" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    add_executable(testUsdGenCudaControlEval tests/testUsdGenCudaControlEval.cpp)
    target_compile_definitions(testUsdGenCudaControlEval PRIVATE USDGEN_ENABLE_CUDA)
    target_link_libraries(testUsdGenCudaControlEval PRIVATE usdGen usdGenGpu)
    add_test(NAME testUsdGenCudaControlEval COMMAND testUsdGenCudaControlEval)
    set_tests_properties(testUsdGenCudaControlEval PROPERTIES
        LABELS "T1;cuda;controls" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
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
    target_compile_options(testUsdGenCudaParameters PRIVATE -UNDEBUG)
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
        target_compile_options(testUsdGenCuda${_cuda_test} PRIVATE -UNDEBUG)
        add_test(NAME testUsdGenCuda${_cuda_test} COMMAND testUsdGenCuda${_cuda_test})
        set_tests_properties(testUsdGenCuda${_cuda_test} PROPERTIES
            LABELS "T1;cuda" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
    endforeach()
    add_executable(testUsdGenCudaNonWidthCompare tests/testUsdGenCudaNonWidthCompare.cu)
    target_link_libraries(testUsdGenCudaNonWidthCompare PRIVATE usdGenGpu usdGen)
    target_compile_options(testUsdGenCudaNonWidthCompare PRIVATE -UNDEBUG)
    add_test(NAME testUsdGenCudaNonWidthCompare COMMAND testUsdGenCudaNonWidthCompare)
    set_tests_properties(testUsdGenCudaNonWidthCompare PROPERTIES
        LABELS "T1;cuda;dag;cow;topology" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    foreach(_cuda_test Rbf Deform)
        add_executable(testUsdGenCuda${_cuda_test} tests/testUsdGenCuda${_cuda_test}.cu)
        target_link_libraries(testUsdGenCuda${_cuda_test} PRIVATE usdGenGpu)
        # These CUDA oracles use assert for both execution and checks.
        # Release's -DNDEBUG must never turn them into false-green no-ops.
        target_compile_options(testUsdGenCuda${_cuda_test} PRIVATE -UNDEBUG)
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
endif()

# M0: build-rule tests (gate B-1) — see cmake/CheckNoStage*.cmake
set(USDGEN_FORBIDDEN_LIB_REGEX "^libusd_(usd|hd|hgi|glf|garch)")
set(USDGEN_FORBIDDEN_INC_REGEX
    "pxr/(usd|imaging|usdImaging)/(usd|hd|hgi|glf|garch)[A-Za-z0-9_]*\\/")

find_program(USDGEN_CXXFILT c++filt)

foreach(_t usdGen usdGenMath usdGenTestUtils)
    add_test(NAME testUsdGenLinkRule_${_t}
        COMMAND ${CMAKE_COMMAND}
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

add_test(NAME testUsdGenNoThirdPartyExports
    COMMAND ${CMAKE_COMMAND}
        -DTARGET_FILE=$<TARGET_FILE:usdGen>
        -DCXXFILT=${USDGEN_CXXFILT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoThirdPartyExports.cmake)
# sol X-1: this is a B-1 gate check and must carry the gate:B-1 label so
# `ctest -L '^gate:B-1$'` selects all five B-1 tests.
set_tests_properties(testUsdGenNoThirdPartyExports
    PROPERTIES LABELS "T0;build;gate:B-1")

# sol S-1/S-2: negative/positive fixtures proving the B-1 gates actually fail
# on forbidden input and pass on clean input (no silent false passes).
add_test(NAME testUsdGenLinkRuleNegativeFixture
    COMMAND ${CMAKE_COMMAND}
        -DTARGET_FILE=${USD_INSTALL_DIR}/lib/libusd_hd.so
        -DOBJDUMP=${CMAKE_OBJDUMP}
        -DCXXFILT=${USDGEN_CXXFILT}
        -DFORBIDDEN=^libusd_tf
        -DEXPECT=fail
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateLinkDriver.cmake)
add_test(NAME testUsdGenLinkRulePositiveFixture
    COMMAND ${CMAKE_COMMAND}
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
add_test(NAME testUsdGenStagingRule
    COMMAND ${CMAKE_COMMAND}
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
set_tests_properties(testUsdGenChainOrder PROPERTIES
    LABELS "T1;gate:SI-5"
    ENVIRONMENT "PXR_PLUGINPATH_NAME=${_usdgen_schema_resources}:${_usdgen_imaging_resources}:${_fake_prman_resources}:${USD_INSTALL_DIR}/plugin/usd:${USD_INSTALL_DIR}/lib/usd;LD_LIBRARY_PATH=${USD_INSTALL_DIR}/lib")

add_executable(testUsdGenPluginDiscovery tests/testUsdGenPluginDiscovery.cpp)
target_link_libraries(testUsdGenPluginDiscovery PRIVATE usdGenImaging usdGenTestUtilsHd)
add_test(NAME testUsdGenPluginDiscovery
    COMMAND testUsdGenPluginDiscovery
        ${_usdgen_schema_resources}
        ${_usdgen_imaging_resources}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes/scene_empty_groom.usda)
set_tests_properties(testUsdGenPluginDiscovery PROPERTIES
    LABELS "T1"
    ENVIRONMENT "PXR_PLUGINPATH_NAME=${_usdgen_schema_resources}:${_usdgen_imaging_resources}:${USD_INSTALL_DIR}/plugin/usd;LD_LIBRARY_PATH=${USD_INSTALL_DIR}/lib")

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
# M-13: join the dirs with ':' explicitly. The old literal mixed CMake's ';'
# list separator into the value, so _usdgen_m1_env ("PXR_PLUGINPATH_NAME=a;b;
# c=d") cut the plugin path after the first dir and mis-split the USD dirs.
set(_usdgen_m1_pluginpath_dirs
    "${_usdgen_schema_resources}" "${_usdgen_imaging_resources}"
    "${_usdgen_shader_resources}"
    "${USD_INSTALL_DIR}/plugin/usd" "${USD_INSTALL_DIR}/lib/usd")
string(JOIN ":" _usdgen_m1_pluginpath ${_usdgen_m1_pluginpath_dirs})
set(_usdgen_m1_env
    "PXR_PLUGINPATH_NAME=${_usdgen_m1_pluginpath};LD_LIBRARY_PATH=${USD_INSTALL_DIR}/lib:$<TARGET_FILE_DIR:usdGen>")

# M1 T0 engine tests (no Hydra, no stage — gate B-1 keeps them clean).
foreach(_t testUsdGenGraph testUsdGenKernelDeterminism testUsdGenWidth
           testUsdGenCpuFanout testUsdGenCpuReferenceMap testUsdGenCurveRagged
           testUsdGenCpuCurveSource testUsdGenLengthRaggedLayout testUsdGenSurfaceRootFrames
           testUsdGenSurfaceRootBindings testUsdGenSurfaceRootBindingBoundaries
           testUsdGenSurfaceBindingCache)
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
           testUsdGenRestAdapter testUsdGenCurveRestAdapter testUsdGenAutoApply testUsdGenContracts)
    add_executable(${_t} tests/${_t}.cpp)
    target_link_libraries(${_t} PRIVATE usdGenTestUtilsHd)
    add_test(NAME ${_t} COMMAND ${_t})
    set_tests_properties(${_t} PROPERTIES
        ENVIRONMENT "${_usdgen_m1_env}" SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
endforeach()
set_tests_properties(testUsdGenTileContract PROPERTIES LABELS "T1;gate:SI-1")
set_tests_properties(testUsdGenStockStormContract PROPERTIES
    LABELS "T1;stock-hydra;unpatched-openusd")
set_tests_properties(testUsdGenScenePublication PROPERTIES
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
    ENVIRONMENT "${_usdgen_m1_env}"
    LABELS "T1;gate:SI-12"
    TIMEOUT 300
)

add_executable(testUsdGenImageMapStage tests/testUsdGenImageMapStage.cpp)
target_link_libraries(testUsdGenImageMapStage PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenImageMapStage PRIVATE
    USDGEN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenImageMapStage COMMAND testUsdGenImageMapStage)
set_tests_properties(testUsdGenImageMapStage PROPERTIES
    ENVIRONMENT "${_usdgen_m1_env}" LABELS "T1;imaging;map")

add_executable(testUsdGenHierarchy tests/testUsdGenHierarchy.cpp)
target_link_libraries(testUsdGenHierarchy PRIVATE usdGenTestUtilsHd)
add_test(NAME testUsdGenHierarchy COMMAND testUsdGenHierarchy)
set_tests_properties(testUsdGenHierarchy PROPERTIES
    ENVIRONMENT "${_usdgen_m1_env}" LABELS "T1;hierarchy" TIMEOUT 120)

add_executable(testUsdGenExpressionTransport tests/testUsdGenExpressionTransport.cpp)
target_link_libraries(testUsdGenExpressionTransport PRIVATE usdGenTestUtilsHd)
target_compile_definitions(testUsdGenExpressionTransport PRIVATE
    USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME testUsdGenExpressionTransport COMMAND testUsdGenExpressionTransport)
set_tests_properties(testUsdGenExpressionTransport PROPERTIES
    ENVIRONMENT "${_usdgen_m1_env}" LABELS "T1;expressions" TIMEOUT 120)

add_executable(testUsdGenSessionIsolation tests/testUsdGenSessionIsolation.cpp)
target_link_libraries(testUsdGenSessionIsolation PRIVATE usdGenImaging)
add_test(NAME testUsdGenSessionIsolation COMMAND testUsdGenSessionIsolation)
set_tests_properties(testUsdGenSessionIsolation PROPERTIES
    LABELS "T1;sessions" TIMEOUT 120)

foreach(_expr_test ExpressionContext ExpressionBindings ExpressionFrontend ExpressionIr)
    add_executable(testUsdGen${_expr_test} tests/testUsdGen${_expr_test}.cpp)
    target_link_libraries(testUsdGen${_expr_test} PRIVATE usdGen)
    add_test(NAME testUsdGen${_expr_test} COMMAND testUsdGen${_expr_test})
    set_tests_properties(testUsdGen${_expr_test} PROPERTIES
        LABELS "T0;expressions" TIMEOUT 120)
endforeach()
add_test(NAME testUsdGenExpressionSchema
    COMMAND ${CMAKE_COMMAND} -E env "GENBUILD=${CMAKE_CURRENT_BINARY_DIR}"
        bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/checks/run_expression_schema.sh")
set_tests_properties(testUsdGenExpressionSchema PROPERTIES
    LABELS "T1;expressions;schema" TIMEOUT 120)

# Note: testUsdGenInvalidation already carries T1;gate:SI-2 (set at line ~446).
# The SI-2 overlay extension (bare primvars locator, section 7) is appended
# within that same test and shares its executable; no new CMake target needed.
set_tests_properties(testUsdGenScheduling PROPERTIES LABELS "T1;gate:SI-3")
set_tests_properties(testUsdGenSnapshotRace PROPERTIES LABELS "T1;gate:SI-4")
set_tests_properties(testUsdGenPopulation PROPERTIES LABELS "T1;gate:SI-6")
set_tests_properties(testUsdGenAdapter PROPERTIES LABELS "T1;gate:SI-7")
set_tests_properties(testUsdGenRestAdapter PROPERTIES LABELS "T1;gate:SI-7")
set_tests_properties(testUsdGenCurveRestAdapter PROPERTIES LABELS "T1;gate:SI-7")
if(USDGEN_ENABLE_CUDA)
    add_executable(testUsdGenCudaHierarchy tests/testUsdGenCudaHierarchy.cpp)
    target_link_libraries(testUsdGenCudaHierarchy PRIVATE usdGenTestUtilsHd usdGenGpu)
    target_compile_definitions(testUsdGenCudaHierarchy PRIVATE USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME testUsdGenCudaHierarchy COMMAND testUsdGenCudaHierarchy)
    set_tests_properties(testUsdGenCudaHierarchy PROPERTIES
        ENVIRONMENT "${_usdgen_m1_env}" LABELS "T1;cuda;hierarchy" TIMEOUT 120 RESOURCE_LOCK usdgen_gpu)
endif()
set_tests_properties(testUsdGenAutoApply PROPERTIES LABELS "T1;gate:SI-8")
set_tests_properties(testUsdGenContracts PROPERTIES LABELS "T1")

# N-5 (09 §5.2, 10 §5.6): schema regeneration-drift guard — the checked-in
# plugin/usdGenSchema/resources must stay byte-identical to a fresh
# usdGenSchema run over libs/usdGenSchema/schema.usda (bin/gen_schema.sh is
# the reference recipe; the N-2 plugInfo post-processing drift is what this
# catches in CI). The source has landed (plan/13 §1); the else() echo-stub
# branch below is retained only for missing sources (never a fake pass).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/testUsdGenSchemaUpToDate.cpp")
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
    LABELS "T1" ENVIRONMENT "${_usdgen_m1_env}" ${_usdgen_schema_uptodate_props})

# M1 T2 Storm tests (EGL harness; the S-8/S-9/L-1 PW decisions get confirmed
# on shipped code here, 11 §2.2 exit-gate table).
add_executable(testUsdGenStormLook tests/testUsdGenStormLook.cpp)
target_link_libraries(testUsdGenStormLook PRIVATE usdGenTestUtilsHd)
target_include_directories(testUsdGenStormLook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test)
add_test(NAME testUsdGenStormLook COMMAND testUsdGenStormLook ${PROJECT_SOURCE_DIR})
set_tests_properties(testUsdGenStormLook PROPERTIES
    LABELS "T2;gate:L-2" ENVIRONMENT "${_usdgen_m1_env}" SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

add_executable(benchUsdGenStorm tests/perf/benchUsdGenStorm.cpp)
target_link_libraries(benchUsdGenStorm PRIVATE usdGenTestUtilsHd)
target_include_directories(benchUsdGenStorm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test)
add_test(NAME benchUsdGenStorm
    COMMAND benchUsdGenStorm
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
    ENVIRONMENT "${_usdgen_m1_env};HD_ENABLE_PERFLOG=1" SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")

# Groom fixtures G1-G5 (S-1/S-5/S-6/S-12/E-5 scenes): pure-text output of
# usd/fixtures/make_fixtures.py (stdlib only, no pxr), so T2 stays
# self-contained wherever python3 exists. Regeneration is byte-identical
# (short decimals, fixed emission order); the files stay gitignored.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/g3_head100k.usda
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/make_fixtures.py
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/make_fixtures.py
    COMMENT "Generating usdGen groom fixtures G1-G5 (gitignored)"
    VERBATIM)
add_custom_target(usdGenFixtures ALL
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usd/fixtures/g3_head100k.usda)
add_dependencies(benchUsdGenStorm usdGenFixtures)

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
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_t}.cpp")
        add_executable(${_t} tests/${_t}.cpp)
        target_link_libraries(${_t} PRIVATE usdGenTestUtilsHd)
        target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/usdGenShaders/test)
        set(_usdgen_storm_cmd ${_t} ${PROJECT_SOURCE_DIR})
        set(_usdgen_storm_props SKIP_RETURN_CODE "77" NOT_RUN_RETURN_VALUES "77")
    else()
        set(_usdgen_storm_cmd
            ${CMAKE_COMMAND} -E echo "M1-STUB ${_t} (pending wave-3 source)")
        set(_usdgen_storm_props SKIP_REGULAR_EXPRESSION "M1-STUB")
    endif()
    add_test(NAME ${_t} COMMAND ${_usdgen_storm_cmd})
    set_tests_properties(${_t} PROPERTIES
        LABELS "${_usdgen_storm_labels_${_t}}"
        RUN_SERIAL TRUE
        ENVIRONMENT "${_usdgen_m1_env}"
        ${_usdgen_storm_props})
endforeach()


# ---------------------------------------------------------------------------
# M0 install-tree tests (plan §6.2, sol S-5/X-4)
#
# Both tests read the install prefix from the USDGEN_INSTALL_PREFIX env var.
# When unset they print a note and return 0 (clean skip). CI sets
# USDGEN_INSTALL_PREFIX to the scratch prefix after running cmake --install.
# ---------------------------------------------------------------------------
add_executable(testUsdGenInstallTree tests/testUsdGenInstallTree.cpp)
target_link_libraries(testUsdGenInstallTree PRIVATE
    usd usdGeom tf arch gf vt sdf trace)
target_compile_definitions(testUsdGenInstallTree PRIVATE
    USDGEN_TEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes/scene_empty_groom.usda"
    USDGEN_TEST_PLUGIN_DIR="${USDGEN_INSTALL_PLUGINDIR}"
    USDGEN_TEST_LIB_DIR="${USDGEN_INSTALL_LIBDIR}"
    USDGEN_TEST_CMAKE_DIR="${USDGEN_INSTALL_CMAKEDIR}"
    USDGEN_TEST_USD_INSTALL_DIR="${USD_INSTALL_DIR}")
add_test(NAME testUsdGenInstallTree COMMAND testUsdGenInstallTree)
set_tests_properties(testUsdGenInstallTree PROPERTIES
    LABELS "T0;install")

add_executable(testUsdGenConsumer tests/testUsdGenConsumer.cpp)
target_compile_definitions(testUsdGenConsumer PRIVATE
    USDGEN_TEST_CONSUMER_SRC="${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer"
    USDGEN_TEST_USD_INSTALL_DIR="${USD_INSTALL_DIR}"
    USDGEN_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}")
add_test(NAME testUsdGenConsumer COMMAND testUsdGenConsumer)
set_tests_properties(testUsdGenConsumer PROPERTIES
    LABELS "T0;install")

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
install(TARGETS usdGenExecutionResources usdGen usdGenImaging usdGenSchema usdGenShaders
    EXPORT usdGenTargets
    RUNTIME DESTINATION "${USDGEN_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${USDGEN_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${USDGEN_INSTALL_LIBDIR}")

if(USDGEN_ENABLE_VULKAN_RUNTIME)
    install(TARGETS usdGenVulkanNative usdGenVulkanFactory
        EXPORT usdGenTargets
        RUNTIME DESTINATION "${USDGEN_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${USDGEN_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${USDGEN_INSTALL_LIBDIR}")
    install(FILES "${_usdgen_vulkan_width_spv}" "${_usdgen_vulkan_width_profile_spv}" "${_usdgen_vulkan_length_spv}" "${_usdgen_vulkan_blend_spv}" "${_usdgen_vulkan_compare_spv}" "${_usdgen_vulkan_compaction_spv}" "${_usdgen_vulkan_set_spv}" "${_usdgen_vulkan_cut_extend_spv}" "${_usdgen_vulkan_reparam_spv}" "${_usdgen_vulkan_minimum_spv}" "${_usdgen_vulkan_literal_v1_spv}" "${_usdgen_vulkan_envelope_v1_spv}" DESTINATION "share/usdGen/vulkan")
endif()

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

# NOTICE
install(FILES NOTICE DESTINATION "share/usdGen")
