cmake_minimum_required(VERSION 3.26)
project(rigExecConsumerProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
# NOTE (verified 2026-09-04): do NOT call find_package(pxr) here as well.
# rigExecConfig.cmake:113 calls find_dependency(pxr CONFIG PATHS ...) with a
# different argument list, so CMake 3.28's find_dependency call-hash does not
# short-circuit, pxrConfig.cmake runs twice, and its unconditional
# add_library(TBB::tbb SHARED IMPORTED) at pxrConfig.cmake:58 aborts the
# configure with "another target with the same name already exists".
find_package(rigExec CONFIG REQUIRED)
add_executable(consumerProbe main.cpp)
target_link_libraries(consumerProbe PRIVATE rigExec::rigExec rigExec::rigExecImaging usdImaging)
message(STATUS "rigExec_PLUGINPATHS = ${rigExec_PLUGINPATHS}")
