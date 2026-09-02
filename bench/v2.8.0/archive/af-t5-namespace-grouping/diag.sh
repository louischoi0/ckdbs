#!/bin/bash
# Why is g7-c8's load 2.2x slower under `namespace` than under `rotate`?
#
# The hypothesis, from kds.conf.sample's own words about D2: "one fsync
# amortized over a batch of concurrent committers ... a batch of one is a
# batch". Seven groups over seven writer cores is the one cell where the two
# placements differ in *sessions per core*: `namespace` puts a group's pair
# on one core, so every core has exactly one committing session and no
# partner to batch with; `rotate` splits every pair, so every core gets two
# sessions from two different groups and batches them.
#
# The control is the durability class. Under `relaxed` there is no
# per-commit sync to batch, so if the gap is the batch-of-one it collapses,
# and if it survives it is something else.
set -uo pipefail
cd /home/cdkbs/ckdbs/.claude/worktrees/workorder-wf-te-t5
ARCH=bench/v2.8.0/archive/af-t5-namespace-grouping
LOG=/tmp/claude-1000/-home-cdkbs-ckdbs/d260129e-cb45-4611-831b-e7d2a57c5ae1/scratchpad/af_t5_diag.log
: > "$LOG"

quiet() {
  for i in $(seq 1 60); do
    load=$(cut -d' ' -f1 /proc/loadavg)
    ok=$(awk -v l="$load" 'BEGIN{print (l < 2.0) ? 1 : 0}')
    if [ "$ok" = "1" ]; then return; fi
    sleep 20
  done
}

run() {
  local name="$1"; shift
  quiet
  echo "==== $name (load $(cut -d' ' -f1 /proc/loadavg)) : $(date -Iseconds) ====" | tee -a "$LOG"
  python3 bench/af_namespace_grouping_probe.py --server build-release/kds_server \
    --workdir /home/cdkbs/mcbench/afdiag --port 15800 "$@" \
    --json "$ARCH/$name.json" > "$LOG.$name.txt" 2>&1
  tail -30 "$LOG.$name.txt" >> "$LOG"
  echo "==== $name done : $(date -Iseconds) ====" | tee -a "$LOG"
}

# The cell again under the default class, to confirm the gap reproduces.
run g7-c8-group   --cores 8 --groups 7 --rows 2000 --reps 3
# The same cell with the per-commit sync taken out of the picture.
run g7-c8-relaxed --cores 8 --groups 7 --rows 2000 --reps 3 --durability relaxed
# And the cell where the two placements have the SAME sessions per core, as
# the negative control: if sessions-per-core is the mechanism, `group` and
# `relaxed` should move both arms together here.
run g3-c8-relaxed --cores 8 --groups 3 --rows 2000 --reps 3 --durability relaxed

echo "DIAG COMPLETE $(date -Iseconds)" | tee -a "$LOG"
