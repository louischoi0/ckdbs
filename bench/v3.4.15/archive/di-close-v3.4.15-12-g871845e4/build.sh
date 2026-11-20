#!/bin/sh
# DI close: the two server binaries, built Release from clean exports of the two commits, in sequence.
#   A = 1c7bb774 (v3.4.15-4-g1c7bb774), B = 871845e4 (v3.4.15-12-g871845e4)
set -e
S=/tmp/claude-1000/-home-louis-ckdbs/32f3df8a-da5e-4b9f-ac13-631b11be169e/scratchpad
R=/home/louis/bench-runs/di-close
mkdir -p "$R/bin"
build() {  # label tar
    rm -rf "$R/src-$1"; mkdir -p "$R/src-$1"
    tar -x -C "$R/src-$1" -f "$2"
    echo "start $1 $(date -u +%FT%TZ)"
    cmake -S "$R/src-$1" -B "$R/src-$1/build-release" -DCMAKE_BUILD_TYPE=Release \
        -DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF > "$R/build-$1.log" 2>&1
    cmake --build "$R/src-$1/build-release" -j"$(nproc)" --target kds_server >> "$R/build-$1.log" 2>&1
    cp "$R/src-$1/build-release/kds_server" "$R/bin/kds_$1"
    stat -c "%y" "$R/src-$1/build-release/kds_server" > "$R/bin/mtime_$1"
    echo "built $1 $(date -u +%FT%TZ)"
}
build A "$S/di-s0/head.tar"
build B "$S/di-close-B.tar"
(cd "$R/bin" && sha256sum kds_A kds_B > sha256)
cat "$R/bin/sha256"
echo "BUILD DONE $(date -u +%FT%TZ)"
