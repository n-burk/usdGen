# usdGen M0 consumer probe (plan §6.2; sol S-5/X-4).
#
# A minimal downstream CMake project proving the installed package config is
# usable. testUsdGenConsumer (tests/testUsdGenConsumer.cpp) configures and
# builds this directory against a scratch install prefix
# (USDGEN_INSTALL_PREFIX) and runs the resulting binary.
#
# Order: find_package(pxr) FIRST, then find_package(usdGen). pxrConfig is
# single-pass — a second find_package(pxr CONFIG) re-runs it and detonates at
# add_library(TBB::tbb IMPORTED) ("target already exists"). usdGenConfig
# guards its own find_dependency(pxr) with if(NOT TARGET usd), so once pxr is
# imported here, the usdGen find is safe in either order — but the consumer
# must not find pxr a second time.
cmake_minimum_required(VERSION 3.26)
project(usdGenConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pxr REQUIRED CONFIG)
find_package(usdGen REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE usdGen::usdGen)

option(USDGEN_CONSUMER_VULKAN "Validate the explicitly installed Vulkan runtime" OFF)
if(USDGEN_CONSUMER_VULKAN)
    if(NOT TARGET usdGen::usdGenVulkanNative OR NOT TARGET usdGen::usdGenVulkanFactory)
        message(FATAL_ERROR "Installed usdGen package has no explicit Vulkan runtime")
    endif()
    add_executable(vulkanConsumer vulkan.cpp)
    target_link_libraries(vulkanConsumer PRIVATE usdGen::usdGenVulkanNative usdGen::usdGenVulkanFactory)
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_SHADER="${usdGen_VULKAN_WIDTH_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_PROFILE_SHADER="${usdGen_VULKAN_WIDTH_PROFILE_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_SHADER="${usdGen_VULKAN_LENGTH_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_SET_SHADER="${usdGen_VULKAN_LENGTH_SET_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_CUT_EXTEND_SHADER="${usdGen_VULKAN_LENGTH_CUT_EXTEND_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_REPARAM_SHADER="${usdGen_VULKAN_LENGTH_REPARAM_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_MINIMUM_SHADER="${usdGen_VULKAN_LENGTH_MINIMUM_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_LITERAL_V1_SHADER="${usdGen_VULKAN_LENGTH_LITERAL_V1_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_ENVELOPE_V1_SHADER="${usdGen_VULKAN_LENGTH_ENVELOPE_V1_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_WIDTH_BLEND_SHADER="${usdGen_VULKAN_WIDTH_BLEND_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_NON_WIDTH_COMPARE_SHADER="${usdGen_VULKAN_NON_WIDTH_COMPARE_SHADER}")
    target_compile_definitions(vulkanConsumer PRIVATE USDGEN_VULKAN_LENGTH_COMPACTION_SHADER="${usdGen_VULKAN_LENGTH_COMPACTION_SHADER}")
endif()
