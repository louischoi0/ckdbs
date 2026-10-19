#!/bin/bash
# CD close part 4: 12 more runs of the every-row UPDATE and the dropped-slot INSERT (the first 6 runs left the UPDATE bimodal, +-90 us per server instance).
# Phases are resumable (flag files); the watchdog (watch.sh) restarts this script if it dies.
R=/home/cdkbs/bench-runs/cd-close
W=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning
S=$W/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
B=$R/bin
GATE="--cell-quiet-pct 10 --cell-quiet-wait 30 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
CENSUS="--runs 6 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 --only insert,insert-dropped,update-range --quiet-load 2 --quiet-wait 30"
phase u3 $S/cd_census.py --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B --arm A=$B/kds_server-A \
  --workdir $R/wd-u3 --port 33091 $CENSUS $GATE --out $R/out/u3.json
phase u4 $S/cd_census.py --arm B=$B/kds_server-B --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 \
  --workdir $R/wd-u4 --port 33101 $CENSUS $GATE --out $R/out/u4.json
touch $R/run4.done; stamp "ALL DONE 4"
