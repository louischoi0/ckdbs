#!/bin/sh
# BL-S1 attribution: the four-core segregated mixed cell under three
# conditions, two runs each, interleaved. Usage: attr.sh OUTDIR
OUT=$1; mkdir -p "$OUT"
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bl-session-balancer/tools
M="python3 $TOOLS/bl_mixed.py --bin /home/cdkbs/bench-runs/bl/bin/kds_server.bls3 --port 17481 --cores 4 --arm mixed --readers 6 --rate 400 --seconds 10 --warmup 2 --runs 1 --max-wait 0 --server-cpus 0,2,4,6 --client-cpus 1,3,5,7 --workdir /home/cdkbs/bench-runs/bl/work-attr"
for r in 1 2; do
  $M --placement segregated --out "$OUT/ws-on/r$r" >> "$OUT/log.txt" 2>&1
  $M --placement segregated --extra-conf "waystone_recording = off" --out "$OUT/ws-off/r$r" >> "$OUT/log.txt" 2>&1
  $M --placement segregated-no0 --extra-conf "waystone_recording = off" --out "$OUT/ws-off-no0/r$r" >> "$OUT/log.txt" 2>&1
  $M --placement segregated-no0 --out "$OUT/ws-on-no0/r$r" >> "$OUT/log.txt" 2>&1
done
echo done > "$OUT/done"
