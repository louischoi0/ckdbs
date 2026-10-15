#!/bin/bash
R=/home/cdkbs/bench-runs/aq-close
W=/home/cdkbs/ckdbs/.claude/worktrees/aq-expression-cabins
B=$R/bin
while [ ! -f $R/run3.done ]; do sleep 3; done
CELLGATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
COMMON="--pin-server 2 --pin-driver 4 --scan-rows 1000 --blocks 6 --quiet-load 2 --quiet-wait 30 $CELLGATE"
echo "$(date -u +%T) phase start f3" >> $R/progress.txt
rm -rf $R/wd-f3; mkdir -p $R/wd-f3
python3 $W/tools/bn_census.py --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A --arm Bp=$B/kds_server-Bp --arm B=$B/kds_server-B \
  --workdir $R/wd-f3 --port 33021 --runs 4 --rows 1000 --ops 2000 --range-ops 120 \
  --only select-pk,update-range $COMMON --out $R/out/f3.json > $R/out/f3.log 2>&1 &
echo $! > $R/flags/f3.pid; echo f3 > $R/flags/current
wait $!; rc=$?
echo "$(date -u +%T) phase end f3 rc=$rc" >> $R/progress.txt
touch $R/run4.done
