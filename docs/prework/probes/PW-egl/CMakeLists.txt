cmake_minimum_required(VERSION 3.20)
project(stormHairLook CXX)
set(CMAKE_CXX_STANDARD 17)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(pxr CONFIG REQUIRED)
add_executable(render_hair render_hair.cpp)
target_link_libraries(render_hair PRIVATE usdAppUtils usdImagingGL usdImaging usdGeom usd garch hd hdSt hgiGL tf gf sdf hio dl)

add_executable(bench_hair bench_hair.cpp)
target_link_libraries(bench_hair PRIVATE usdImagingGL usdImaging usdGeom usd garch glf hd hdSt hgiGL cameraUtil tf gf sdf vt hio dl)

# PW-2 (S-8): variant A vs B, first-frame (SDR compile) + steady state
add_executable(bench_s8 bench_s8.cpp)
target_link_libraries(bench_s8 PRIVATE usdImagingGL usdImaging usdGeom usd garch glf hd hdSt hgiGL cameraUtil tf gf sdf vt hio dl)

# PW-3 (S-9): refineLevel toggle, per-frame timing
add_executable(bench_refine bench_refine.cpp)
target_link_libraries(bench_refine PRIVATE usdImagingGL usdImaging usdGeom usd garch glf hd hdSt hgiGL cameraUtil tf gf sdf vt hio dl)
