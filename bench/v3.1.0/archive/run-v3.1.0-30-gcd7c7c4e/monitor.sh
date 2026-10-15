#!/bin/bash
# 2-second host monitor: one line per sample, foreign processes above 10 % CPU.
# "Foreign" = anything that is not this run's own kds_server / scenario driver.
RUN=/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e
echo $$ > "$RUN/monitor.pid"
while [ ! -e "$RUN/all.done" ]; do
    out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
        | grep -v "$RUN/kds_server" | grep -v "scenario[1-4]_" | grep -v "bench-runs/v3.1.0-30-gcd7c7c4e" \
        | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
    echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> "$RUN/monitor.log"
    sleep 1
done
