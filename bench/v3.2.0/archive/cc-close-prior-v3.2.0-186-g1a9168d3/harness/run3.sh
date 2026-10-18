#!/bin/bash
R=/home/cdkbs/bench-runs/cc-prior
echo $$ > $R/out/run.pid
taskset -c 3 python3 $R/harness/gain_order.py >> $R/out/run.log 2>&1
rc=$?
echo "rc=$rc $(date -u +%FT%TZ)" >> $R/out/run.log
exit $rc
