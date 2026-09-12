cmake_minimum_required(VERSION 3.26)
project(usdGenStormExtensionValidation LANGUAGES CXX)

# Isolated development harness: rebuild HdSt against the matching SDK, never
# overwrite that SDK. This is not a distributable replacement OpenUSD build.
set(USD_INSTALL_DIR "" CACHE PATH "Matching OpenUSD 26.08 SDK")
set(USDGEN_PATCHED_OPENUSD_SOURCE "" CACHE PATH "Private patched OpenUSD 26.08 source")
if(NOT EXISTS "${USDGEN_PATCHED_OPENUSD_SOURCE}/pxr/imaging/hdSt/resourceRegistry.cpp")
    message(FATAL_ERROR "Set USDGEN_PATCHED_OPENUSD_SOURCE to a private patched source tree")
endif()
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
file(STRINGS "${USD_INSTALL_DIR}/include/pxr/pxr.h" _version REGEX "^#define PXR_VERSION ")
if(NOT _version STREQUAL "#define PXR_VERSION 2608")
    message(FATAL_ERROR "This development harness is pinned to OpenUSD 26.08")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_hdst "${USDGEN_PATCHED_OPENUSD_SOURCE}/pxr/imaging/hdSt")
file(GLOB _sources CONFIGURE_DEPENDS "${_hdst}/*.cpp")
# Matches the reference SDK's MaterialX-on, Ptex-off, GL-on configuration.
list(REMOVE_ITEM _sources "${_hdst}/ptexMipmapTextureLoader.cpp")
add_library(usdGenPatchedStorm SHARED ${_sources})
set_target_properties(usdGenPatchedStorm PROPERTIES OUTPUT_NAME usd_hdSt)
target_include_directories(usdGenPatchedStorm BEFORE PUBLIC "${USDGEN_PATCHED_OPENUSD_SOURCE}")
target_include_directories(usdGenPatchedStorm PRIVATE "${_hdst}")
get_target_property(_dependencies hdSt INTERFACE_LINK_LIBRARIES)
target_link_libraries(usdGenPatchedStorm PUBLIC ${_dependencies})
target_compile_definitions(usdGenPatchedStorm PUBLIC PXR_MATERIALX_SUPPORT_ENABLED
    PRIVATE HDST_EXPORTS=1 MFB_PACKAGE_NAME=hdSt MFB_ALT_PACKAGE_NAME=hdSt
    MFB_PACKAGE_MODULE=HdSt PXR_BUILD_LOCATION=usd PXR_PLUGIN_BUILD_LOCATION=../plugin/usd
    PXR_GL_SUPPORT_ENABLED PXR_X11_SUPPORT_ENABLED GL_GLEXT_PROTOTYPES
    GLX_GLXEXT_PROTOTYPES BOOST_NO_CXX98_FUNCTION_BASE)
target_compile_options(usdGenPatchedStorm PRIVATE -Wno-deprecated-declarations)

enable_testing()
add_executable(testUsdGenStormCommitPublication testCommitPublication.cpp)
target_include_directories(testUsdGenStormCommitPublication PRIVATE ../../usdGenShaders/test)
target_link_libraries(testUsdGenStormCommitPublication PRIVATE usdGenPatchedStorm ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormCommitPublication COMMAND testUsdGenStormCommitPublication)
set_tests_properties(testUsdGenStormCommitPublication PROPERTIES
    LABELS "T2;storm-extension;publication" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

# Native retained-scene-index ingress smoke test.  This intentionally remains
# separate from the BAR-only publication test: it registers a real retained
# scene index with Storm and supplies the hdStBasisCurvesGpu datasource.
add_executable(testUsdGenNativeBasisCurves testUsdGenNativeBasisCurves.cpp)
target_include_directories(testUsdGenNativeBasisCurves PRIVATE
    ../../usdGenShaders/test)
target_link_libraries(testUsdGenNativeBasisCurves PRIVATE
    usdGenPatchedStorm hdx ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenNativeBasisCurves COMMAND testUsdGenNativeBasisCurves)
set_tests_properties(testUsdGenNativeBasisCurves PROPERTIES
    LABELS "T2;storm-extension;native-basis-curves" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

add_executable(testUsdGenStormGpuGroupDataSource testUsdGenStormGpuGroupDataSource.cpp)
target_link_libraries(testUsdGenStormGpuGroupDataSource PRIVATE
    usdGenPatchedStorm ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormGpuGroupDataSource COMMAND testUsdGenStormGpuGroupDataSource)
set_tests_properties(testUsdGenStormGpuGroupDataSource PROPERTIES
    LABELS "T2;storm-extension;gpu-group-datasource" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

add_executable(testUsdGenStormGpuGroupMemberDataSource
    testUsdGenStormGpuGroupMemberDataSource.cpp)
target_link_libraries(testUsdGenStormGpuGroupMemberDataSource PRIVATE
    usdGenPatchedStorm ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormGpuGroupMemberDataSource
    COMMAND testUsdGenStormGpuGroupMemberDataSource)
set_tests_properties(testUsdGenStormGpuGroupMemberDataSource PROPERTIES
    LABELS "T2;storm-extension;gpu-group-member-datasource" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

add_executable(testUsdGenStormGpuGroupController
    testUsdGenStormGpuGroupController.cpp)
target_include_directories(testUsdGenStormGpuGroupController PRIVATE
    ../../usdGenShaders/test)
target_link_libraries(testUsdGenStormGpuGroupController PRIVATE
    usdGenPatchedStorm ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormGpuGroupController
    COMMAND testUsdGenStormGpuGroupController)
set_tests_properties(testUsdGenStormGpuGroupController PROPERTIES
    LABELS "T2;storm-extension;gpu-group-controller" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

# This private plugin metadata is generated only for the ordering test.  The
# normal retained/native fixtures deliberately run without it, avoiding a
# second staging wrapper in those tests.
set(USDGEN_RESOURCE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build-codex"
    CACHE PATH "usdGen build tree providing schema/imaging test resources")
set(_usdgen_schema_resources
    "${USDGEN_RESOURCE_BUILD_DIR}/usd/usdGenSchema/resources")
set(_usdgen_imaging_resources
    "${USDGEN_RESOURCE_BUILD_DIR}/usd/usdGenImaging/resources")
set(_hdst_gpu_group_staging_resources
    "${CMAKE_CURRENT_BINARY_DIR}/usd/hdStGpuGroupStaging/resources")
file(MAKE_DIRECTORY "${_hdst_gpu_group_staging_resources}")
# Use the full private-HdSt metadata, not a second fixture plugin entry: a
# process must never discover stock and patched libusd_hdSt side by side.
# The generated entry keeps stock shader resources while binding every HdSt
# type (including the staging plugin) to usdGenPatchedStorm.
set(PLUG_INFO_LIBRARY_PATH "$<TARGET_FILE:usdGenPatchedStorm>")
set(PLUG_INFO_RESOURCE_PATH "resources")
set(PLUG_INFO_ROOT "${USD_INSTALL_DIR}/lib/usd/hdSt")
configure_file(
    "${USDGEN_PATCHED_OPENUSD_SOURCE}/pxr/imaging/hdSt/plugInfo.json"
    "${_hdst_gpu_group_staging_resources}/plugInfo.json.configure" @ONLY)
file(GENERATE
    OUTPUT "${_hdst_gpu_group_staging_resources}/plugInfo.json"
    INPUT "${_hdst_gpu_group_staging_resources}/plugInfo.json.configure")
add_executable(testUsdGenStormGpuGroupStagingPluginOrder
    testUsdGenStormGpuGroupStagingPluginOrder.cpp)
target_link_libraries(testUsdGenStormGpuGroupStagingPluginOrder PRIVATE
    usdGenPatchedStorm hdsi ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormGpuGroupStagingPluginOrder
    COMMAND testUsdGenStormGpuGroupStagingPluginOrder)
set_tests_properties(testUsdGenStormGpuGroupStagingPluginOrder PROPERTIES
    LABELS "T2;storm-extension;gpu-group-staging-plugin-order" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT "PXR_PLUGINPATH_NAME=${_usdgen_schema_resources}:${_usdgen_imaging_resources}:${_hdst_gpu_group_staging_resources};LD_LIBRARY_PATH=$<TARGET_FILE_DIR:usdGenPatchedStorm>:${USD_INSTALL_DIR}/lib")

add_executable(testUsdGenStormGpuGroupStagingSceneIndex
    testUsdGenStormGpuGroupStagingSceneIndex.cpp)
target_link_libraries(testUsdGenStormGpuGroupStagingSceneIndex PRIVATE
    usdGenPatchedStorm ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormGpuGroupStagingSceneIndex
    COMMAND testUsdGenStormGpuGroupStagingSceneIndex)
set_tests_properties(testUsdGenStormGpuGroupStagingSceneIndex PROPERTIES
    LABELS "T2;storm-extension;gpu-group-staging-scene-index" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

add_executable(testUsdGenStormGpuGroupStagingReentry
    testUsdGenStormGpuGroupStagingReentry.cpp)
target_link_libraries(testUsdGenStormGpuGroupStagingReentry PRIVATE
    usdGenPatchedStorm ${CMAKE_DL_LIBS})
add_test(NAME testUsdGenStormGpuGroupStagingReentry
    COMMAND testUsdGenStormGpuGroupStagingReentry)
set_tests_properties(testUsdGenStormGpuGroupStagingReentry PROPERTIES
    LABELS "T2;storm-extension;gpu-group-staging-reentry" SKIP_RETURN_CODE 77 TIMEOUT 60
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")

set(USDGEN_CUDA_BUILD_DIR "" CACHE PATH "Optional matching usdGen CUDA build for real transfer validation")
if(USDGEN_CUDA_BUILD_DIR)
    find_package(CUDAToolkit REQUIRED)
    foreach(_library libusdGen.so libusdGenGpu.a libusdGenSchema.so)
        if(NOT EXISTS "${USDGEN_CUDA_BUILD_DIR}/${_library}")
            message(FATAL_ERROR "Missing ${USDGEN_CUDA_BUILD_DIR}/${_library}; build usdGen with CUDA first")
        endif()
    endforeach()
    # Build the application imaging plugin against this harness's private
    # HdSt.  Linking the production usdGenImaging library here would instead
    # pull in the stock-SDK HdSt ABI and make plugin discovery ambiguous.
    file(GLOB_RECURSE _usdgen_private_imaging_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/usdGenImaging/*.cpp")
    list(FILTER _usdgen_private_imaging_sources EXCLUDE REGEX
        "usdGenGraphDescBuilderStage\\.cpp$")
    add_library(usdGenPrivateImaging SHARED ${_usdgen_private_imaging_sources})
    target_include_directories(usdGenPrivateImaging BEFORE PUBLIC
        "${USDGEN_PATCHED_OPENUSD_SOURCE}")
    target_include_directories(usdGenPrivateImaging PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/usdGenImaging"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/usdGen"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/usdGenSchema")
    target_link_libraries(usdGenPrivateImaging
        PUBLIC hd hdsi hf usd usdGeom usdShade sdr usdImaging
               sdf tf gf vt trace pxOsd
               "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so"
               usdGenPatchedStorm
        PRIVATE "${USDGEN_CUDA_BUILD_DIR}/libusdGenSchema.so"
                "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
                hgiGL CUDA::cudart CUDA::cusolver)
    target_compile_definitions(usdGenPrivateImaging PUBLIC
        USDGEN_HAS_CUDA_GL_INTEROP)
    set_target_properties(usdGenPrivateImaging PROPERTIES
        CXX_VISIBILITY_PRESET default)

    # This generated metadata is visible only to the real-Groom CUDA test.
    # Keep the public plugin name/types, but resolve its library directly to
    # the private imaging target so Plug never opens stock libusdGenImaging.
    set(_usdgen_private_imaging_resources
        "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenPrivateImaging/resources")
    file(MAKE_DIRECTORY "${_usdgen_private_imaging_resources}")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../plugin/usdGenImaging/resources/plugInfo.json.in"
        _usdgen_private_imaging_pluginfo)
    string(REPLACE "../../$<TARGET_FILE_NAME:usdGenImaging>"
        "$<TARGET_FILE:usdGenPrivateImaging>"
        _usdgen_private_imaging_pluginfo "${_usdgen_private_imaging_pluginfo}")
    file(GENERATE
        OUTPUT "${_usdgen_private_imaging_resources}/plugInfo.json"
        CONTENT "${_usdgen_private_imaging_pluginfo}")

    add_executable(testUsdGenCudaGroomGroupPublication
        testCudaGroomGroupPublication.cpp)
    target_compile_definitions(testUsdGenCudaGroomGroupPublication PRIVATE
        USDGEN_ENABLE_CUDA
        USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(testUsdGenCudaGroomGroupPublication PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenCudaGroomGroupPublication PRIVATE
        usdGenPrivateImaging usdGenPatchedStorm hdx
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so"
        "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaGroomGroupPublication
        COMMAND testUsdGenCudaGroomGroupPublication)
    set_tests_properties(testUsdGenCudaGroomGroupPublication PROPERTIES
        LABELS "T2;storm-extension;groom-group-ingress;cuda"
        SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        ENVIRONMENT "PXR_PLUGINPATH_NAME=${_usdgen_schema_resources}:${_usdgen_private_imaging_resources}:${_hdst_gpu_group_staging_resources};LD_LIBRARY_PATH=$<TARGET_FILE_DIR:usdGenPrivateImaging>:$<TARGET_FILE_DIR:usdGenPatchedStorm>:${USD_INSTALL_DIR}/lib")
    add_executable(testUsdGenCudaGroomRender testCudaGroomRender.cpp)
    target_compile_definitions(testUsdGenCudaGroomRender PRIVATE
        USDGEN_ENABLE_CUDA
        USDGEN_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(testUsdGenCudaGroomRender PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenCudaGroomRender PRIVATE
        usdGenPrivateImaging usdGenPatchedStorm hdx
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so"
        "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaGroomRender COMMAND testUsdGenCudaGroomRender)
    set_tests_properties(testUsdGenCudaGroomRender PROPERTIES
        LABELS "T2;storm-extension;groom-group-render;cuda"
        SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK usdgen_gpu
        ENVIRONMENT "PXR_PLUGINPATH_NAME=${_usdgen_schema_resources}:${_usdgen_private_imaging_resources}:${_hdst_gpu_group_staging_resources};LD_LIBRARY_PATH=$<TARGET_FILE_DIR:usdGenPrivateImaging>:$<TARGET_FILE_DIR:usdGenPatchedStorm>:${USD_INSTALL_DIR}/lib")
    add_executable(testUsdGenStormCudaPublication ../testUsdGenCudaGlInterop.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaGlComputation.cpp)
    target_compile_definitions(testUsdGenStormCudaPublication PRIVATE
        USDGEN_HAS_CUDA_GL_INTEROP USDGEN_TEST_STORM_POST_COMMIT USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenStormCudaPublication PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenStormCudaPublication PRIVATE usdGenPatchedStorm
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so" "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenStormCudaPublication COMMAND testUsdGenStormCudaPublication)
    set_tests_properties(testUsdGenStormCudaPublication PROPERTIES
        LABELS "T2;storm-extension;publication;cuda" SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")
    add_executable(testUsdGenCudaBasisCurvesProvider testCudaBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaGlComputation.cpp)
    target_compile_definitions(testUsdGenCudaBasisCurvesProvider PRIVATE
        USDGEN_HAS_CUDA_GL_INTEROP USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenCudaBasisCurvesProvider PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenCudaBasisCurvesProvider PRIVATE usdGenPatchedStorm
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so" "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaBasisCurvesProvider COMMAND testUsdGenCudaBasisCurvesProvider)
    set_tests_properties(testUsdGenCudaBasisCurvesProvider PROPERTIES
        LABELS "T2;storm-extension;basis-curves;cuda" SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")
    add_executable(testUsdGenCudaTiledBasisCurvesProvider testCudaTiledBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaGlComputation.cpp)
    target_compile_definitions(testUsdGenCudaTiledBasisCurvesProvider PRIVATE
        USDGEN_HAS_CUDA_GL_INTEROP USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenCudaTiledBasisCurvesProvider PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenCudaTiledBasisCurvesProvider PRIVATE usdGenPatchedStorm
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so" "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaTiledBasisCurvesProvider COMMAND testUsdGenCudaTiledBasisCurvesProvider)
    set_tests_properties(testUsdGenCudaTiledBasisCurvesProvider PROPERTIES
        LABELS "T2;storm-extension;basis-curves;cuda;tiles" SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")
    add_executable(testUsdGenCudaNativeBasisCurves testCudaNativeBasisCurves.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaGlComputation.cpp)
    target_compile_definitions(testUsdGenCudaNativeBasisCurves PRIVATE
        USDGEN_HAS_CUDA_GL_INTEROP USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenCudaNativeBasisCurves PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenCudaNativeBasisCurves PRIVATE usdGenPatchedStorm hdx
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so" "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaNativeBasisCurves COMMAND testUsdGenCudaNativeBasisCurves)
    set_tests_properties(testUsdGenCudaNativeBasisCurves PROPERTIES
        LABELS "T2;storm-extension;native-basis-curves;cuda" SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")
    add_executable(testUsdGenCudaGpuGroupPublication testCudaGpuGroupPublication.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaGlComputation.cpp)
    target_compile_definitions(testUsdGenCudaGpuGroupPublication PRIVATE
        USDGEN_HAS_CUDA_GL_INTEROP USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenCudaGpuGroupPublication PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenCudaGpuGroupPublication PRIVATE usdGenPatchedStorm hdx
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so" "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenCudaGpuGroupPublication COMMAND testUsdGenCudaGpuGroupPublication)
    set_tests_properties(testUsdGenCudaGpuGroupPublication PROPERTIES
        LABELS "T2;storm-extension;gpu-group-publication;cuda" SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")
    # This compiles the application-side CUDA-group mapper directly against
    # private HdSt.  Do not link usdGenImaging here: that shared library is
    # built against the stock SDK and would load duplicate HdSt definitions.
    add_executable(testUsdGenDeviceCurveGroupPublisher testDeviceCurveGroupPublisher.cpp
        ../../libs/usdGenImaging/usdGenImaging/deviceCurveGroupPublisher.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaBasisCurvesProvider.cpp
        ../../libs/usdGenImaging/usdGenImaging/cudaGlComputation.cpp
        ../../libs/usdGenImaging/usdGenImaging/usdGenTilePublisher.cpp)
    target_compile_definitions(testUsdGenDeviceCurveGroupPublisher PRIVATE
        USDGEN_HAS_CUDA_GL_INTEROP USDGEN_ENABLE_CUDA)
    target_include_directories(testUsdGenDeviceCurveGroupPublisher PRIVATE
        ../../libs/usdGen ../../libs/usdGenImaging ../../usdGenShaders/test)
    target_link_libraries(testUsdGenDeviceCurveGroupPublisher PRIVATE usdGenPatchedStorm hdx
        "${USDGEN_CUDA_BUILD_DIR}/libusdGen.so" "${USDGEN_CUDA_BUILD_DIR}/libusdGenGpu.a"
        CUDA::cudart CUDA::cusolver ${CMAKE_DL_LIBS})
    add_test(NAME testUsdGenDeviceCurveGroupPublisher
        COMMAND testUsdGenDeviceCurveGroupPublisher)
    set_tests_properties(testUsdGenDeviceCurveGroupPublisher PROPERTIES
        LABELS "T2;storm-extension;device-curve-group-publisher;cuda"
        SKIP_RETURN_CODE 77 TIMEOUT 60
        ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:usdGenPatchedStorm>")
endif()
