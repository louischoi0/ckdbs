#!/usr/bin/env bash
# The other side of the drain sweep, and the one that discriminates.
#
# 1000, 500, 200 and 50 us all round *up* to the same 1 ms `epoll_wait`
# timeout (`IdleTimeoutMs`, sched/scheduler.cpp:210), so that sweep could
# not have moved the constant whatever caused it. Values **above** a
# millisecond can: at 3000 us the idle block becomes 3 ms and at 5000 us it
# becomes 5. If the shipped statement's ~1.09 ms is the owner's idle block,
# the p50 follows this up.
set -u
W=/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0
A=$W/bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/drain
S=/home/ubuntu/ssb/bin/kds_server
mkdir -p "$A"
port=22200
for iv in 2000 3000 5000; do
  for seat in owner foreign; do
    for rep in 1 2 3; do
      while pgrep -x cc1plus >/dev/null || pgrep -x ld >/dev/null || pgrep -x kds_tests >/dev/null; do sleep 5; done
      rm -rf /home/ubuntu/ssb/drainwork2
      extra=""
      if [ "$seat" = foreign ]; then extra="--arrival-core -1"; fi
      python3 "$W/bench/single_relation_probe.py" --server "$S" \
        --workdir /home/ubuntu/ssb/drainwork2 --arm multi --cores 4 --sessions 1 \
        --relations 1 --rows 600 --seat "$seat" $extra \
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
