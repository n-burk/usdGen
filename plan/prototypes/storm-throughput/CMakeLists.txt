cmake_minimum_required(VERSION 3.20)
project(hairbench CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(pxr CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
add_executable(hairbench hairbench.cpp)
target_link_libraries(hairbench
    usd usdGeom usdImagingGL hd hdSt glf garch gf tf trace OpenGL::GL)
