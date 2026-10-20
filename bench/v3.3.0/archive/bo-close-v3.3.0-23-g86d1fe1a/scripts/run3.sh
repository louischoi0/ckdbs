#!/bin/bash
R=/home/cdkbs/bench-runs/bo-close
T=/home/cdkbs/ckdbs/.claude/worktrees/bo-relation-stats-checkpoints/tools
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
stamp() { echo "$(date -u '+%F %T UTC') $1" >> $R/progress.txt; }
free_kb() { df --output=avail / | tail -1; }
C="--runs 6 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 --only select-pk,insert,delete-pk --quiet-load 2 --quiet-wait 30"
run() { local name=$1 o1=$2 o2=$3 o3=$4 port=$5
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return; }
  if [ $(free_kb) -lt 2097152 ]; then stamp "STOPPED free space under 2 GB before $name"; exit 1; fi
  stamp "phase start $name order $o1,$o2,$o3 (load $(cut -d' ' -f1-3 /proc/loadavg), cc1plus $(pgrep -c cc1plus), free $(free_kb) KB)"
  rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 $T/bn_census.py --arm $o1=$B/kds_server-$o1 --arm $o2=$B/kds_server-$o2 --arm $o3=$B/kds_server-$o3 \
     --workdir $R/wd-$name --port $port $C $GATE --out $R/out/$name.json > $R/out/$name.log 2>&1
  local rc=$?
  stamp "phase end $name rc=$rc (data dir $(du -sm $R/wd-$name | cut -f1) MB, free $(free_kb) KB)"
  rm -rf $R/wd-$name
  if [ $rc -eq 0 ]; then touch $R/flags/$name.done; else stamp "STOPPED $name rc=$rc"; exit 1; fi
}

run w5 A A2 B 34161
run w7 A2 B A 34181
stamp DONE
