#!/usr/bin/env bash
# Build and test the Pond. The first build also fetches and builds Drogon, which takes a few minutes.
#   scripts/build.sh            configure, build, run tests
#   scripts/build.sh --clean    wipe build/ first
set -euo pipefail
cd "$(dirname "$0")/.."

[ "${1:-}" = --clean ] && rm -rf build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
