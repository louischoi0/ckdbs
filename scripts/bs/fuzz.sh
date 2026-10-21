#!/bin/sh
# BS-S5: build the three libFuzzer targets and run each for a fixed budget
# (instructions/v3.0.0/workorder-bs-test-hardening.md).
#
#   scripts/bs/fuzz.sh [seconds per target]      default 600 (BS_FUZZ_SECONDS)
#
# clang with ASan + UBSan, the engine built with -fsanitize=fuzzer-no-link,
# in build-bs-fuzz/. Each target starts from its committed corpus,
# fuzz/corpus/<target>/, seeded by scripts/bs/fuzz_seeds.py from the
# parser corpus and the KWP golden sessions; what a run adds goes to
# build-bs-fuzz/corpus/<target>/ and a crash to build-bs-fuzz/crashes/,
# which a person commits under fuzz/crashes/<target>/ with the fix (BS-R3).
# Never run by the push hook (§6.1).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SECONDS_EACH="${1:-${BS_FUZZ_SECONDS:-600}}"
JOBS="${BS_JOBS:-$(nproc)}"
KDS_LLVM="${KDS_LLVM:-/home/cdkbs/opt/llvm18/usr/lib/llvm-18}"
BUILD="$ROOT/build-bs-fuzz"
LD_LIBRARY_PATH="$KDS_LLVM/lib:$KDS_LLVM/../../x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
ASAN_OPTIONS="external_symbolizer_path=$KDS_LLVM/bin/llvm-symbolizer"
UBSAN_OPTIONS="print_stacktrace=1:external_symbolizer_path=$KDS_LLVM/bin/llvm-symbolizer"
export ASAN_OPTIONS UBSAN_OPTIONS

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_CXX_COMPILER="$KDS_LLVM/bin/clang++" \
    -DCMAKE_BUILD_TYPE=Debug -DKDS_SANITIZE=address -DKDS_FUZZ=ON \
    -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF
cmake --build "$BUILD" -j"$JOBS" --target fuzz_sql fuzz_kwp fuzz_volume

failures=0
for target in fuzz_sql fuzz_kwp fuzz_volume; do
    work="$BUILD/corpus/$target"
    crashes="$BUILD/crashes/$target"
    mkdir -p "$work" "$crashes"
    # The committed corpus is read, never written: new inputs land in `work`.
    if "$BUILD/fuzz/$target" "$work" "$ROOT/fuzz/corpus/$target" \
        -max_total_time="$SECONDS_EACH" -timeout=30 -rss_limit_mb=4096 \
        -artifact_prefix="$crashes/" -print_final_stats=1 \
        > "$BUILD/$target.log" 2>&1; then
        echo "ok   $target: $(grep -E '^stat::number_of_executed_units' "$BUILD/$target.log" | awk '{print $2}') runs in ${SECONDS_EACH} s"
    else
        failures=$((failures + 1))
        # Newest first: the directory keeps earlier nights' artifacts.
        echo "FAIL $target: $(ls -t "$crashes" | head -1), log $BUILD/$target.log"
    fi
done
echo "fuzz: $failures target(s) with a crash"
[ "$failures" -eq 0 ]
