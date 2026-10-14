#!/bin/bash
# BW close: A=74b0a3dd, B=541ce440, A2=A's bytes as second server. Phases resumable (flag files).
R=/home/cdkbs/bench-runs/bw-close
W=/home/cdkbs/ckdbs/.claude/worktrees/bw-instant-add-drop-column
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
phase s1 $W/tools/bm_scan.py census --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-s1 --port 32811 --runs 3 --blocks 6 --pin-server 2 --pin-driver 4 --cells bn,wide-h --scan-ops 96 --point-ops 2000 \
  --quiet-load 2 --quiet-wait 30 $GATE --out $R/out/s1.json
phase u1 $W/tools/bn_census.py --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-u1 --port 32821 --runs 3 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 \
  --only select-pk,insert,update-range --quiet-load 2 --quiet-wait 30 $GATE --out $R/out/u1.json
touch $R/run.done; stamp "ALL DONE"
