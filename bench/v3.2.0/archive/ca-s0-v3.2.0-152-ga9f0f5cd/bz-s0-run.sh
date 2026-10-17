#!/bin/bash
# CA-S0 driver: A (ABAB interleaved base / adv-off) then B (sampled base). Writes $OUT/status.
H=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-harness
OUT=$1
BIN=/home/cdkbs/bench-runs/bz/kds_server.base
export S1_ARGS="--years 5 --symbols 8 --exchanges 2 --ops 300 --qps-ops 300 --compare-rounds 4 --conn-ops 100 --write-ops 1000 --write-batches 1,100 --connections 1,4"
export S2_ARGS="--organizations 300 --ships 30 --operations 300 --cargos 10000 --bookers 8 --bookings 9000 --verify 100 --seed 1 --sync"
export S4_ARGS="--days 2 --blocks 4 --session-seconds 10 --overnight-seconds 6"
mkdir -p "$OUT"
st() { echo "$(date -u +%FT%TZ) $*" > "$OUT/status"; echo "$(date -u +%FT%TZ) $*" >> "$OUT/timeline"; }
OFF=("waystone_recording = false" "waystone_replay = false" "access_statistics = false")
for R in 1 2 3; do
    if [ $R -le 2 ]; then export SCEN="1 2 3 4"; else export SCEN="1 2 3"; fi
    st "A rep $R base"; "$H/quad.sh" $BIN base "$OUT" $R
    st "A rep $R adv-off"; "$H/quad.sh" $BIN adv-off "$OUT" $R "${OFF[@]}"
done
export SCEN="1 2 3 4" SAMPLER=/home/cdkbs/bench-runs/bz/bin/kds_sampler.so KDS_SAMPLER_HZ=500
st "B sampled base"; "$H/quad.sh" $BIN prof-base "$OUT" 1
st "DONE"; touch "$OUT/done"
