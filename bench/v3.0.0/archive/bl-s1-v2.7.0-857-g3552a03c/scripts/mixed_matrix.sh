#!/bin/sh
# BL-S1 mixed cells (BL §5.1): A, the one-core priority premise with the slice
# sweep; B, the four-core heterogeneous placement premise (BL-R2's partition).
# Usage: mixed_matrix.sh OUTDIR RUNS   (waits for WAITFOR if given; writes OUTDIR/done)
OUT=$1; RUNS=${2:-5}; WAITFOR=$3
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bl-session-balancer/tools
BIN=/home/cdkbs/bench-runs/bl/bin/kds_server.bls1-slice
if [ -n "$WAITFOR" ]; then while [ ! -f "$WAITFOR" ]; do sleep 10; done; fi
mkdir -p "$OUT"
M="/home/cdkbs/bench-runs/bl/run_gated.sh python3 $TOOLS/bl_mixed.py --bin $BIN --port 17451 --rate 400 --seconds 8 --warmup 2 --runs 1 --max-load 3.0 --max-wait 15 --workdir /home/cdkbs/bench-runs/bl/work-mixed"
for r in $(seq 1 "$RUNS"); do
  # A: one core, every session on it.
  for cell in "isolated 0" "insert 0" "walk 64" "walk 32" "walk 16" "mixed 64" "mixed 32" "mixed 16"; do
    set -- $cell
    $M --cores 1 --placement same --arm $1 --slice $2 --server-cpus 0 --client-cpus 4,5,6,7 --out "$OUT/A/r$r" >> "$OUT/log.txt" 2>&1
  done
  # B: four cores, heterogeneous sessions; how they are placed.
  for pl in kernel spread segregated; do
    $M --cores 4 --placement $pl --arm mixed --readers 6 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --out "$OUT/B/r$r" >> "$OUT/log.txt" 2>&1
  done
  $M --cores 4 --placement spread --arm isolated --readers 6 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --out "$OUT/B/r$r" >> "$OUT/log.txt" 2>&1
done
touch "$OUT/done"
