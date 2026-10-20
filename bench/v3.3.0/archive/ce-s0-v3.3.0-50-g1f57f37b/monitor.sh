#!/bin/bash
# 1-second host monitor: one line per sample, foreign processes above 10 % CPU (not this run's own).
RUN=/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b
echo $$ > "$RUN/monitor.pid"
while [ ! -e "$RUN/all.done" ]; do
    out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
        | grep -v "$RUN/" | grep -v "postgres" \
        | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
    echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> "$RUN/monitor.log"
done
