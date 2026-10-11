#!/bin/bash
# Re-run, with a quiet-host gate (EW_QWAIT), every amplification-pass run that started or ran beside another
# CPU above 0.5 busy; rejected runs are kept under <pass>-excluded/. Rounds repeat until none is flagged.
cd /home/cdkbs/bench-runs/ew
export EW_DRV=/home/cdkbs/bench-runs/ew/ew_bulk.py EW_QWAIT=120
date -u '+%F %T' > start-bulk-rerun.txt
for round in 1 2 3 4 5 6; do
  tot=0
  for p in res-bulk res-bulk-swap res-bulk-ctl res-bulk-ctl-swap; do
    m=$(python3 flag.py $p | awk '{print $3}')
    tot=$((tot + m))
  done
  echo "round $round moved $tot" >> rerun.log
  if [ "$tot" = "0" ]; then break; fi
  python3 ew_run.py 8 10000,1000,200 /home/cdkbs/bench-runs/ew/res-bulk >> run-bulk-main.log 2>&1
  EW_SWAP=1 python3 ew_run.py 5 10000,1000 /home/cdkbs/bench-runs/ew/res-bulk-swap >> run-bulk-swap.log 2>&1
  EW_BINB=C python3 ew_run.py 8 10000,1000 /home/cdkbs/bench-runs/ew/res-bulk-ctl >> run-bulk-ctl.log 2>&1
  EW_BINB=C EW_SWAP=1 python3 ew_run.py 5 10000,1000 /home/cdkbs/bench-runs/ew/res-bulk-ctl-swap >> run-bulk-ctl-swap.log 2>&1
done
date -u '+%F %T' > end-bulk-rerun.txt
echo ALLDONE > all3.done
