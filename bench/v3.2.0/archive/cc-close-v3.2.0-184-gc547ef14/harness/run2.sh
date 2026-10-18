#!/bin/bash
# one launch of the resumable harness; writes run.done only when it exits 0
R=/home/cdkbs/bench-runs/cc-close
mkdir -p $R/out2
rm -f $R/out2/run.done
echo $$ > $R/out2/run.pid
CC_OUT=$R/out2 taskset -c 3 python3 $R/harness/cc_close_run.py >> $R/out2/run.log 2>&1
rc=$?
echo "rc=$rc $(date -u +%FT%TZ)" >> $R/out2/run.log
[ $rc -eq 0 ] && echo done > $R/out2/run.done
exit $rc
