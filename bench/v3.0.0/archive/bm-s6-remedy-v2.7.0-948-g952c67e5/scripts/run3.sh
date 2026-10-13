#!/bin/bash
# third batch: the every-row UPDATE only, server start order B, B2, A, A'
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bm-scan-kernel-open/382ae513-4d71-443e-8517-51d482efafba/scratchpad/s6r
W=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
R=/home/cdkbs/bench-runs/bm-s6r
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
cd $W
mkdir -p $R/bn1000s
( while [ ! -f $S/stop_watch3 ]; do echo "$(date -u +%T) $(cat /proc/loadavg)" >> $R/out/hostwatch.log; sleep 10; done ) &
echo "batch3 start $(date -u +%T)" >> $S/progress
python3 tools/bn_census.py --arm B=$R/bin/kds_server-B --arm B2=$R/bin/kds_server-B2 --arm A=$R/bin/kds_server-A --arm A2=$R/bin/kds_server-A2 \
  --workdir $R/bn1000s --port 32631 --runs 5 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
  --only update-range --quiet-load 2 --quiet-wait 240 $GATE --out $R/out/bn1000s.json > $R/out/bn1000s.log 2>&1
echo "batch3 done rc=$? $(date -u +%T)" >> $S/progress
touch $S/batch3.done
touch $S/stop_watch3
