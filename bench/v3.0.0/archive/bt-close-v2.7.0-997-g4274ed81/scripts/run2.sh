#!/bin/bash
# BT-S5 close, second part: two more batches taken after the host went quiet (load < 1).
# Waits for run.sh to finish (run.done), then runs p8 and p9.  Resumable like run.sh.
R=/home/cdkbs/bench-runs/bt-close
W=/home/cdkbs/ckdbs/.claude/worktrees/bt-accept-tuple-row
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
QW=${QW:-30}
mkdir -p $R/out $R/flags
echo $$ > $R/run2.pid
while [ ! -f $R/run.done ]; do sleep 2; done
rm -f $R/run2.done

stamp() { echo "$1 $(date -u +%T)" >> $R/progress; }

( while [ ! -f $R/run2.done ]; do
    echo "$(date -u +%T) $(cat /proc/loadavg) competing=$(pgrep -c -f 'cc1plus|cmake --build|ctest')" >> $R/out/hostwatch.log
    sleep 10
  done ) &
echo $! > $R/hostwatch2.pid

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

# ---- P8: scan row, bn + wide-h, order A A2 B again, five more runs ----
phase p8 $W/tools/bm_scan.py census --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-p8 --port 32881 --runs 5 --pin-server 2 --pin-driver 4 --cells bn,wide-h --scan-ops 192 --point-ops 3000 \
  --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p8.json

# ---- P9: every-row UPDATE + point SELECT, 1000 rows, order B A A2, five more runs ----
phase p9 $W/tools/bn_census.py --arm B=$B/kds_server-B --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 \
  --workdir $R/wd-p9 --port 32891 --runs 5 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
  --only select-pk,update-range --quiet-load 2 --quiet-wait $QW $GATE --out $R/out/p9.json

stamp "ALL DONE 2"
touch $R/run2.done
