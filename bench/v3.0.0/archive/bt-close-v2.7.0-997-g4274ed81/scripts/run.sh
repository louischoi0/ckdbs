#!/bin/bash
# BT-S5 close measurement: A (eeb88f47) vs B (4274ed81), A' = A's bytes as a second server.
# Phases are resumable: a phase whose flag file exists is skipped, so re-running this script after an
# orphaned driver resumes at the unfinished phase (which restarts from a clean workdir).
R=/home/cdkbs/bench-runs/bt-close
W=/home/cdkbs/ckdbs/.claude/worktrees/bt-accept-tuple-row
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
QW=${QW:-30}
mkdir -p $R/out $R/flags
echo $$ > $R/run.pid
rm -f $R/run.done

stamp() { echo "$1 $(date -u +%T)" >> $R/progress; }

# host watch: loadavg and competing build/test count every 10 s
if [ ! -f $R/hostwatch.pid ] || ! kill -0 "$(cat $R/hostwatch.pid)" 2>/dev/null; then
  ( while [ ! -f $R/run.done ]; do
      echo "$(date -u +%T) $(cat /proc/loadavg) competing=$(pgrep -c -f 'cc1plus|cmake --build|ctest')" >> $R/out/hostwatch.log
      sleep 10
    done ) &
  echo $! > $R/hostwatch.pid
fi

# phase NAME LOGBASE CMD... : runs CMD (a python driver) in the background, records its pid, waits.
phase() {
  local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name (done)"; return 0; }
  stamp "start $name"
  rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 &
  echo $! > $R/flags/$name.pid
  echo "$name" > $R/flags/current
  wait $!
  local rc=$?
  stamp "end $name rc=$rc"
  [ $rc -eq 0 ] && touch $R/flags/$name.done
}

# ---- P1: scan row, bn + wide-h, order A A2 B ----
phase p1b $W/tools/bm_scan.py census --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-p1b --port 32811 --runs 4 --pin-server 2 --pin-driver 4 --cells bn,wide-h --scan-ops 192 --point-ops 3000 \
  --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p1b.json

# ---- P2: every-row UPDATE + point SELECT, 1000 rows, order A A2 B ----
phase p2 $W/tools/bn_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-p2 --port 32821 --runs 5 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
  --only select-pk,update-range --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p2.json

# ---- P3: same, server start order reversed: B A2 A ----
phase p3 $W/tools/bn_census.py --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-p3 --port 32831 --runs 5 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
  --only select-pk,update-range --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p3.json

# ---- P4: scan row, bn + wide-h, start order reversed: B A2 A ----
phase p4 $W/tools/bm_scan.py census --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-p4 --port 32841 --runs 3 --pin-server 2 --pin-driver 4 --cells bn,wide-h --scan-ops 192 --point-ops 3000 \
  --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p4.json

# ---- P5: sizes 200 and 1,000 rows of the scan row ----
phase p5 $W/tools/bm_scan.py census --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-p5 --port 32851 --runs 4 --pin-server 2 --pin-driver 4 --cells rows-200,rows-1k --scan-ops 192 --point-ops 3000 \
  --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p5.json

# ---- P6: every-row UPDATE + point SELECT at 200 and 10,000 rows ----
phase p6 $W/tools/bn_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-p6 --port 32861 --runs 3 --pin-server 2 --pin-driver 4 --rows 200 --scan-rows 200 --ops 3000 --scan-ops 800 --range-ops 600 --blocks 12 \
  --only select-pk,update-range --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p6.json
phase p7 $W/tools/bn_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-p7 --port 32871 --runs 3 --pin-server 2 --pin-driver 4 --rows 10000 --scan-rows 10000 --ops 3000 --scan-ops 200 --range-ops 60 --blocks 12 \
  --only select-pk,update-range --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p7.json

stamp "ALL DONE"
touch $R/run.done
