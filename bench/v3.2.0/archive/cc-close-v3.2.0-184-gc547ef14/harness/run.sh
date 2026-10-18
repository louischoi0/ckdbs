#!/bin/bash
# one launch of the resumable harness; writes run.done only when it exits 0
R=/home/cdkbs/bench-runs/cc-close
mkdir -p $R/out
rm -f $R/out/run.done
echo $$ > $R/out/run.pid
taskset -c 3 python3 $R/harness/cc_close_run.py >> $R/out/run.log 2>&1
rc=$?
echo "rc=$rc $(date -u +%FT%TZ)" >> $R/out/run.log
[ $rc -eq 0 ] && echo done > $R/out/run.done
exit $rc
