#!/usr/bin/env bash
# Build and test the Pond.
#   scripts/build.sh            configure, build, run tests
#   scripts/build.sh --clean    wipe build/ first
#   scripts/build.sh --local    build against ../puddle instead of the pinned deps/puddle
set -euo pipefail
cd "$(dirname "$0")/.."

puddle=deps/puddle
for a in "$@"; do
  case "$a" in
    --clean) rm -rf build ;;
    --local) puddle=../puddle ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
[ "$puddle" = deps/puddle ] && git submodule update --init deps/puddle

args=(-DCMAKE_BUILD_TYPE=Debug -DPUDDLE_DIR="$PWD/$puddle")
# Homebrew's OpenSSL is keg-only; the system one is LibreSSL.
if [ "$(uname)" = Darwin ] && command -v brew >/dev/null; then
  args+=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)")
fi

cmake -S . -B build "${args[@]}"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
