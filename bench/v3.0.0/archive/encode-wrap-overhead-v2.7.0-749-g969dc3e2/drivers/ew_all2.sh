#!/bin/bash
# Amplification passes (ew_bulk.py): same four passes as ew_all.sh.
cd /home/cdkbs/bench-runs/ew
export EW_DRV=/home/cdkbs/bench-runs/ew/ew_bulk.py
date -u '+%F %T' > start-bulk.txt
python3 ew_run.py 8 10000,1000,200 /home/cdkbs/bench-runs/ew/res-bulk > run-bulk-main.log 2>&1
EW_SWAP=1 python3 ew_run.py 5 10000,1000 /home/cdkbs/bench-runs/ew/res-bulk-swap > run-bulk-swap.log 2>&1
EW_BINB=C python3 ew_run.py 8 10000,1000 /home/cdkbs/bench-runs/ew/res-bulk-ctl > run-bulk-ctl.log 2>&1
EW_BINB=C EW_SWAP=1 python3 ew_run.py 5 10000,1000 /home/cdkbs/bench-runs/ew/res-bulk-ctl-swap > run-bulk-ctl-swap.log 2>&1
date -u '+%F %T' > end-bulk.txt
echo ALLDONE > all2.done
