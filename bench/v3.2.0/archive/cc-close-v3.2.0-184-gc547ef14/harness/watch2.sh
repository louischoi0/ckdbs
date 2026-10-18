#!/bin/bash
# every 60 s: log the status file's age; if the harness process is gone without run.done, or the status is stale > 180 s,
# stop leftover servers and relaunch (the harness resumes from its finished blocks).
R=/home/cdkbs/bench-runs/cc-close
W=$R/out2/watch.log
while [ ! -e $R/out2/run.done ]; do
    now=$(date +%s); m=$(stat -c %Y $R/out2/run.status 2>/dev/null || echo 0); age=$((now-m))
    pid=$(cat $R/out2/run.pid 2>/dev/null); alive=dead; kill -0 $pid 2>/dev/null && alive=alive
    echo "$(date -u +%FT%TZ) status_age=${age}s harness=$alive load=$(cut -d' ' -f1 /proc/loadavg) :: $(cut -d' ' -f2- $R/out2/run.status 2>/dev/null)" >> $W
    if [ ! -e $R/out2/run.done ] && { [ "$alive" = dead ] || [ $age -gt 180 ]; }; then
        echo "$(date -u +%FT%TZ) ORPHAN (alive=$alive age=$age): relaunching" >> $W
        for p in $(ps -eo pid,args | grep -E "[k]ds_server_(A|B) --config $R/run/" | awk '{print $1}'); do kill -TERM $p; done
        [ "$alive" = alive ] && kill -TERM $pid 2>/dev/null
        sleep 5
        nohup $R/harness/run2.sh > /dev/null 2>&1 &
    fi
    sleep 60
done
echo "$(date -u +%FT%TZ) finished" >> $W
