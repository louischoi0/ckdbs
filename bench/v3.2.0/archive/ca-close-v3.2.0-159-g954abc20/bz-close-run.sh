#!/bin/bash
# CA milestone-close A/B: base/c13/c134, 4 reps rotating order, all four scenarios per rep (s4 included every rep).
H=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-harness
OUT=$1; B=/home/cdkbs/bench-runs/bz
export S1_ARGS="--years 5 --symbols 8 --exchanges 2 --ops 300 --qps-ops 300 --compare-rounds 4 --conn-ops 100 --write-ops 1000 --write-batches 1,100 --connections 1,4"
export S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 10000 --bookers 8 --bookings 9000 --verify 100 --seed 1 --sync"
export S4_ARGS="--days 2 --blocks 4 --session-seconds 10 --overnight-seconds 6"
export SCEN="1 2 3 4"
mkdir -p "$OUT"
st() { echo "$(date -u +%FT%TZ) $*" > "$OUT/status"; echo "$(date -u +%FT%TZ) $*" >> "$OUT/timeline"; }
ORD=("base c13 c134" "c13 c134 base" "c134 base c13" "base c13 c134")
for R in 1 2 3 4; do
  for A in ${ORD[$((R-1))]}; do
    st "rep $R $A"
    if pgrep -x 'cc1plus|cmake|ctest' >/dev/null; then echo "$(date -u +%FT%TZ) BUILD ACTIVE before rep $R $A" >> "$OUT/timeline"; fi
    "$H/quad.sh" $B/kds_server.$A $A "$OUT" $R
  done
done
st "DONE"; touch "$OUT/done"
