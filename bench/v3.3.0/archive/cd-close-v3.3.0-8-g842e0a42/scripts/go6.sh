#!/bin/bash
R=/home/cdkbs/bench-runs/cd-close
S=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
chmod +x $S/run6.sh $S/watch6.sh
while [ ! -f $R/run5.done ]; do sleep 2; done
echo "$(date -u +%T) part 6 launch (load $(cut -d' ' -f1-3 /proc/loadavg), cc1plus $(pgrep -c cc1plus))" >> $R/progress.txt
nohup $S/run6.sh > $R/out/run6.sh.out 2>&1 &
echo $! > $R/run6.pid
nohup $S/watch6.sh > /dev/null 2>&1 &
sleep 1
