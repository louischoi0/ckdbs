#!/bin/bash
# AF-T5's owed cells: 7 groups, the only sizing that puts a second declared
# group on a writer core. Plus one bridge cell (g3-c4) re-run at this commit,
# so the new numbers can be read against §3's table rather than only against
# their own arms.
set -uo pipefail
cd /home/cdkbs/ckdbs/.claude/worktrees/workorder-wf-te-t5
ARCH=bench/v2.8.0/archive/af-t5-namespace-grouping
LOG=/tmp/claude-1000/-home-cdkbs-ckdbs/d260129e-cb45-4611-831b-e7d2a57c5ae1/scratchpad/af_t5_g7.log
mkdir -p "$ARCH"
: > "$LOG"

run() {
  local name="$1"; shift
  echo "==== $name : $(date -Iseconds) ====" | tee -a "$LOG"
  python3 bench/af_namespace_grouping_probe.py --server build-release/kds_server \
    --workdir /home/cdkbs/mcbench/af7 --port 15700 "$@" \
    --json "$ARCH/$name.json" > "$LOG.$name.txt" 2>&1
  tail -40 "$LOG.$name.txt" >> "$LOG"
  echo "==== $name done : $(date -Iseconds) ====" | tee -a "$LOG"
}

run g3-c4-at154df22 --cores 4 --groups 3 --rows 2000 --reps 5
run g7-c4 --cores 4 --groups 7 --rows 2000 --reps 5
run g7-c8 --cores 8 --groups 7 --rows 2000 --reps 5

echo "G7 SWEEP COMPLETE $(date -Iseconds)" | tee -a "$LOG"
