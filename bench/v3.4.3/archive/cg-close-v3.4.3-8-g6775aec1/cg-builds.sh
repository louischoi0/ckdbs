#!/bin/bash
# CG-S3 builds: A = f44846ef (v3.4.3, exported tree), B = 6775aec1 (this worktree).
WT=/home/cdkbs/ckdbs/.claude/worktrees/cg-on-conflict-do-update
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
BIN=/home/cdkbs/bench-runs/cg/bin
mkdir -p "$BIN"
P=$SP/cg-builds.progress
echo "$(date -u +%T) start" > "$P"
cmake -S "$SP/cg-base" -B "$SP/cg-base/build-release" -DCMAKE_BUILD_TYPE=Release > "$SP/cg-buildA.log" 2>&1 &&
  cmake --build "$SP/cg-base/build-release" -j8 --target kds_server >> "$SP/cg-buildA.log" 2>&1 &&
  cp "$SP/cg-base/build-release/kds_server" "$BIN/kds_server.f44846ef" || { echo "A FAIL" >> "$P"; exit 1; }
echo "$(date -u +%T) A done" >> "$P"
cd "$WT" || exit 1
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release > "$SP/cg-buildB.log" 2>&1 &&
  cmake --build build-release -j8 --target kds_server >> "$SP/cg-buildB.log" 2>&1 &&
  cp build-release/kds_server "$BIN/kds_server.6775aec1" || { echo "B FAIL" >> "$P"; exit 1; }
echo "$(date -u +%T) B done; tree $(git status --short | wc -l) changes at $(git rev-parse --short HEAD)" >> "$P"
sha256sum "$BIN"/kds_server.f44846ef "$BIN"/kds_server.6775aec1 >> "$P"
echo BUILDS-END >> "$P"
