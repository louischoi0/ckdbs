#!/usr/bin/env bash
# Two follow-ups the main matrix leaves open.
#
# E: peer listeners ON but `range_size_ids = 0` - sessions on every core,
#    nothing ever splits. Separates "the session moved" from "the relation
#    split", which arm A conflates.
# F: arm B's gain under `durability = relaxed` - if it survives the fsync
#    being removed it is scheduling, if it vanishes it is the commit.
set -uo pipefail
CELL=/tmp/claude-1000/-home-ubuntu-ckdbs/ad86eadc-4ba2-462f-a647-87d18fa27322/scratchpad/s2_cell2.sh
FREE="--cargos 20000 --bookings 5000 --seed 1 --verify 0 --manifest-customers 0"

port=15650
run () {
    local label=$1 cores=$2 pl=$3 extra=$4; shift 4
    port=$((port+1))
    echo "### $label (cores=$cores pl=$pl extra='$extra' port=$port) $(date -u +%T)"
    EXTRA_CONF="$extra" bash "$CELL" "$label" "$cores" "$pl" "$port" -- "$@"
    echo "### $label rc=$?"
}

for c in 2 4 8; do
    run "e-c$c-b8-nosplit" "$c" on "range_size_ids = 0" $FREE --bookers 8
done

run "f-c1-b8-relaxed" 1 off "durability = relaxed" $FREE --bookers 8
run "f-c8-b8-relaxed" 8 off "durability = relaxed" $FREE --bookers 8

echo "FOLLOWUP DONE $(date -u +%T)"
