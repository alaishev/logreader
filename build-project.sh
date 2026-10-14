#!/usr/bin/env bash
set -e

# Library and CLI only: plain CMake, no external dependencies (Release by default, see CMakeLists.txt).
# Tests and benchmarks need GoogleTest / Google Benchmark from Conan: see run-tests.sh and run-bench.sh
cmake -S . -B build
cmake --build build -j
