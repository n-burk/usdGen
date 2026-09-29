# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(noodles_package_consumer LANGUAGES CXX)

option(NOODLES_PACKAGE_CONSUMER_WITH_USD
    "Require the optional noodles::usd installed target"
    OFF)

find_package(noodles 1.1 CONFIG REQUIRED)

add_executable(noodles_package_consumer main.cpp)
target_link_libraries(noodles_package_consumer PRIVATE noodles::noodles)
target_compile_features(noodles_package_consumer PRIVATE cxx_std_17)
target_compile_definitions(noodles_package_consumer PRIVATE
    NOODLES_PACKAGE_ASSET_DIR="${noodles_ASSET_DIR}")

if(NOODLES_PACKAGE_CONSUMER_WITH_USD)
    if(NOT TARGET noodles::usd)
        message(FATAL_ERROR
            "Installed noodles package does not include optional noodles::usd")
    endif()
    target_link_libraries(noodles_package_consumer PRIVATE noodles::usd)
    target_compile_definitions(noodles_package_consumer PRIVATE
        NOODLES_PACKAGE_CONSUMER_WITH_USD)
endif()
