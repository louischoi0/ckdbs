#!/bin/bash
# CD close part 5: cell 6 again with the per-trial vectors saved (100 trials each).
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
phase p6b $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p6b --port 33111 --cells 6 --add-trials 100 \
  --out $R/out/p6b.json --progress $R/progress.txt
touch $R/run5.done; stamp "ALL DONE 5"
