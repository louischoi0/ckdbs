#!/usr/bin/env bash
# The scenario-2 core matrix: cores in {1,2,4,8} against booker concurrency,
# with and without per-core listeners, plus the stock (join-bearing) arm.
set -uo pipefail
CELL=/tmp/claude-1000/-home-ubuntu-ckdbs/ad86eadc-4ba2-462f-a647-87d18fa27322/scratchpad/s2_cell.sh

# Identical engine work in every cell: the reporter's two supported shapes
# only, no verify pass, so nothing in the grid depends on a shape that a
# spread relation refuses. The refusals get their own arm below.
FREE="--cargos 20000 --bookings 5000 --seed 1 --verify 0 --manifest-customers 0"

port=15610
run () {  # run <label> <cores> <pl> <flags...>
    local label=$1 cores=$2 pl=$3; shift 3
    port=$((port+1))
    echo "### $label (cores=$cores pl=$pl port=$port) $(date -u +%T)"
    bash "$CELL" "$label" "$cores" "$pl" "$port" -- "$@"
    echo "### $label rc=$?"
}

# --- A: the scaling grid, per-core listeners on wherever cores > 1 --------
for b in 1 4 8; do
    run "a-c1-b$b" 1 off $FREE --bookers $b
done
for c in 2 4 8; do
    for b in 1 4 8; do
        run "a-c$c-b$b" "$c" on $FREE --bookers $b
    done
done

# --- B: the shipped default (one listener, core 0) as the control ---------
for c in 2 4 8; do
    run "b-c$c-b8-nopl" "$c" off $FREE --bookers 8
done

# --- C: the stock workload - verify pass and the customer-statement join --
run "c-stock-c1" 1 off --cargos 20000 --bookings 3000 --seed 1 --verify 25 --bookers 4
for c in 2 4 8; do
    run "c-stock-c$c" "$c" on --cargos 20000 --bookings 3000 --seed 1 --verify 25 --bookers 4
done

# --- D: noise floor, the two cells the scaling claim rests on -------------
for r in 2 3; do
    run "d-c1-b1-r$r" 1 off $FREE --bookers 1
done
for r in 2 3; do
    run "d-c8-b8-r$r" 8 on $FREE --bookers 8
done

echo "ALL DONE $(date -u +%T)"
