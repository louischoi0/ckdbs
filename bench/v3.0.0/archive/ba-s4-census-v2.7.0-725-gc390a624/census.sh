#!/bin/sh
# BA-S4's census: every cell of BA-R0's matrix, KDS then PostgreSQL, one
# ba_census.py invocation per cell, sequentially. Detached by census-start.sh.
W=/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks
OUT=$W/bench/v3.0.0/archive/ba-s4-census-v2.7.0-725-gc390a624
WORK=$HOME/bench-runs/ba-s4/work
LOG=$OUT/census.log
mkdir -p "$OUT" "$WORK"
BIN=$WORK/kds_server.release
test -f "$BIN" || cp "$W/build-release/kds_server" "$BIN"
sha256sum "$BIN" >> "$OUT/binary.sha256"
# A cell whose three runs are already in its directory is not run again:
# the census resumes where a stop left it.
done3() { test -f "$1-r0.json" && test -f "$1-r1.json" && test -f "$1-r2.json"; }
COMMON="--runs 3 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 --max-load 6 --max-wait 300 --workdir $WORK"
for shape in point trade insert1 insertN recent; do
  for dur in relaxed group strict; do
    for k in 1 2 4; do
      case $k in 1) cpus=0;; 2) cpus=0,2;; 4) cpus=0,2,4,6;; esac
      for s in 1 2 4 8 16; do
        done3 "$OUT/kds/kds-$shape-$dur-k$k-s$s" && continue
        python3 "$W/tools/ba_census.py" --engine kds --bin "$BIN" --port 15641 \
          --shape $shape --durability $dur --cores $k --sessions $s --server-cpus $cpus \
          --out "$OUT/kds" $COMMON >> "$LOG" 2>&1 || echo "CELL FAILED kds $shape $dur k$k s$s" >> "$LOG"
      done
    done
  done
done
for shape in point trade insert1 insertN recent; do
  for dur in off on; do
    for cpus in 0 0,2 0,2,4,6; do
      for s in 1 2 4 8 16; do
        done3 "$OUT/pg-cpus$(echo $cpus | tr , -)/pg-$shape-$dur-s$s" && continue
        python3 "$W/tools/ba_census.py" --engine pg --pg-port 15434 \
          --shape $shape --durability $dur --sessions $s --server-cpus $cpus \
          --out "$OUT/pg-cpus$(echo $cpus | tr , -)" $COMMON >> "$LOG" 2>&1 || echo "CELL FAILED pg $shape $dur $cpus s$s" >> "$LOG"
      done
    done
  done
done
date -u > "$OUT/census-done.flag"
