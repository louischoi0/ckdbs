#!/bin/bash
# CD close: A=f3ec25ea (v3.3.0), B=842e0a42 (v3.3.0-8-g842e0a42), A2 = A's bytes as a second server.
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
CENSUS="--runs 3 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 2000 --range-ops 120 --blocks 6 --only select-pk,insert,insert-plain,insert-spare,insert-dropped,update-range --quiet-load 2 --quiet-wait 30"
phase u1 $S/cd_census.py --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-u1 --port 33021 $CENSUS $GATE --out $R/out/u1.json
phase u2 $S/cd_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-u2 --port 33031 $CENSUS $GATE --out $R/out/u2.json
phase p4 $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p4 --port 33041 --cells 4 --reps 2 \
  --out $R/out/p4.json --progress $R/progress.txt
phase p5 $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p5 --port 33042 --cells 5 --reps 3 --stream-rows 1000000 \
  --out $R/out/p5.json --progress $R/progress.txt
phase p6 $S/cd_pass.py --bin $B/kds_server-B --workdir $R/wd-p6 --port 33043 --cells 6 --add-trials 60 \
  --out $R/out/p6.json --progress $R/progress.txt
phase s1 $W/tools/bm_scan.py census --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
  --workdir $R/wd-s1 --port 33051 --runs 3 --blocks 6 --pin-server 2 --pin-driver 4 --cells bn --scan-ops 96 --point-ops 2000 \
  --quiet-load 2 --quiet-wait 30 $GATE --out $R/out/s1.json
phase z $W/tools/bm_scan.py census --arm B=$B/kds_server-B --arm A2=$B/kds_server-A2 --arm A=$B/kds_server-A \
  --workdir $R/wd-z --port 33061 --runs 2 --blocks 6 --pin-server 2 --pin-driver 4 --cells rows-200,rows-1k --scan-ops 96 --point-ops 2000 \
  --quiet-load 2 --quiet-wait 30 $GATE --out $R/out/z.json
touch $R/run.done; stamp "ALL DONE"
