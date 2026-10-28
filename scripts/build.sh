#!/usr/bin/env bash
# Build and test the Pond.
#   scripts/build.sh            configure, build, run tests
#   scripts/build.sh --clean    wipe build/ first
set -euo pipefail
cd "$(dirname "$0")/.."

[ "${1:-}" = --clean ] && rm -rf build

args=(-DCMAKE_BUILD_TYPE=Debug)
# Homebrew's OpenSSL is keg-only; the system one is LibreSSL.
if [ "$(uname)" = Darwin ] && command -v brew >/dev/null; then
  args+=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)")
fi

cmake -S . -B build "${args[@]}"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
