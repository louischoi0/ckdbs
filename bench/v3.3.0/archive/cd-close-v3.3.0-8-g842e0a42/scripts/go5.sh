#!/bin/bash
R=/home/cdkbs/bench-runs/cd-close
S=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
chmod +x $S/run5.sh $S/watch5.sh
while [ ! -f $R/run4.done ]; do sleep 2; done
echo "$(date -u +%T) part 5 launch (load $(cut -d' ' -f1-3 /proc/loadavg), cc1plus $(pgrep -c cc1plus))" >> $R/progress.txt
nohup $S/run5.sh > $R/out/run5.sh.out 2>&1 &
echo $! > $R/run5.pid
nohup $S/watch5.sh > /dev/null 2>&1 &
sleep 1
