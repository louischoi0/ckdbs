#!/bin/bash
# CD close part 2: the pass cells re-run with the final cd_pass.py (SHOW META deltas around each pass, the
# stall timeline of cell 5, and a no-pass control for cell 5). Resumable; watch2.sh restarts it if it dies.
R=/home/cdkbs/bench-runs/cd-close
S=/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/bench/v3.3.0/archive/cd-close-v3.3.0-8-g842e0a42/scripts
B=$R/bin
stamp() { echo "$(date -u +%T) $1" >> $R/progress.txt; }
phase() { local name=$1; shift
  [ -f $R/flags/$name.done ] && { stamp "skip $name"; return 0; }
  stamp "phase start $name"; rm -rf $R/wd-$name; mkdir -p $R/wd-$name
  python3 "$@" > $R/out/$name.log 2>&1 & echo $! > $R/flags/$name.pid; echo $name > $R/flags/current
  wait $!; local rc=$?; stamp "phase end $name rc=$rc"; [ $rc -eq 0 ] && touch $R/flags/$name.done; }
phase p4b $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p4b --port 33071 --cells 4 --reps 2 \
  --out $R/out/p4b.json --progress $R/progress.txt
phase p5b $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p5b --port 33072 --cells 5 --reps 3 --stream-rows 1000000 \
  --out $R/out/p5b.json --progress $R/progress.txt
phase p5c $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p5c --port 33073 --cells 5 --reps 3 --stream-rows 1000000 --control \
  --out $R/out/p5c.json --progress $R/progress.txt
touch $R/run2.done; stamp "ALL DONE 2"
