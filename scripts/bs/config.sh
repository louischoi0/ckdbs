#!/bin/sh
# BS-S1: one BS configuration, built in a directory of its own and run there
# (instructions/v3.0.0/workorder-bs-test-hardening.md).
#
#   scripts/bs/config.sh <config> <step>...     steps: build, suite, sim
#
#   asan      Debug, ASan + UBSan, clang (BS-Q2 (a))   -> build-bs-asan/
#   tsan      Debug, TSan, clang                        -> build-bs-tsan/
#   noassert  Release, so every assert() is off, the default compiler
#                                                       -> build-bs-noassert/
#
# `suite` is the hook's own range - ctest less the labels scripts/test.sh
# excludes - so a configuration's verdict is about the same cells the push
# gate runs. `sim` is scripts/sim.sh with $BS_SIM_FRESH date-derived seeds
# (default 4) beside the committed corpus. None of this is built or run by
# scripts/githooks/pre-push (the order's §6.1); scripts/bs/nightly.sh runs it.
#
# clang is the user-space install scripts/bs/install-llvm18.sh makes, no
# sudo needed; KDS_LLVM points elsewhere.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CONFIG="$1"
[ $# -gt 0 ] && shift
JOBS="${BS_JOBS:-$(nproc)}"
KDS_LLVM="${KDS_LLVM:-/home/cdkbs/opt/llvm18/usr/lib/llvm-18}"
BUILD="$ROOT/build-bs-$CONFIG"
EXE_LINK=""

usage() {
    echo "usage: config.sh asan|tsan|noassert build|suite|sim..." >&2
    exit 2
}

use_clang() {
    [ -x "$KDS_LLVM/bin/clang++" ] ||
        { echo "config.sh: no clang++ under $KDS_LLVM; run scripts/bs/install-llvm18.sh" >&2; exit 2; }
    COMPILER="-DCMAKE_CXX_COMPILER=$KDS_LLVM/bin/clang++"
}

case "$CONFIG" in
    asan)
        use_clang
        FLAGS="-DCMAKE_BUILD_TYPE=Debug -DKDS_SANITIZE=address"
        # A leak fails its cell as any other report does; UBSan's stack
        # names the line.
        ASAN_OPTIONS="external_symbolizer_path=$KDS_LLVM/bin/llvm-symbolizer"
        UBSAN_OPTIONS="print_stacktrace=1:external_symbolizer_path=$KDS_LLVM/bin/llvm-symbolizer"
        export ASAN_OPTIONS UBSAN_OPTIONS
        ;;
    tsan)
        use_clang
        # The runtime shared: clang links TSan's static C++ runtime whole,
        # and its operator new/delete then collide with the test binary's
        # replacements (tests/alloc_counter.cpp); a shared one is interposed
        # over instead.
        EXE_LINK="-shared-libsan -Wl,-rpath,$KDS_LLVM/lib/clang/18/lib/linux"
        FLAGS="-DCMAKE_BUILD_TYPE=Debug -DKDS_SANITIZE=thread"
        TSAN_OPTIONS="external_symbolizer_path=$KDS_LLVM/bin/llvm-symbolizer"
        # A death test forks a threaded process, which TSan reports; the
        # threadsafe style re-executes the binary instead.
        GTEST_DEATH_TEST_STYLE=threadsafe
        export TSAN_OPTIONS GTEST_DEATH_TEST_STYLE
        ;;
    noassert)
        COMPILER=""
        FLAGS="-DCMAKE_BUILD_TYPE=Release"
        ;;
    *) usage ;;
esac

[ $# -gt 0 ] || usage
for STEP in "$@"; do
    case "$STEP" in
        build)
            # shellcheck disable=SC2086
            cmake -S "$ROOT" -B "$BUILD" $COMPILER $FLAGS \
                ${EXE_LINK:+"-DCMAKE_EXE_LINKER_FLAGS=$EXE_LINK"}
            cmake --build "$BUILD" -j"$JOBS"
            ;;
        suite)
            # The label set scripts/test.sh excludes, read from it so the
            # two cannot drift.
            LE=$(sed -n 's/.*-LE \([^ ]*\).*/\1/p' "$ROOT/scripts/test.sh")
            # --no-tests=error: a broken registration is a failure, not a pass.
            ctest --test-dir "$BUILD" -j"$JOBS" --output-on-failure --no-tests=error ${LE:+-LE "$LE"}
            ;;
        sim)
            SIM_BIN="$BUILD/ckdbs-sim" SIM_ARTIFACTS="${SIM_ARTIFACTS:-$BUILD/sim-artifacts.txt}" \
                "$ROOT/scripts/sim.sh" "${BS_SIM_FRESH:-4}"
            ;;
        *) usage ;;
    esac
done
