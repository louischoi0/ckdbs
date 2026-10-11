#!/bin/bash
# Extension (after the pre-specified matrix): 8 more A vs B' pairs of corr-exists-scan at 200/1000/10000,
# and a positive control, A vs the previous B (23e53bed), 8 pairs at 1000 and 200 and 10000.
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/nhf
O=/home/cdkbs/bench-runs/nhf
C=/home/cdkbs/bench-runs/nhf-ctl
date -u '+%Y-%m-%d %H:%M UTC' > $O/start-ext.txt
for n in 200 1000 10000; do
  python3 $S/nh_run.py 8 $n $O/res-ext --only corr-exists-scan >> $O/run-ext.log 2>&1
  NH_OUT=$C python3 $S/nh_run.py 8 $n $C/res --only corr-exists-scan >> $C/run.log 2>&1
done
date -u '+%Y-%m-%d %H:%M UTC' > $O/end-ext.txt
echo ALLDONE >> $C/run.log
