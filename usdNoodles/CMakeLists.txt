cmake_minimum_required(VERSION 3.20)

project(noodles VERSION 1.1.0 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(CTest)
include(FetchContent)
include(GNUInstallDirs)

option(BUILD_SHARED_LIBS "Build noodles as a shared library" ON)
option(NOODLES_FETCH_DEPENDENCIES "Fetch missing third-party dependencies" ON)
option(NOODLES_REQUIRE_TESTS
    "Fail configuration when BUILD_TESTING is on but GoogleTest is unavailable"
    OFF)
option(NOODLES_ENABLE_USD_EXPRESSION_CONNECTIONS
    "Build the optional OpenUSD SeExpr attribute-connection adapter"
    OFF)
set(NOODLES_USDGEN_SCHEMA_RESOURCE_DIR "" CACHE PATH
    "usdGenSchema build-tree resources for optional USD adapter tests")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NOODLES_HEADERS
    core/AttributeConnectionDrag.h
    core/Animator.h
    core/Glyph.h
    core/GraphLayout.h
    core/LinkSelectionMode.h
    core/Math.h
    core/NodeData.h
    core/NodeLayout.h
    core/NodeVertex.h
    core/RenderConfig.h
    core/api.h
    core/pragmas.h
    render/FontAtlas.h
    render/GraphNodeRenderer.h
    render/GraphRenderer.h
    render/IconRenderManager.h
    render/IconVertexCache.h
    render/LinkCurveParams.h
    render/LinkGeometry.h
    render/LinkRenderManager.h
    render/NodeRenderManager.h
    render/NodeTransformFrame.h
    render/NodeVertexCache.h
    render/Profiler.h
    render/ShaderLibrary.h
    render/StickerRenderer.h
    render/TextLayout.h
    render/TextRenderManager.h
    render/TextVertexCache.h
    render/VertexGenerator.h
    render/noodles_gl.h
    spatial/SpatialIndex.h
    undo/Command.h
    undo/CompoundCommand.h
    undo/LambdaCommand.h
    undo/NoodlesUndoManager.h
)

set(NOODLES_SOURCES
    core/AttributeConnectionDrag.cpp
    core/Animator.cpp
    core/GraphLayout.cpp
    core/NodeData.cpp
    core/NodeLayout.cpp
    core/NodeVertex.cpp
    core/RenderConfig.cpp
    render/FontAtlas.cpp
    render/GraphNodeRenderer.cpp
    render/GraphRenderer.cpp
    render/IconRenderManager.cpp
    render/IconVertexCache.cpp
    render/LinkGeometry.cpp
    render/LinkRenderManager.cpp
    render/NodeRenderManager.cpp
    render/NodeTransformFrame.cpp
    render/NodeVertexCache.cpp
    render/Profiler.cpp
    render/ShaderLibrary.cpp
    render/StickerRenderer.cpp
    render/TextLayout.cpp
    render/TextRenderManager.cpp
    render/TextVertexCache.cpp
    render/VertexGenerator.cpp
    spatial/SpatialIndex.cpp
    undo/CompoundCommand.cpp
    undo/LambdaCommand.cpp
    undo/NoodlesUndoManager.cpp
)

# stb_image and RapidJSON are implementation-only dependencies. Fetching uses a
# deliberately empty SOURCE_SUBDIR so modern FetchContent populates the source
# without importing either dependency's own build or test targets.
find_path(NOODLES_STB_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb)
if(NOT NOODLES_STB_INCLUDE_DIR)
    if(NOT NOODLES_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "stb_image.h was not found and NOODLES_FETCH_DEPENDENCIES=OFF")
    endif()
    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4
        SOURCE_SUBDIR noodles-cmake-unused
    )
    FetchContent_MakeAvailable(stb)
    set(NOODLES_STB_INCLUDE_DIR "${stb_SOURCE_DIR}")
endif()

find_package(RapidJSON QUIET)
if(RapidJSON_FOUND)
    if(RapidJSON_INCLUDE_DIRS)
        set(NOODLES_RAPIDJSON_INCLUDE_DIR "${RapidJSON_INCLUDE_DIRS}")
    elseif(RapidJSON_INCLUDE_DIR)
        set(NOODLES_RAPIDJSON_INCLUDE_DIR "${RapidJSON_INCLUDE_DIR}")
    endif()
endif()
if(NOT NOODLES_RAPIDJSON_INCLUDE_DIR)
    if(NOT NOODLES_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "RapidJSON was not found and NOODLES_FETCH_DEPENDENCIES=OFF")
    endif()
    FetchContent_Declare(
        rapidjson
        GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
        GIT_TAG f54b0e47a08782a6131cc3d60f94d038fa6e0a51
        SOURCE_SUBDIR noodles-cmake-unused
    )
    FetchContent_MakeAvailable(rapidjson)
    set(NOODLES_RAPIDJSON_INCLUDE_DIR "${rapidjson_SOURCE_DIR}/include")
endif()

add_library(noodles ${NOODLES_HEADERS} ${NOODLES_SOURCES})
add_library(noodles::noodles ALIAS noodles)

# Runtime hosts pass this directory to ShaderLibrary/FontAtlas. Keep a
# build-tree contract for add_subdirectory consumers; the installed package
# config publishes the corresponding installed path under the same name.
set(NOODLES_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH
    "Path to the noodles runtime fonts and shaders")
set_property(TARGET noodles PROPERTY NOODLES_ASSET_DIR "${NOODLES_ASSET_DIR}")

set_target_properties(noodles PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION "${PROJECT_VERSION}"
    # Public structs are still experimental. Keep each minor line on its own
    # shared-library install name until the ABI is declared stable.
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
)
target_compile_features(noodles PUBLIC cxx_std_17)

# stb_image is header-only; exactly one translation unit provides its
# implementation for both shared and static builds.
set_source_files_properties(render/FontAtlas.cpp PROPERTIES
    COMPILE_DEFINITIONS STB_IMAGE_IMPLEMENTATION
)

target_include_directories(noodles
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/noodles>
)
target_include_directories(noodles SYSTEM PRIVATE
        "${NOODLES_STB_INCLUDE_DIR}"
        "${NOODLES_RAPIDJSON_INCLUDE_DIR}"
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(noodles PRIVATE NOODLES_EXPORTS)
else()
    target_compile_definitions(noodles PUBLIC NOODLES_STATIC)
endif()

# GL header and link selection mirrors render/noodles_gl.h:
#   iOS/iPadOS       OpenGL ES 3 framework
#   macOS            system OpenGL 3 core framework
#   Android          GLESv3
#   Linux/Windows    OpenGL + GLEW
set(NOODLES_PACKAGE_GL_BACKEND "")
set(_noodles_install_targets noodles)
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_library(NOODLES_OPENGLES_FRAMEWORK OpenGLES REQUIRED)
    # Use a relocatable framework link item in the exported target rather than
    # embedding this machine's absolute Xcode SDK path.
    target_link_libraries(noodles PUBLIC "-framework OpenGLES")
    target_compile_definitions(noodles PRIVATE GLES_SILENCE_DEPRECATION)
    set(NOODLES_PACKAGE_GL_BACKEND "OpenGLES")
elseif(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(noodles PUBLIC OpenGL::GL)
    set(NOODLES_PACKAGE_GL_BACKEND "OpenGL")
elseif(ANDROID)
    target_link_libraries(noodles PUBLIC GLESv3)
    set(NOODLES_PACKAGE_GL_BACKEND "GLESv3")
else()
    find_package(OpenGL REQUIRED)
    find_package(GLEW QUIET)
    if(TARGET GLEW::GLEW)
        target_link_libraries(noodles PUBLIC OpenGL::GL GLEW::GLEW)
        set(NOODLES_PACKAGE_GL_BACKEND "SystemGLEW")
    else()
        if(NOT NOODLES_FETCH_DEPENDENCIES)
            message(FATAL_ERROR "GLEW was not found and NOODLES_FETCH_DEPENDENCIES=OFF")
        endif()
        FetchContent_Declare(
            glew
            URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip
            URL_HASH SHA256=a9046a913774395a095edcc0b0ac2d81c3aacca61787b39839b941e9be14e0d4
            SOURCE_SUBDIR noodles-cmake-unused
        )
        FetchContent_MakeAvailable(glew)

        add_library(noodles_glew STATIC "${glew_SOURCE_DIR}/src/glew.c")
        set_target_properties(noodles_glew PROPERTIES
            EXPORT_NAME glew
            POSITION_INDEPENDENT_CODE ON
        )
        target_include_directories(noodles_glew
            PUBLIC
                $<BUILD_INTERFACE:${glew_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        # GLEW's optional GLU declarations require a system GLU header even
        # though noodles never uses GLU. Keep this public so every consumer of
        # the bundled GLEW target sees the same no-GLU header contract.
        target_compile_definitions(noodles_glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
        target_link_libraries(noodles_glew PUBLIC OpenGL::GL)
        target_link_libraries(noodles PUBLIC noodles_glew)
        list(APPEND _noodles_install_targets noodles_glew)
        set(NOODLES_PACKAGE_GL_BACKEND "BundledGLEW")
        set(NOODLES_BUNDLED_GLEW_INCLUDE_DIR "${glew_SOURCE_DIR}/include")
    endif()
endif()

set(NOODLES_PACKAGE_WITH_USD OFF)
if(NOODLES_ENABLE_USD_EXPRESSION_CONNECTIONS)
    find_package(pxr REQUIRED CONFIG)
    add_library(noodles_usd usd/ExpressionConnections.h usd/ExpressionConnections.cpp
        usd/api.h)
    add_library(noodles::usd ALIAS noodles_usd)
    target_link_libraries(noodles_usd PUBLIC noodles::noodles usd)
    target_include_directories(noodles_usd PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/noodles>)
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(noodles_usd PRIVATE NOODLES_USD_EXPORTS)
    else()
        target_compile_definitions(noodles_usd PUBLIC NOODLES_USD_STATIC)
    endif()
    set_target_properties(noodles_usd PROPERTIES
        EXPORT_NAME usd
        POSITION_INDEPENDENT_CODE ON
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    list(APPEND _noodles_install_targets noodles_usd)
    set(NOODLES_PACKAGE_WITH_USD ON)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(noodles_tests
            tests/AttributeConnectionDragTest.cpp
            tests/GraphLayoutTest.cpp
            tests/NodeTransformFrameTest.cpp
            tests/NoodlesCoreTest.cpp
            tests/NoodlesHeadlessTest.cpp
            tests/NoodlesIconRenderTest.cpp
            tests/NoodlesRenderTest.cpp
            tests/NoodlesSpatialTest.cpp
            tests/NoodlesUndoTest.cpp
            tests/TextVertexCacheTest.cpp
        )
        target_link_libraries(noodles_tests PRIVATE noodles::noodles GTest::gtest_main)
        add_test(NAME noodles.unit COMMAND noodles_tests --gtest_color=no)
        set_tests_properties(noodles.unit PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOODLES_ENABLE_USD_EXPRESSION_CONNECTIONS)
            if(NOT EXISTS "${NOODLES_USDGEN_SCHEMA_RESOURCE_DIR}/plugInfo.json")
                message(FATAL_ERROR
                    "NOODLES_USDGEN_SCHEMA_RESOURCE_DIR must contain plugInfo.json when optional USD tests are enabled")
            endif()
            add_executable(noodles_usd_tests tests/ExpressionConnectionsTest.cpp)
            target_link_libraries(noodles_usd_tests PRIVATE noodles::usd GTest::gtest_main)
            add_test(NAME noodles.usd.expression_connections
                COMMAND noodles_usd_tests --gtest_color=no)
            set_tests_properties(noodles.usd.expression_connections PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                ENVIRONMENT "PXR_PLUGINPATH_NAME=${NOODLES_USDGEN_SCHEMA_RESOURCE_DIR}")
        endif()
    else()
        if(NOODLES_REQUIRE_TESTS)
            message(FATAL_ERROR
                "GoogleTest not found while NOODLES_REQUIRE_TESTS=ON")
        endif()
        message(STATUS "GoogleTest not found; noodles unit tests will not be built")
    endif()
endif()

set(NOODLES_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/noodles"
    CACHE STRING "Install location for noodles CMake package files")
set(NOODLES_INSTALL_ASSETDIR "${CMAKE_INSTALL_DATADIR}/noodles/assets")

install(TARGETS ${_noodles_install_targets}
    EXPORT noodlesTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY core/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/noodles/core"
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY render/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/noodles/render"
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY spatial/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/noodles/spatial"
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY undo/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/noodles/undo"
    FILES_MATCHING PATTERN "*.h")
if(NOODLES_ENABLE_USD_EXPRESSION_CONNECTIONS)
    install(DIRECTORY usd/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/noodles/usd"
        FILES_MATCHING PATTERN "*.h")
endif()
install(DIRECTORY assets/ DESTINATION "${NOODLES_INSTALL_ASSETDIR}")
if(NOODLES_BUNDLED_GLEW_INCLUDE_DIR)
    install(DIRECTORY "${NOODLES_BUNDLED_GLEW_INCLUDE_DIR}/GL"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(FILES LICENSE.txt THIRD_PARTY_LICENSES.txt README.md BUILDING.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/noodles")

configure_package_config_file(
    cmake/noodlesConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/noodlesConfig.cmake"
    INSTALL_DESTINATION "${NOODLES_INSTALL_CMAKEDIR}"
    PATH_VARS NOODLES_INSTALL_ASSETDIR
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/noodlesConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
)

install(EXPORT noodlesTargets
    FILE noodlesTargets.cmake
    NAMESPACE noodles::
    DESTINATION "${NOODLES_INSTALL_CMAKEDIR}"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/noodlesConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/noodlesConfigVersion.cmake"
    DESTINATION "${NOODLES_INSTALL_CMAKEDIR}"
)
