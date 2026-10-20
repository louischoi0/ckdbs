#!/bin/bash
# 60 s watchdog for the row-cost sweep: polls the progress file's mtime and the master pid; resumes an orphan at once.
RC=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost
echo $$ > $RC/watchdog-send.pid
while [ ! -e $RC/all-send.done ]; do
    now=$(date +%s)
    mp=$(cat $RC/master-send.pid 2>/dev/null)
    pa=$(( now - $(stat -c %Y $RC/progress-send 2>/dev/null || echo $now) ))
    echo "$(date -u +%FT%TZ) master=${mp:-none} alive=$([ -n "$mp" ] && kill -0 "$mp" 2>/dev/null && echo y || echo n) progress_age=${pa}s $(cat $RC/progress-send 2>/dev/null | cut -d' ' -f2-)" >> $RC/watchdog-send.log
    if [ -z "$mp" ] || ! kill -0 "$mp" 2>/dev/null; then
        echo "$(date -u +%FT%TZ) ORPHAN: restarting" >> $RC/watchdog-send.log
        setsid nohup bash $RC/run_send.sh >> $RC/master-send.out 2>&1 < /dev/null &
    elif [ "$pa" -gt 600 ]; then
        echo "$(date -u +%FT%TZ) STALL: restarting" >> $RC/watchdog-send.log
        kill -TERM -- "-$mp" 2>/dev/null; sleep 3; kill -KILL -- "-$mp" 2>/dev/null
        setsid nohup bash $RC/run_send.sh >> $RC/master-send.out 2>&1 < /dev/null &
    fi
    sleep 60
done
