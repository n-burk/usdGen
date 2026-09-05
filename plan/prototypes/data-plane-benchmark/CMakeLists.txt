cmake_minimum_required(VERSION 3.20)
project(usdGenDataPlaneProbe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(pxr CONFIG REQUIRED
  PATHS /home/burkard/work/OpenUSD_26_08 NO_DEFAULT_PATH)

add_executable(vdfBench vdfBench.cpp)
target_include_directories(vdfBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vdfBench PRIVATE vdf exec tf gf vt work trace arch)

add_executable(tbbBench tbbBench.cpp)
target_include_directories(tbbBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  /home/burkard/work/OpenUSD_26_08/include)
target_link_libraries(tbbBench PRIVATE tf gf vt work arch TBB::tbb)

add_executable(simdBench simdBench.cpp)
target_include_directories(simdBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(simdBench PRIVATE -fopt-info-vec-optimized)
target_link_options(vdfBench PRIVATE -rdynamic)

add_executable(simdBenchNoVec simdBench.cpp)
target_include_directories(simdBenchNoVec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(simdBenchNoVec PRIVATE -fno-tree-vectorize)

add_executable(vdfBench2 vdfBench2.cpp)
target_include_directories(vdfBench2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vdfBench2 PRIVATE vdf exec tf gf vt work trace arch)
target_link_options(vdfBench2 PRIVATE -rdynamic)
