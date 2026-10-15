#!/bin/bash
# Multi-core census at 9b7d53d7: KDS then PostgreSQL, one ba_census.py invocation per cell, sequentially.
# Usage: census.sh <shapes...>   (shapes run in the given order; cells: sessions {16,8} x cores/cpuset {1,2,4})
W=/home/cdkbs/ckdbs/.claude/worktrees/pk-range-strict-bounds
A=$W/bench/v3.0.0/archive/census-multicore-v3.2.0-64-g9b7d53d7
ROOT=/home/cdkbs/bench-runs/mc-v320
WORK=$ROOT/work
PROG=$ROOT/progress
mkdir -p $A/kds $A/pg $WORK
BIN=$WORK/kds_server.release
test -f $BIN || cp /tmp/claude-1000/-home-cdkbs-ckdbs/0221b531-38a9-4c26-b0d4-39c18d514efa/scratchpad/b/kds_server $BIN
sha256sum $BIN >> $A/binary.sha256
COMMON="--runs 3 --seconds 4 --warmup 1 --client-cpus 4,5,6,7 --max-load 6 --max-wait 120 --workdir $WORK"
gate() {
  n=0
  while pgrep -x ctest > /dev/null || pgrep -x cc1plus > /dev/null; do
    sleep 10; n=$((n + 1)); test $n -ge 60 && break
  done
  test $n -gt 0 && echo "GATE waited $((n * 10)) s" >> $A/census.log
  echo "load $(cut -d' ' -f1-3 /proc/loadavg)" >> $A/census.log
}
for shape in "$@"; do
  for s in 16 8; do
    for k in 1 2 4; do
      case $k in 1) cpus=0;; 2) cpus=0,2;; 4) cpus=0,2,4,6;; esac
      echo "$(date -u +%T) kds $shape k$k s$s" >> $PROG
      gate
      python3 $W/tools/ba_census.py --engine kds --bin $BIN --port 15641 --shape $shape --durability relaxed \
        --cores $k --sessions $s --server-cpus $cpus --out $A/kds $COMMON >> $A/census.log 2>&1 || echo "CELL FAILED kds $shape k$k s$s" >> $A/census.log
    done
  done
  for s in 16 8; do
    for k in 1 2 4; do
      case $k in 1) cpus=0;; 2) cpus=0,2;; 4) cpus=0,2,4,6;; esac
      echo "$(date -u +%T) pg $shape cpus$cpus s$s" >> $PROG
      gate
      python3 $W/tools/ba_census.py --engine pg --pg-port 15434 --shape $shape --durability off --sessions $s \
        --server-cpus $cpus --out $A/pg/cpus$(echo $cpus | tr , -) $COMMON >> $A/census.log 2>&1 || echo "CELL FAILED pg $shape cpus$cpus s$s" >> $A/census.log
    done
  done
done
echo "$(date -u +%T) DONE" >> $PROG
