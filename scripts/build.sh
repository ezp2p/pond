#!/usr/bin/env bash
# Build and test the Pond. The first build also fetches and builds Drogon, which takes a few minutes.
#   scripts/build.sh            configure, build, run tests
#   scripts/build.sh --clean    wipe build/ first
set -euo pipefail
cd "$(dirname "$0")/.."

[ "${1:-}" = --clean ] && rm -rf build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
# A job count, always: with Makefiles, a bare --parallel means no limit, and a full rebuild (Drogon too) then starts dozens of
# compilers at once, enough to run a 32 GB machine out of memory. At most 8 unless CMAKE_BUILD_PARALLEL_LEVEL says.
cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(( cores < 8 ? cores : 8 ))}
cmake --build build --parallel "$jobs"
ctest --test-dir build --output-on-failure
