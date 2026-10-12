#!/bin/sh
# BL-S3's utility: arm D's skew under the balancer against the same skew
# without it and a balanced placement without it (BL section 5.2 bar ii).
# Usage: converge.sh OUTDIR RUNS BALANCER_BIN BASE_BIN
OUT=$1; RUNS=${2:-3}; BAL=$3; BASE=$4
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bl-session-balancer/tools
mkdir -p "$OUT"
for r in $(seq 1 "$RUNS"); do
  for cell in "2 16 0,2 4,5,6,7" "4 8 0,2,4,6 1,3,5,7" "4 16 0,2,4,6 1,3,5,7"; do
    set -- $cell
    for arm in "base balanced $BASE" "base skew $BASE" "bal skew $BAL"; do
      set -- $cell $arm
      /home/cdkbs/bench-runs/bl/quiet_gate.sh
      python3 $TOOLS/ba_census.py --bin $7 --port 17445 --shape pkmix --durability relaxed \
        --cores $1 --sessions $2 --server-cpus $3 --client-cpus $4 --placement $6 \
        --runs 1 --seconds 12 --warmup 2 --rows 16000 --max-load 3.0 --max-wait 15 \
        --workdir /home/cdkbs/bench-runs/bl/work-conv-$5 --out "$OUT/$5/r$r" >> "$OUT/log.txt" 2>&1
    done
  done
done
touch "$OUT/done"
