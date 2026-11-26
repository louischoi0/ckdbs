#!/bin/bash
# Host monitor: one line per sample (about every 2 s), foreign processes above 10 % of one CPU.
# "Foreign" = anything that is not this run's own kds_server, scenario driver or top.
R=/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354
echo $$ > $R/run/monitor.pid
while [ ! -e $R/run/DONE ]; do
  out=$(top -b -c -n2 -d1 -w 400 -o %CPU 2>/dev/null | awk '/^top -/{blk++} blk==2 && $1 ~ /^[0-9]+$/ && $9+0 > 10 {print}' \
      | grep -v "$R/bin/kds_server" | grep -v "scenario[1-4]_" | grep -v " top -b" \
      | awk '{c=$12; for(i=13;i<=NF&&i<=14;i++) c=c" "$i; printf "%s:%s:%s | ", $1, $9, substr(c,1,90)}')
  echo "$(date -u +%FT%TZ) load=$(cut -d' ' -f1 /proc/loadavg) foreign=${out:-none}" >> $R/run/monitor.log
  sleep 1
done
