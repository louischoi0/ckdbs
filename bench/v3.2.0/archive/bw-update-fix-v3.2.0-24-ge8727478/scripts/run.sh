#!/bin/bash
# BW update fix: A0=74b0a3dd, A0'=A0 bytes, B=541ce440, C=e8727478. Two start orders. Resumable by flag files.
R=/home/cdkbs/bench-runs/bw-update-fix
W=/home/cdkbs/ckdbs/.claude/worktrees/bw-instant-add-drop-column
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
U="--rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 --pin-server 2 --pin-driver 4 --only select-pk,update-range --quiet-load 2 --quiet-wait 30 $GATE"
phase u1 $W/tools/bn_census.py --arm C=$B/kds_server-C --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A --workdir $R/wd-u1 --port 32831 --runs 5 $U --out $R/out/u1.json
phase u2 $W/tools/bn_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B --arm C=$B/kds_server-C --workdir $R/wd-u2 --port 32841 --runs 5 $U --out $R/out/u2.json
touch $R/run.done; stamp "ALL DONE"
