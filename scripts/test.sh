#!/bin/sh
# Resolved from this script's own location, not from the caller's working
# directory: these scripts moved from the repo root into scripts/, and a
# bare ./build.sh only worked while both sat at the root.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
ctest --test-dir "$ROOT/build" --output-on-failure
