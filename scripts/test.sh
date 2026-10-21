#!/bin/sh
# Resolved from this script's own location, not from the caller's working
# directory: these scripts moved from the repo root into scripts/, and a
# bare ./build.sh only worked while both sat at the root.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
# A cell labelled `bs-nightly` is BS's (instructions/v3.0.0/workorder-bs-test-hardening.md
# §6.1): it runs from scripts/bs/nightly.sh, never from the push hook.
ctest --test-dir "$ROOT/build" --output-on-failure -LE bs-nightly
