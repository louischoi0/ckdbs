#!/bin/bash
# BL-S5 close: the statement matrix, then the mixed matrix - never overlapped.
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
ARC=/home/cdkbs/ckdbs/.claude/worktrees/bl-s4-partition/bench/v3.4.0/archive/bl-close-v3.4.1-10-gf4bad273
mkdir -p "$ARC"
echo "$(date -u +%T) RUN-START" > "$SP/s5-run.progress"
bash "$SP/s5-stmt.sh" "$ARC/stmt"
echo "$(date -u +%T) stmt finished" >> "$SP/s5-run.progress"
bash "$SP/s5-mixed.sh" "$ARC/mixed"
echo "$(date -u +%T) RUN-END" >> "$SP/s5-run.progress"
