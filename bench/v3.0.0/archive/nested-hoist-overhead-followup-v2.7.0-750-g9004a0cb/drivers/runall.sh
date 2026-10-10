#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/nhf
O=/home/cdkbs/bench-runs/nhf
date -u '+%Y-%m-%d %H:%M UTC' > $O/start.txt
python3 $S/nh_run.py 8 200 $O/res --only corr-exists-scan > $O/run.log 2>&1
python3 $S/nh_run.py 8 1000 $O/res --only corr-exists-scan,corr-exists-point,ctl-select-pk >> $O/run.log 2>&1
python3 $S/nh_run.py 8 10000 $O/res --only corr-exists-scan,ctl-scan-filter >> $O/run.log 2>&1
date -u '+%Y-%m-%d %H:%M UTC' > $O/start-swap.txt
NH_SWAP=1 python3 $S/nh_run.py 5 1000 $O/res-swap --only corr-exists-scan > $O/run-swap.log 2>&1
date -u '+%Y-%m-%d %H:%M UTC' > $O/end.txt
echo ALLDONE >> $O/run-swap.log
