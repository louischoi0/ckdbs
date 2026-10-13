#!/bin/bash
# second batch: the every-row UPDATE only, the four servers started in the reverse order (B2, B, A', A)
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bm-scan-kernel-open/382ae513-4d71-443e-8517-51d482efafba/scratchpad/s6r
W=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
R=/home/cdkbs/bench-runs/bm-s6r
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
cd $W
while [ ! -f $S/all.done ]; do sleep 2; done
mkdir -p $R/bn1000r
rm -f $S/stop_watch
( while [ ! -f $S/stop_watch2 ]; do echo "$(date -u +%T) $(cat /proc/loadavg)" >> $R/out/hostwatch.log; sleep 10; done ) &
echo "batch2 start $(date -u +%T)" >> $S/progress
python3 tools/bn_census.py --arm B2=$R/bin/kds_server-B2 --arm B=$R/bin/kds_server-B --arm A2=$R/bin/kds_server-A2 --arm A=$R/bin/kds_server-A \
  --workdir $R/bn1000r --port 32621 --runs 5 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
  --only update-range --quiet-load 2 --quiet-wait 240 $GATE --out $R/out/bn1000r.json > $R/out/bn1000r.log 2>&1
echo "batch2 done rc=$? $(date -u +%T)" >> $S/progress
touch $S/batch2.done
touch $S/stop_watch2
