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

set(USDGEN_CUDA_BUILD_DIR "" CACHE PATH "Optional matching usdGen CUDA build for real transfer validation")
if(USDGEN_CUDA_BUILD_DIR)
    find_package(CUDAToolkit REQUIRED)
    foreach(_library libusdGen.so libusdGenGpu.a)
        if(NOT EXISTS "${USDGEN_CUDA_BUILD_DIR}/${_library}")
            message(FATAL_ERROR "Missing ${USDGEN_CUDA_BUILD_DIR}/${_library}; build usdGen with CUDA first")
        endif()
    endforeach()
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
endif()
