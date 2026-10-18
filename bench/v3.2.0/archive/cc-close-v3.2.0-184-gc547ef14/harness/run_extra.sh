#!/bin/bash
# sixth extra batch: same swapped plan on both binaries, with and without the overhead tables
R=/home/cdkbs/bench-runs/cc-close
rm -f $R/out/extra.done
echo $$ > $R/out/extra.pid
echo "$(date -u +%FT%TZ) gain4 launch" > $R/out/run.status
rm -f $R/out/gain4.json
taskset -c 3 python3 $R/harness/gain4.py >> $R/out/extra6.log 2>&1
rc=$?
echo "rc=$rc $(date -u +%FT%TZ)" >> $R/out/extra6.log
[ $rc -eq 0 ] && echo done > $R/out/extra.done
exit $rc
