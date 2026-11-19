#!/bin/bash
# descriptive cell: 3 cells of 6 s for each of FOR UPDATE and plain, alternating, B binary only, fresh server each.
R=/home/louis/ck-tmp/dc-close/run
D=/home/louis/ckdbs/.claude/worktrees/dc-select-for-update/bench/v3.4.13/archive/dc-close-v3.4.13-14-gcd80d31d
cd $R
n=0
for i in 1 2 3; do
  for m in fu plain; do
    n=$((n+1))
    while pgrep -f "cc1plus|cmake --build|ctest --test" > /dev/null; do echo "$(date -u +%FT%TZ) rmw held $(cat /proc/loadavg)" >> $R/held.log; sleep 15; done
    echo "$(date -u +%FT%TZ) rmw $m cell $i start load $(cat /proc/loadavg)" >> $R/lives.log
    echo "rmw $m $i" > $R/status.txt
    taskset -c 5 python3 -I $D/rmw_cell.py --binary $R/B.bin --wd $R/wd-rmw --mode $m --seconds 6 --port $((27900 + n)) \
      --json $D/rmw-$m-$i.json > $D/rmw-$m-$i.log 2>&1
    rm -f $R/wd-rmw/*.db; rm -rf $R/wd-rmw/*.wal
  done
done
echo "$(date -u +%FT%TZ) RMW ALLDONE" > $R/status.txt
