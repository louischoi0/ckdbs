#!/bin/bash
# Once per 2 s: epoch, loadavg, and every process outside this run that has used
# more than 10 % of a CPU over its life (ps pcpu) or matches a build/test name.
# Stopped with: kill $(cat monitor.pid)
RUN=/home/cdkbs/bench-runs/pg18-f2f1ee7
echo $$ > $RUN/monitor.pid
while true; do
    {
        echo "T $(date +%s) $(cut -d' ' -f1-3 /proc/loadavg)"
        ps -eo pid,pcpu,etimes,comm,args --no-headers | awk '($2+0 > 10.0) || ($5 ~ /ctest|cc1plus|cmake|ninja|make /)' \
            | grep -v "pg18-f2f1ee7\|pg-bench-18\|shell-snapshots\|ps -eo\|awk " | cut -c1-140 | sed 's/^/  X /'
    } >> $RUN/logs/monitor.log
    sleep 2
done
