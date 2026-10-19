#!/bin/bash
R=/home/cdkbs/bench-runs/cd-close
S=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
chmod +x $S/run.sh $S/watch.sh
rm -rf $R/smoke
echo "$(date -u +%T) measurement launch (load $(cut -d' ' -f1-3 /proc/loadavg), cc1plus $(pgrep -c cc1plus))" >> $R/progress.txt
nohup $S/run.sh > $R/out/run.sh.out 2>&1 &
echo $! > $R/run.pid
nohup $S/watch.sh > /dev/null 2>&1 &
sleep 2
tail -n 3 $R/progress.txt
