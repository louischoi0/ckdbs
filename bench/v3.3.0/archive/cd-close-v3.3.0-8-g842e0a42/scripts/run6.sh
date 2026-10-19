#!/bin/bash
# CD close part 6: the pass at 10k, 100k and 1M rows under durability strict and group (rule 4b: both ends of durability).
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
phase p4s $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p4s --port 33121 --cells 4 --sizes 10000,100000,1000000 --reps 2 --durability strict \
  --out $R/out/p4s.json --progress $R/progress.txt
phase p4g $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p4g --port 33122 --cells 4 --sizes 10000,100000,1000000 --reps 2 --durability group \
  --out $R/out/p4g.json --progress $R/progress.txt
touch $R/run6.done; stamp "ALL DONE 6"
