#!/bin/sh
# BI-S1's measurement (workorder-bi-checkpoint-off-the-reactor.md §5.1):
# cores {1,4} x durability {relaxed, group}, three runs each, interleaved,
# 600 s per cell, against the archived build-release of 55ee8370.
DIR=/home/cdkbs/bench-runs/bi-s1-v2.7.0-779-g55ee8370
DRIVER=/home/cdkbs/ckdbs/.claude/worktrees/bi-checkpoint-off-the-reactor/tools/bi_checkpoint_stall.py
mkdir -p "$DIR/work" "$DIR/cells"
for run in 1 2 3; do
  for cell in "1 relaxed" "4 relaxed" "1 group" "4 group"; do
    set -- $cell
    python3 "$DRIVER" --server "$DIR/kds_server" --workdir "$DIR/work" \
      --cores "$1" --durability "$2" --seconds 600 \
      --tag "c$1-$2-r$run" --out "$DIR/cells/c$1-$2-r$run.json" >> "$DIR/run.log" 2>&1
    echo "done c$1-$2-r$run rc=$?" >> "$DIR/run.log"
    rm -f "$DIR/work/c$1-$2-r$run.db"
    rm -rf "$DIR/work/c$1-$2-r$run.db.wal"
  done
done
echo ALLDONE >> "$DIR/run.log"
