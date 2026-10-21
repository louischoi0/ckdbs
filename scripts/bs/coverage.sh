#!/bin/sh
# BS-S2: line and branch coverage of src/, per subsystem, against the
# recorded floor (instructions/v3.0.0/workorder-bs-test-hardening.md;
# BS-Q2 (a) clang's source-based coverage, BS-Q4 (b) a drop below the floor
# fails the nightly and never a push).
#
#   scripts/bs/coverage.sh            build, run the hook's range, report
#   scripts/bs/coverage.sh --record   ... and rewrite the floor to what ran
#
# The floor is scripts/bs/coverage-floor.txt: one line per src/ subsystem,
# `<dir> <line %> <branch %>`, and a ratchet - `--record` raises a value and
# never lowers one, so a drop stays a failure until a person lowers it in a
# commit that says why. Never run by the push hook (§6.1).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JOBS="${BS_JOBS:-$(nproc)}"
KDS_LLVM="${KDS_LLVM:-/home/cdkbs/opt/llvm18/usr/lib/llvm-18}"
BUILD="$ROOT/build-bs-coverage"
LD_LIBRARY_PATH="$KDS_LLVM/lib:$KDS_LLVM/../../x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_CXX_COMPILER="$KDS_LLVM/bin/clang++" \
    -DCMAKE_BUILD_TYPE=Debug -DKDS_COVERAGE=ON
cmake --build "$BUILD" -j"$JOBS"

rm -rf "$BUILD/profiles"
mkdir -p "$BUILD/profiles"
# ctest starts one process per cell; `%4m` merges them online into a pool of
# four files rather than leaving ~4,000 profiles of the whole binary on disk.
LLVM_PROFILE_FILE="$BUILD/profiles/kds-%4m.profraw" \
    ctest --test-dir "$BUILD" -j"$JOBS" -LE bs-nightly --output-on-failure > "$BUILD/ctest.log" 2>&1 ||
    echo "coverage: some cells failed (see $BUILD/ctest.log); coverage reports what ran"
"$KDS_LLVM/bin/llvm-profdata" merge -sparse "$BUILD"/profiles/*.profraw -o "$BUILD/kds.profdata"
"$KDS_LLVM/bin/llvm-cov" export -summary-only -instr-profile="$BUILD/kds.profdata" \
    "$BUILD/tests/kds_tests" > "$BUILD/coverage.json"
python3 -I "$ROOT/scripts/bs/coverage_report.py" "$BUILD/coverage.json" \
    "$ROOT/scripts/bs/coverage-floor.txt" "$ROOT" "$@"
