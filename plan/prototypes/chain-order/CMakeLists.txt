cmake_minimum_required(VERSION 3.20)
project(GChainOrder CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenGL REQUIRED)
find_package(pxr CONFIG REQUIRED)

foreach(tag A B)
  add_library(usdGenStub${tag} SHARED src/stub.cpp)
  target_compile_definitions(usdGenStub${tag} PRIVATE STUB_TAG_ID=${tag})
  target_link_libraries(usdGenStub${tag} PRIVATE
      ${PXR_LIBRARIES})
  target_include_directories(usdGenStub${tag} PRIVATE ${PXR_INCLUDE_DIRS})
  set_target_properties(usdGenStub${tag} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()

add_library(usdGenStubRenderer SHARED src/stubRenderer.cpp)
target_link_libraries(usdGenStubRenderer PRIVATE ${PXR_LIBRARIES})
target_include_directories(usdGenStubRenderer PRIVATE ${PXR_INCLUDE_DIRS})
set_target_properties(usdGenStubRenderer PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(probeChainOrder src/probeChainOrder.cpp)
target_link_libraries(probeChainOrder PRIVATE ${PXR_LIBRARIES})
target_include_directories(probeChainOrder PRIVATE ${PXR_INCLUDE_DIRS})
# rigExecImaging is resolved at runtime through the plugin; the probe declares
# the three C entry points itself and links the shared object directly.
if(EXISTS "${RIGEXEC_IMAGING_LIB}")
  target_link_libraries(probeChainOrder PRIVATE "${RIGEXEC_IMAGING_LIB}")
endif()
