#!/usr/bin/env bash
# F, retried: arm B's gain with the fsync removed. If it survives
# `durability = relaxed` it is scheduling; if it vanishes it is the commit.
set -uo pipefail
CELL=/tmp/claude-1000/-home-ubuntu-ckdbs/ad86eadc-4ba2-462f-a647-87d18fa27322/scratchpad/s2_cell2.sh
FREE="--cargos 20000 --bookings 5000 --seed 1 --verify 0 --manifest-customers 0"

port=15660
run () {
    local label=$1 cores=$2 pl=$3 dur=$4; shift 4
    port=$((port+1))
    echo "### $label (cores=$cores pl=$pl durability=$dur port=$port) $(date -u +%T)"
    DURABILITY="$dur" bash "$CELL" "$label" "$cores" "$pl" "$port" -- "$@"
    echo "### $label rc=$?"
}

run "f-c1-b8-relaxed" 1 off relaxed $FREE --bookers 8
run "f-c8-b8-relaxed" 8 off relaxed $FREE --bookers 8
run "f-c8-b8-pl-relaxed" 8 on relaxed $FREE --bookers 8

echo "FOLLOWUP2 DONE $(date -u +%T)"
