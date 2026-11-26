#!/bin/bash
# Scenario 4 run A again (tag r2-s4): the first run A (s4, 21:15:10 to 21:19:43 UTC) overlapped
# another session's full build for 43 of its 123 monitor samples. Same servers, config, driver
# arguments and binary. Waits for run_pin.sh's DONE2. Resumable.
R=/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354
T=$R/src/tools
OUT=$R/out
NEW=$R/bin/kds_server
OLD=$R/bin/kds_server_old
STATUS=$R/run/status
export PYTHONPATH=$T
while [ ! -e $R/run/DONE2 ]; do sleep 2; done
echo $$ > $R/run/runner3.pid
eval "$(sed -n '/^st() {/,/^sat2() {/p' $R/run.sh | sed '$d')"

( while [ ! -e $R/run/DONE3 ]; do
    out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
        | grep -v "$R/bin/kds_server" | grep -v "scenario[1-4]_" | grep -v " top -b" \
        | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
    echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> $R/run/monitor.log
    sleep 1
  done ) &
MON=$!

st "runner3 $$ start (scenario 4 run A again)"
s4run r2-s4 $NEW
st "runner3 $$ DONE"
touch $R/run/DONE3
kill $MON 2>/dev/null
