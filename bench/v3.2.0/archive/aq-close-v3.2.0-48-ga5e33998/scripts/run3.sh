#!/bin/bash
R=/home/cdkbs/bench-runs/aq-close
W=/home/cdkbs/ckdbs/.claude/worktrees/aq-expression-cabins
B=$R/bin
CELLGATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
COMMON="--pin-server 2 --pin-driver 4 --scan-rows 1000 --blocks 6 --quiet-load 2 --quiet-wait 30 $CELLGATE"
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
BN="$W/tools/bn_census.py"
phase f1 $BN --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B --arm Bp=$B/kds_server-Bp \
  --workdir $R/wd-f1 --port 33001 --runs 4 --rows 1000 --ops 2000 --range-ops 120 \
  --only select-pk,update-range $COMMON --out $R/out/f1.json
phase f2 $BN --arm Bp=$B/kds_server-Bp --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-f2 --port 33011 --runs 4 --rows 1000 --ops 2000 --range-ops 120 \
  --only select-pk,update-range $COMMON --out $R/out/f2.json
touch $R/run3.done; stamp "ALL DONE 3"
