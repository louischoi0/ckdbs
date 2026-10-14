#!/bin/bash
R=/home/cdkbs/bench-runs/bw-s6
W=/home/cdkbs/ckdbs/.claude/worktrees/bw-instant-add-drop-column
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
COMMON="--pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --blocks 6 --quiet-load 2 --quiet-wait 20 $GATE"
phase u1 $W/tools/bn_census.py --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-u1 --port 32921 --runs 4 --only select-pk,insert $COMMON --out $R/out/u1.json
phase u2 $W/tools/bn_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-u2 --port 32931 --runs 4 --only select-pk,insert $COMMON --out $R/out/u2.json
phase l1 $W/tools/bn_census.py --arm B=$B/kds_server-B \
  --workdir $R/wd-l1 --port 32941 --runs 4 --only insert,insert-listed $COMMON --out $R/out/l1.json
touch $R/run.done; stamp "ALL DONE"
