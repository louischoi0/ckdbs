#!/bin/bash
# Q1: ba_census A vs B interleaved run by run (3 runs per arm per cell), arm order flipped per run.
R=/home/louis/bench-runs/dp-close
T=/home/louis/ckdbs/.claude/worktrees/sync-stale-records/tools/ba_census.py
HB=$R/q1b.heartbeat
declare -A BIN=([A]=$R/bin/kds_server_A [B]=$R/bin/kds_server_B [A2]=$R/bin/kds_server_A2)
declare -A PORT=([A]=15781 [B]=15782 [A2]=15783)
for k in 1; do
  if [ $k = 1 ]; then SC=2; SS=2; CC=4,5; else SC=0,2; SS=4; CC=4,5,6,7; fi
  for shape in point insert1 pkmix; do
    for run in 1 2 3 4 5 6; do
      case $((run % 3)) in 1) ORD="A A2 B";; 2) ORD="B A A2";; 0) ORD="A2 B A";; esac
      for arm in $ORD; do
        out=$R/archive/q1b/k$k-$shape-$arm-r$run
        mkdir -p $out
        echo "$(date -u +%T) start k=$k $shape $arm r$run load=$(cut -d' ' -f1-3 /proc/loadavg) cc1=$(pgrep -c cc1plus)" >> $HB
        python3 -I $T --engine kds --bin ${BIN[$arm]} --port ${PORT[$arm]} --shape $shape \
          --durability relaxed --cores $k --sessions $SS --seconds 8 --warmup 2 --runs 1 \
          --rows 10000 --server-cpus $SC --client-cpus $CC --workdir $R/work-$arm \
          --max-load 3 --max-wait 30 --out $out > $out/stdout.log 2>&1
        echo "$(date -u +%T) done k=$k $shape $arm r$run rc=$?" >> $HB
      done
    done
  done
done
echo "$(date -u +%T) Q1b END" >> $HB
