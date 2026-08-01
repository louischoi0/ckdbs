#!/bin/sh
# Paths are relative to the repo root, resolved from this script's own
# location so it works from anywhere - it no longer sits at the root.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$ROOT/build" -j"$(nproc)"
