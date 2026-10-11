#!/bin/bash
cd /home/cdkbs/bench-runs/ew
date -u '+%F %T' > start-main.txt
python3 ew_run.py 8 10000,1000,200 /home/cdkbs/bench-runs/ew/res > run-main.log 2>&1
date -u '+%F %T' > end-main.txt
EW_SWAP=1 python3 ew_run.py 5 10000,1000 /home/cdkbs/bench-runs/ew/res-swap > run-swap.log 2>&1
date -u '+%F %T' > end-swap.txt
EW_BINB=C python3 ew_run.py 8 10000,1000 /home/cdkbs/bench-runs/ew/res-ctl > run-ctl.log 2>&1
EW_BINB=C EW_SWAP=1 python3 ew_run.py 5 10000,1000 /home/cdkbs/bench-runs/ew/res-ctl-swap > run-ctl-swap.log 2>&1
date -u '+%F %T' > end-ctl.txt
echo ALLDONE > all.done
