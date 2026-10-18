#!/bin/bash
# every 60 s: log status age; relaunch if the harness is gone without run.done or status stale > 120 s
R=/home/cdkbs/bench-runs/cc-prior
while [ ! -e $R/out/run.done ]; do
    now=$(date +%s); m=$(stat -c %Y $R/out/run.status 2>/dev/null || echo 0); age=$((now-m))
    pid=$(cat $R/out/run.pid 2>/dev/null); alive=dead; kill -0 $pid 2>/dev/null && alive=alive
    echo "$(date -u +%FT%TZ) status_age=${age}s harness=$alive load=$(cut -d' ' -f1 /proc/loadavg) :: $(cut -d' ' -f2- $R/out/run.status 2>/dev/null)" >> $R/out/watch.log
    if [ ! -e $R/out/run.done ] && { [ "$alive" = dead ] || [ $age -gt 120 ]; }; then
        echo "$(date -u +%FT%TZ) ORPHAN (alive=$alive age=$age): relaunching" >> $R/out/watch.log
        for p in $(ps -eo pid,args | grep -E "[k]ds_server_(A|B) --config $R/run/" | awk '{print $1}'); do kill -TERM $p; done
        [ "$alive" = alive ] && kill -TERM $pid 2>/dev/null
        sleep 3
        nohup $R/harness/run.sh > /dev/null 2>&1 &
    fi
    sleep 60
done
echo "$(date -u +%FT%TZ) finished" >> $R/out/watch.log
