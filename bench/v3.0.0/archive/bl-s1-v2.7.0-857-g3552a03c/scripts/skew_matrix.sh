#!/bin/sh
# BL-S1 skew cells (BL §5.1): arms interleaved per run, one fresh server per run.
# Usage: skew_matrix.sh OUTDIR RUNS   (writes OUTDIR/done when finished)
OUT=$1; RUNS=${2:-5}
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bl-session-balancer/tools
BIN=/home/cdkbs/bench-runs/bl/bin/kds_server.3552a03c
mkdir -p "$OUT"
for r in $(seq 1 "$RUNS"); do
  for cell in "2 16 0,2 4,5,6,7" "4 16 0,2,4,6 1,3,5,7" "4 8 0,2,4,6 1,3,5,7" "2 4 0,2 4,5,6,7"; do
    set -- $cell
    for dur in relaxed group; do
      for arm in kernel balanced skew; do
        /home/cdkbs/bench-runs/bl/quiet_gate.sh; python3 $TOOLS/ba_census.py --bin $BIN --port 17441 --shape pkmix --durability $dur \
          --cores $1 --sessions $2 --server-cpus $3 --client-cpus $4 --placement $arm \
          --runs 1 --seconds 8 --warmup 2 --rows 16000 --max-load 3.0 --max-wait 15 \
          --workdir /home/cdkbs/bench-runs/bl/work-skew --out "$OUT/r$r" >> "$OUT/log.txt" 2>&1
      done
    done
  done
done
touch "$OUT/done"
