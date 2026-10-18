#!/bin/bash
R=/home/cdkbs/bench-runs/cc-close
W=$R/out/watch-extra.log
while [ ! -e $R/out/extra.done ]; do
    now=$(date +%s); m=$(stat -c %Y $R/out/run.status 2>/dev/null || echo 0); age=$((now-m))
    pid=$(cat $R/out/extra.pid 2>/dev/null); alive=dead; kill -0 $pid 2>/dev/null && alive=alive
    echo "$(date -u +%FT%TZ) status_age=${age}s harness=$alive load=$(cut -d' ' -f1 /proc/loadavg) :: $(cut -d' ' -f2- $R/out/run.status 2>/dev/null)" >> $W
    if [ ! -e $R/out/extra.done ] && { [ "$alive" = dead ] || [ $age -gt 180 ]; }; then
        echo "$(date -u +%FT%TZ) ORPHAN (alive=$alive age=$age): relaunching" >> $W
        for p in $(ps -eo pid,args | grep -E "[k]ds_server_(A|B) --config $R/run/" | awk '{print $1}'); do kill -TERM $p; done
        [ "$alive" = alive ] && kill -TERM $pid 2>/dev/null
        sleep 5
        nohup $R/harness/run_extra.sh > /dev/null 2>&1 &
    fi
    sleep 60
done
echo "$(date -u +%FT%TZ) finished" >> $W
