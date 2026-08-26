#!/usr/bin/env bash
# SS-B5: the demand-conversion cell. `refusal_baseline_probe.py` unchanged
# over the four cells `bench/v2.1.0` §9b measured, five reps each, so the
# two-era counter reads its second era against exactly the first.
set -u
W=/home/ubuntu/ckdbs/.claude/worktrees/workplan-v2.2.0
A=$W/bench/v2.2.0/archive/ssb-v2.2.0-11-g982e133/b5
S=/home/ubuntu/ssb/bin/kds_server
mkdir -p "$A"
port=19000
for cores in 4 8; do
  for sess in 4 8; do
    for rep in 1 2 3 4 5; do
      # do not start beside another worktree's build or test run
      while pgrep -x cc1plus >/dev/null || pgrep -x ld >/dev/null || pgrep -x kds_tests >/dev/null; do sleep 5; done
      rm -rf /home/ubuntu/ssb/b5work
      python3 "$W/bench/refusal_baseline_probe.py" --server "$S" \
        --workdir /home/ubuntu/ssb/b5work --cores "$cores" --sessions "$sess" \
        --tables 6 --rows 200 --port "$port" \
        --json "$A/c${cores}-s${sess}-r${rep}.json" > /dev/null 2>"$A/c${cores}-s${sess}-r${rep}.err"
      echo "c$cores s$sess r$rep rc=$? $(python3 -c "
import json,sys
try:
    d=json.load(open('$A/c${cores}-s${sess}-r${rep}.json'))
    print('attempts',d['attempts'],'accepted',d['accepted'],'refused',d['refused'],'rate',d['refusal_rate'],d['by_class'],'engine',d['engine_counted_total'])
except Exception as e:
    print('NO JSON', e)
")"
      port=$((port+20))
      sleep 2
    done
  done
done
