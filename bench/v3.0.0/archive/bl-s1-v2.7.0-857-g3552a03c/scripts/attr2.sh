#!/bin/sh
# BL-S1 attribution, second set: Waystone recording off throughout; where the
# sessions are placed. Two runs each, interleaved. Usage: attr2.sh OUTDIR
OUT=$1; mkdir -p "$OUT"
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bl-session-balancer/tools
M="python3 $TOOLS/bl_mixed.py --bin /home/cdkbs/bench-runs/bl/bin/kds_server.bls3 --port 17482 --cores 4 --arm mixed --readers 6 --rate 400 --seconds 10 --warmup 2 --runs 1 --max-wait 0 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --workdir /home/cdkbs/bench-runs/bl/work-attr2 --extra-conf waystone_recording=off"
for r in 1 2; do
  for pl in spread kernel batch0 segregated-no0; do
    $M --placement $pl --out "$OUT/$pl/r$r" >> "$OUT/log.txt" 2>&1
  done
  python3 $TOOLS/bl_mixed.py --bin /home/cdkbs/bench-runs/bl/bin/kds_server.bls3 --port 17482 --cores 4 --arm isolated --readers 6 --rate 400 --seconds 10 --warmup 2 --runs 1 --max-wait 0 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --workdir /home/cdkbs/bench-runs/bl/work-attr2 --extra-conf waystone_recording=off --placement batch0 --out "$OUT/isolated/r$r" >> "$OUT/log.txt" 2>&1
done
echo done > "$OUT/done"
