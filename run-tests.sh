#!/usr/bin/env bash
set -e

# Unit tests. GoogleTest comes from Conan; the build goes to build-dev/, separate from the plain build/.
# Arguments are passed to ctest, for example: ./run-tests.sh -R FilterMatch
conan install . --output-folder=build-dev --build=missing
cmake --preset conan-release -DBUILD_TESTING=ON
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure "$@"
