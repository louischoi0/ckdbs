#!/bin/bash
# Whole measurement, sequential, never overlapping a build of ours. Runners pinned off CPUs 3-7.
H=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/bw
R=/home/cdkbs/bench-runs/bw/res
mkdir -p $R
cd $H
P="taskset -c 0-2 python3 -u"
date -u '+%F %T' > $R/start.txt
$P bw_run1.py 8 200,1000,10000 $R/c1-main > $R/c1-main.log 2>&1
BW_SWAP=1 $P bw_run1.py 5 200,1000,10000 $R/c1-swap > $R/c1-swap.log 2>&1
$P bw_run23.py c2 12 1000 20000 $R/c2-n1000 > $R/c2-n1000.log 2>&1
$P bw_run23.py c3 12 1000 10000 $R/c3-n1000 > $R/c3-n1000.log 2>&1
$P bw_run23.py c3c 12 1000 10000 $R/c3c-n1000 > $R/c3c-n1000.log 2>&1
$P bw_run23.py c3b 12 1000 20000 $R/c3b-n1000 > $R/c3b-n1000.log 2>&1
for n in 200 10000; do
  $P bw_run23.py c2 6 $n 20000 $R/c2-n$n > $R/c2-n$n.log 2>&1
  $P bw_run23.py c3 6 $n 10000 $R/c3-n$n > $R/c3-n$n.log 2>&1
  $P bw_run23.py c3c 6 $n 10000 $R/c3c-n$n > $R/c3c-n$n.log 2>&1
done
date -u '+%F %T' > $R/end.txt
echo ALLDONE > $R/done.txt
