#!/bin/bash
# 1-second monitor and 60 s watchdog for the row-cost re-run; both end at rerun.done.
RC=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost
RUN=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
(
while [ ! -e "$RC/rerun.done" ]; do
    out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
        | grep -v "$RUN/kds_server" | grep -v "scenario[1-4]_" | grep -v "bench-runs/v3.3.0-38-ga40882af" | grep -v "postgres" \
        | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
    echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> "$RC/monitor-rerun.log"
    sleep 1
done
) &
while [ ! -e "$RC/rerun.done" ]; do
    now=$(date +%s)
    mp=$(cat $RC/master-rerun.pid 2>/dev/null)
    pa=$(( now - $(stat -c %Y $RC/progress 2>/dev/null || echo $now) ))
    echo "$(date -u +%FT%TZ) master=${mp:-none} alive=$([ -n "$mp" ] && kill -0 "$mp" 2>/dev/null && echo y || echo n) progress_age=${pa}s $(cat $RC/progress $RC/progress-send 2>/dev/null | tail -1 | cut -d' ' -f2-)" >> $RC/watchdog-rerun.log
    if [ -z "$mp" ] || ! kill -0 "$mp" 2>/dev/null; then
        echo "$(date -u +%FT%TZ) ORPHAN: restarting" >> $RC/watchdog-rerun.log
        setsid nohup bash $RC/rerun_rc.sh >> $RC/master-rerun.out 2>&1 < /dev/null &
    fi
    sleep 60
done
