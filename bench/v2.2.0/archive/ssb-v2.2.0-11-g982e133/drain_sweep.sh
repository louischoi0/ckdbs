#!/usr/bin/env bash
# Does the shipped round trip's ~1 ms track the idle reactor's block?
#
# `IdleTimeoutMs` caps the 10 ms idle block at the next timer's deadline
# (sched/scheduler.cpp:196-214) and the WAL drain timer is the only timer
# armed at this cadence, so `wal_drain_interval_us` *is* how long a core
# with nothing to do sleeps before it next polls its ring inbox. If the
# shipped statement's cost at one session is that sleep, the p50 follows
# this knob down. Both arms run at every point; `relaxed` keeps the device
# out of the comparison.
set -u
W=/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0
A=$W/bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/drain
S=/home/ubuntu/ssb/bin/kds_server
mkdir -p "$A"
port=21400
for iv in 1000 500 200 50; do
  for seat in owner foreign; do
    for rep in 1 2 3; do
      while pgrep -x cc1plus >/dev/null || pgrep -x ld >/dev/null || pgrep -x kds_tests >/dev/null; do sleep 5; done
      rm -rf /home/ubuntu/ssb/drainwork
      extra=""
      if [ "$seat" = foreign ]; then extra="--arrival-core -1"; fi
      python3 "$W/bench/single_relation_probe.py" --server "$S" \
        --workdir /home/ubuntu/ssb/drainwork --arm multi --cores 4 --sessions 1 \
        --relations 1 --rows 2000 --seat "$seat" $extra \
        --durability relaxed --wal-drain-interval-us "$iv" --port "$port" \
        --json "$A/iv${iv}-${seat}-r${rep}.json" > /dev/null 2>&1
      echo "iv=$iv $seat r$rep $(python3 -c "
import json
d=json.load(open('$A/iv${iv}-${seat}-r${rep}.json'))
print('ips',d['inserts_per_second'],'p0',d['insert_p0_us'],'p25',d['insert_p25_us'],'p50',d['insert_p50_us'],'p95',d['insert_p95_us'],'p99',d['insert_p99_us'],'exec',d['executed'],'ref',d['refused'])
")"
      port=$((port+8))
      sleep 2
    done
  done
done
