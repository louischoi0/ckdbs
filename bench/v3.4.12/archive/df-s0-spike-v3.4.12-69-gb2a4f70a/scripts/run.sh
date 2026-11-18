#!/bin/bash
R=/home/louis/bench-runs/df-s0
T=/home/louis/ckdbs/.claude/worktrees/da-q8-memory-bound-key/tools
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
stamp() { echo "$(date -u '+%F %T UTC') $1" >> $R/progress.txt; }
C="--runs 6 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 --only select-pk,update-expr --quiet-load 2 --quiet-wait 30"
run() { local name=$1 port=$2; shift 2
  stamp "phase start $name order $* (load $(cut -d' ' -f1-3 /proc/loadavg), cc1plus $(pgrep -c cc1plus))"
  rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  local args=""; for o in "$@"; do args="$args --arm $o=$B/w-$o"; done
  python3 $T/bn_census.py $args --workdir $R/wd-$name --port $port $C $GATE --out $R/out/$name.json > $R/out/$name.log 2>&1
  local rc=$?
  stamp "phase end $name rc=$rc"
  rm -rf $R/wd-$name
}
run w1 34021 A A2 B C
run w2 34031 B C A A2
run w3 34041 A2 C B A
run w4 34051 C B A2 A
stamp DONE
