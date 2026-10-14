#!/usr/bin/env bash
set -e

# Benchmarks. Google Benchmark comes from Conan; the build goes to build-dev/, separate from the plain build/.
# Arguments are passed to Google Benchmark, for example: ./run-bench.sh --benchmark_filter=Match
conan install . --output-folder=build-dev --build=missing
cmake --preset conan-release -DBUILD_BENCHMARKS=ON
cmake --build --preset conan-release --target logreader_bench
build-dev/bench/logreader_bench "$@"
