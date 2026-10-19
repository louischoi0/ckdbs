#!/bin/bash
# CD close part 3: the reverse-start-order scan run (cell 3), 4 runs.
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
phase s2 $W/tools/bm_scan.py census --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-s2 --port 33081 --runs 4 --blocks 6 --pin-server 2 --pin-driver 4 --cells bn --scan-ops 96 --point-ops 2000 \
  --quiet-load 2 --quiet-wait 30 $GATE --out $R/out/s2.json
touch $R/run3.done; stamp "ALL DONE 3"
