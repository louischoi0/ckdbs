#!/bin/bash
# BA-S17's census: the KDS half of BA-R0's matrix, one ba_census.py invocation per cell, sequentially.
# Same flags, pinning and loop order as BA-S4's census.sh (archive/ba-s4-census-*/census.sh); the only addition is
# gate(): before a cell starts, wait (at most 30 min) until no ctest / cc1plus / kds_tests process exists.
W=/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks
ROOT=/home/cdkbs/bench-runs/ba-s17
OUT=$ROOT/kds
WORK=$ROOT/work
LOG=$ROOT/census.log
mkdir -p "$OUT" "$WORK"
BIN=$WORK/kds_server.release
test -f "$BIN" || cp /home/cdkbs/bench-runs/ba-close/bin/kds_server-B "$BIN"
sha256sum "$BIN" >> "$ROOT/binary.sha256"
done3() { test -f "$1-r0.json" && test -f "$1-r1.json" && test -f "$1-r2.json"; }
gate() {
  n=0
  while pgrep -x ctest > /dev/null || pgrep -x cc1plus > /dev/null || pgrep -x kds_tests > /dev/null; do
    sleep 15; n=$((n + 1)); test $n -ge 120 && break
  done
  test $n -gt 0 && echo "GATE waited $((n * 15)) s" >> "$LOG"
}
COMMON="--runs 3 --seconds 8 --warmup 2 --client-cpus 4,5,6,7 --max-load 6 --max-wait 300 --workdir $WORK"
for shape in point trade insert1 insertN recent; do
  for dur in relaxed group strict; do
    for k in 1 2 4; do
      case $k in 1) cpus=0;; 2) cpus=0,2;; 4) cpus=0,2,4,6;; esac
      for s in 1 2 4 8 16; do
        done3 "$OUT/kds-$shape-$dur-k$k-s$s" && continue
        gate
        python3 "$W/tools/ba_census.py" --engine kds --bin "$BIN" --port 15641 \
          --shape $shape --durability $dur --cores $k --sessions $s --server-cpus $cpus \
          --out "$OUT" $COMMON >> "$LOG" 2>&1 || echo "CELL FAILED kds $shape $dur k$k s$s" >> "$LOG"
      done
    done
  done
done
date -u > "$ROOT/census-done.flag"
