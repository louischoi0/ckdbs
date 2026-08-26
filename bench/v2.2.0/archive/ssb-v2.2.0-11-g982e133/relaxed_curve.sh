#!/usr/bin/env bash
# The shipped curve with the device sync taken out of it.
#
# `sync-relaxed` measured one shipped session at a p50 of 1,091 us where the
# seated one costs 23.6 us, and the `wal_drain_interval_us` sweep ruled the
# reactor's idle block out as the cause. This asks the next question: is the
# ~1.09 ms a cost each *statement* pays serially, or a cost the *core* pays
# once and several sessions share? If throughput rises with S while p50
# holds, several sessions share it.
set -u
W=/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0
A=$W/bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/relaxed-curve
S=/home/ubuntu/ssb/bin/kds_server
mkdir -p "$A"
port=21800
for sess in 1 2 4 8; do
  for seat in foreign owner; do
    while pgrep -x cc1plus >/dev/null || pgrep -x ld >/dev/null || pgrep -x kds_tests >/dev/null; do sleep 5; done
    rm -rf /home/ubuntu/ssb/rx
    extra=""
    if [ "$seat" = foreign ]; then extra="--arrival-core -1"; fi
    python3 "$W/bench/single_relation_probe.py" --server "$S" \
      --workdir /home/ubuntu/ssb/rx --arm multi --cores 4 --sessions "$sess" \
      --relations 1 --rows 1500 --seat "$seat" $extra --durability relaxed \
      --port "$port" --json "$A/s${sess}-${seat}.json" > /dev/null 2>&1
    echo "relaxed $seat S=$sess $(python3 -c "
import json
d=json.load(open('$A/s${sess}-${seat}.json'))
print('ips',d['inserts_per_second'],'p0',d['insert_p0_us'],'p25',d['insert_p25_us'],'p50',d['insert_p50_us'],'p99',d['insert_p99_us'],'exec',d['executed'],'ref',d['refused'],'cpu',d['cpu_busy'])
")"
    port=$((port+8))
    sleep 2
  done
done
