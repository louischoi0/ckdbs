#!/bin/bash
# CA round 2 Part A driver: base/c1/c13 rotating order, 3 reps (s4 in reps 1-2 only).
H=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-harness
OUT=$1; B=/home/cdkbs/bench-runs/bz
export S1_ARGS="--years 5 --symbols 8 --exchanges 2 --ops 300 --qps-ops 300 --compare-rounds 4 --conn-ops 100 --write-ops 1000 --write-batches 1,100 --connections 1,4"
export S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 10000 --bookers 8 --bookings 9000 --verify 100 --seed 1 --sync"
export S4_ARGS="--days 2 --blocks 4 --session-seconds 10 --overnight-seconds 6"
mkdir -p "$OUT"
st() { echo "$(date -u +%FT%TZ) $*" > "$OUT/status"; echo "$(date -u +%FT%TZ) $*" >> "$OUT/timeline"; }
ORD=("base c1 c13" "c1 c13 base" "c13 base c1")
for R in 1 2 3; do
  if [ $R -le 2 ]; then export SCEN="1 2 3 4"; else export SCEN="1 2 3"; fi
  for A in ${ORD[$((R-1))]}; do st "rep $R $A"; "$H/quad.sh" $B/kds_server.$A $A "$OUT" $R; done
done
st "DONE"; touch "$OUT/done"
