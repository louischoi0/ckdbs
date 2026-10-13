#!/bin/sh
# BM-S6 builds: three plain Release exports (A, A', B) and B's frame-pointer build.
# Run detached: nohup sh build.sh > build.log 2>&1 &   (done flag: build.done)
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/cf0ad77a-b48f-47e8-b714-576faa5dc6c9/scratchpad/bm-s6
WT=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
cd "$SP" || exit 1
rm -f build.done build.failed
export_src() { # name commit
    rm -rf "$SP/src-$1"
    mkdir -p "$SP/src-$1"
    git -C "$WT" archive "$2" | tar -x -C "$SP/src-$1" || exit 1
}
build() { # name srcname extra-flags
    echo "=== build $1 from src-$2 ($3) $(date -u +%H:%M:%S)"
    if [ -n "$3" ]; then
        cmake -S "$SP/src-$2" -B "$SP/build-$1" -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF "-DCMAKE_CXX_FLAGS=$3" > "$SP/cmake-$1.log" 2>&1 || { touch build.failed; exit 1; }
    else
        cmake -S "$SP/src-$2" -B "$SP/build-$1" -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > "$SP/cmake-$1.log" 2>&1 || { touch build.failed; exit 1; }
    fi
    nice -n 5 cmake --build "$SP/build-$1" --target kds_server -j8 > "$SP/make-$1.log" 2>&1 || { touch build.failed; exit 1; }
    echo "=== done $1 $(date -u +%H:%M:%S)"
}
export_src A fa80da9d
export_src A2 fa80da9d
export_src B 6b771ec1
build A A ""
build B B ""
build A2 A2 ""
build Bfp B "-fno-omit-frame-pointer"
gcc -O2 -shared -fPIC -o "$SP/kds_sampler.so" "$WT/tools/kds_sampler.c" || { touch build.failed; exit 1; }
touch build.done
