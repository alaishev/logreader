cmake_minimum_required(VERSION 3.25)

project(LogReader VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Performance is the main goal, so build Release unless asked otherwise
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_TESTING "Build unit tests (requires GoogleTest)" OFF)
option(BUILD_BENCHMARKS "Build performance benchmarks (requires Google Benchmark), see run-bench.sh" OFF)

add_library(logreader STATIC
    src/filter.cpp
    src/log_reader.cpp
)

add_library(logreader::logreader ALIAS logreader)

target_include_directories(logreader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# CLI utility: logreader-cli <path_to_file> <filter>
add_executable(logreader-cli
    src/main.cpp
)

target_link_libraries(logreader-cli PRIVATE logreader::logreader)


if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()