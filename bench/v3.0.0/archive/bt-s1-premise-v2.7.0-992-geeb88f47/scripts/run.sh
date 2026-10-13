#!/bin/bash
# BT-S1: bm_scan census, bn cell, arms A, A2 (same bytes), D1, D2, D3, all, D4; 5 runs.
R=/home/cdkbs/bench-runs/bt-s1
W=/home/cdkbs/ckdbs/.claude/worktrees/bt-accept-tuple-row
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
mkdir -p $R/out $R/bn10000
rm -f $R/run.done
echo "start $(date -u +%T)" > $R/progress
( while [ ! -f $R/run.done ]; do echo "$(date -u +%T) $(cat /proc/loadavg)" >> $R/out/hostwatch.log; sleep 10; done ) &
B=$R/bin
python3 $W/tools/bm_scan.py census --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm D1=$B/kds_server-D1 \
  --arm D2=$B/kds_server-D2 --arm D3=$B/kds_server-D3 --arm all=$B/kds_server-all --arm D4=$B/kds_server-D4 \
  --workdir $R/bn10000 --port 32711 --runs 5 --pin-server 2 --pin-driver 4 --cells bn --scan-ops 192 --point-ops 3000 \
  --quiet-load 2 --quiet-wait 240 $GATE --out $R/out/bmscan-bn.json > $R/out/bmscan-bn.log 2>&1
echo "bm_scan done rc=$? $(date -u +%T)" >> $R/progress
touch $R/run.done
