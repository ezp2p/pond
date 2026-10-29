#!/usr/bin/env bash
# Copy what doco publishes from this repository into the doco repository beside it (../doco, or the folder given): the API spec
# (api/) and the design (docs/). Run it whenever either changes, and commit the result in doco too.
#   scripts/sync-doco.sh            into ../doco/pond
#   scripts/sync-doco.sh <doco>     into <doco>/pond
set -euo pipefail
cd "$(dirname "$0")/.."
doco=${1:-../doco}
[ -d "$doco/.git" ] || { echo "no doco repository at $doco" >&2; exit 1; }
out="$doco/pond"

rm -rf "$out/api" "$out/docs"
mkdir -p "$out/api" "$out/docs"
cp -R api/. "$out/api/"
cp -R docs/. "$out/docs/"
echo "copied api/ and docs/ into $out ($(git rev-parse --short HEAD 2>/dev/null || echo uncommitted))"
