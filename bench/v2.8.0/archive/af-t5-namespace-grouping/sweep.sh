#!/bin/bash
set -uo pipefail
cd /home/cdkbs/ckdbs/.claude/worktrees/workorder-wf-te-t5
ARCH=bench/v2.8.0/archive/af-t5-namespace-grouping
LOG=/tmp/claude-1000/-home-cdkbs-ckdbs/d260129e-cb45-4611-831b-e7d2a57c5ae1/scratchpad/af_t5_sweep.log
mkdir -p "$ARCH"
: > "$LOG"

run() {
  local name="$1"; shift
  echo "==== $name : $(date -Iseconds) ====" | tee -a "$LOG"
  python3 bench/af_namespace_grouping_probe.py --server build-release/kds_server \
    --workdir /home/cdkbs/mcbench/af --port 15600 "$@" \
    --json "$ARCH/$name.json" > "$LOG.$name.txt" 2>&1
  tail -30 "$LOG.$name.txt" >> "$LOG"
  echo "==== $name done : $(date -Iseconds) ====" | tee -a "$LOG"
}

run g1-c4 --cores 4 --groups 1 --rows 2000 --reps 5
run g2-c4 --cores 4 --groups 2 --rows 2000 --reps 5
run g2-c8 --cores 8 --groups 2 --rows 2000 --reps 5
run g3-c4 --cores 4 --groups 3 --rows 2000 --reps 5
run g3-c8 --cores 8 --groups 3 --rows 2000 --reps 5
run g7-c4 --cores 4 --groups 7 --rows 2000 --reps 5
run g7-c8 --cores 8 --groups 7 --rows 2000 --reps 5
run g7-c8-rows200 --cores 8 --groups 7 --rows 200 --reps 5
run g7-c8-rows10000 --cores 8 --groups 7 --rows 10000 --reps 3

echo "SWEEP COMPLETE $(date -Iseconds)" | tee -a "$LOG"
