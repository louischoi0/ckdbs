#!/bin/bash
# 1-second host monitor for the row-cost sweep: foreign processes above 10 % CPU.
RUN=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
echo $$ > "$RUN/monitor4.pid"
while [ ! -e "$RUN/rowcost/all.done" ]; do
    out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
        | grep -v "$RUN/kds_server" | grep -v "scenario[1-4]_" | grep -v "bench-runs/v3.3.0-38-ga40882af" | grep -v "postgres" \
        | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
    echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> "$RUN/rowcost/monitor.log"
    sleep 1
done
