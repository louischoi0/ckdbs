#!/bin/bash
# BL-S5 builds: A (0af06bbc, exported tree), B (f4bad273, this worktree), B-nobal (B + nobal patch).
set -u
WT=/home/cdkbs/ckdbs/.claude/worktrees/bl-s4-partition
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
BIN=/home/cdkbs/bench-runs/bl/bin
mkdir -p "$BIN"
echo "$(date -u +%T) start" > "$SP/s5-builds.progress"
cmake -S "$SP/bl-base" -B "$SP/bl-base/build-release" -DCMAKE_BUILD_TYPE=Release > "$SP/s5-buildA.log" 2>&1 &&
  cmake --build "$SP/bl-base/build-release" -j8 --target kds_server >> "$SP/s5-buildA.log" 2>&1 &&
  cp "$SP/bl-base/build-release/kds_server" "$BIN/kds_server.0af06bbc" || { echo "A FAIL" >> "$SP/s5-builds.progress"; exit 1; }
echo "$(date -u +%T) A done" >> "$SP/s5-builds.progress"
cd "$WT" || exit 1
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release > "$SP/s5-buildB.log" 2>&1 &&
  cmake --build build-release -j8 --target kds_server >> "$SP/s5-buildB.log" 2>&1 &&
  cp build-release/kds_server "$BIN/kds_server.f4bad273" || { echo "B FAIL" >> "$SP/s5-builds.progress"; exit 1; }
echo "$(date -u +%T) B done" >> "$SP/s5-builds.progress"
F=src/server/load_placer.cpp
cp "$F" "$SP/lp-s5.bak"
sed -i 's|^std::uint32_t LoadPlacer::Place(std::uint32_t core, Session& session) {$|std::uint32_t LoadPlacer::Place(std::uint32_t core, Session\& session) {\n    static const bool kOff = std::getenv("KDS_BL_NO_BALANCE") != nullptr;  // measurement build only\n    if (kOff) return core;|; s|^#include <array>$|#include <array>\n#include <cstdlib>|' "$F"
git diff "$F" > "$SP/nobal-s5.patch"
cmake --build build-release -j8 --target kds_server >> "$SP/s5-buildB.log" 2>&1 &&
  cp build-release/kds_server "$BIN/kds_server.f4bad273-nobal" || echo "NOBAL FAIL" >> "$SP/s5-builds.progress"
cp "$SP/lp-s5.bak" "$F"
cmake --build build-release -j8 --target kds_server >> "$SP/s5-buildB.log" 2>&1
echo "$(date -u +%T) nobal done; source restored: $(git status --short | wc -l) changes" >> "$SP/s5-builds.progress"
sha256sum "$BIN"/kds_server.0af06bbc "$BIN"/kds_server.f4bad273 "$BIN"/kds_server.f4bad273-nobal >> "$SP/s5-builds.progress"
echo BUILDS-END >> "$SP/s5-builds.progress"
