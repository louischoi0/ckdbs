#!/bin/sh
# Resolved from this script's own location, not from the caller's working
# directory: these scripts moved from the repo root into scripts/, and a
# bare ./build.sh only worked while both sat at the root.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
# `-LE heap-suspended` is SUS-1's label doing what the order asks
# (instructions/v3.0.0/workorder-as-sus1-heap-suspended.md, AS-R4): heap-only
# cells are excluded from the gate. **Registering the label is not excluding
# it** - the two `gtest_discover_tests` calls in tests/CMakeLists.txt put the
# labelled cells in the same ctest project as everything else, so without
# this flag a labelled cell still runs here and the label is only a name.
# The labelled set is empty today (AS-Q4: there is no nightly runner, and
# excluding cells from the only thing that runs them would delete coverage),
# so this changes nothing until a cell is renamed to carry the convention.
ctest --test-dir "$ROOT/build" --output-on-failure -LE heap-suspended
