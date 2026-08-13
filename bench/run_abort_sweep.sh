#!/usr/bin/env bash
# The abort/commit A/B sweep: one fresh server pair per cell.
#
#   run_abort_sweep.sh <outdir> <durability> <rows> <K> [extra driver args...]
#
# Fresh data file and fresh server per cell, by rule - and here it is also
# forced: 67ce947 raised the WAL segment format floor, so the base binary
# cannot mount a file the head binary wrote.
set -euo pipefail

W=/home/cdkbs/ckdbs/.claude/worktrees/assert-orphan-flag-format-v2
out="$1"; dur="$2"; rows="$3"; k="$4"; shift 4

mkdir -p "$out"
cell="rows${rows}-k${k}-${dur}"

# The CPU window is sized so one 10 ms scheduler tick is a small error: a
# transaction's CPU is roughly proportional to K, so the transaction count per
# window is scaled inversely to keep every window's CPU comparable.
cpu_txns=$(( 6000 / k )); [ "$cpu_txns" -lt 500 ] && cpu_txns=500

hp=$("$W/bench/run_ab_server.sh" "$W/build-rel-head/kds_server" 15601 /home/cdkbs/abbench/head "$dur")
bp=$("$W/bench/run_ab_server.sh" "$W/build-rel-base/kds_server" 15602 /home/cdkbs/abbench/base "$dur")
trap 'kill '"$hp $bp"' 2>/dev/null || true' EXIT

python3 "$W/tools/assertion_abort_benchmark.py" \
    --port 15601 --server-pid "$hp" --label head \
    --ab-port 15602 --ab-server-pid "$bp" --ab-label base \
    --rows "$rows" --reservations "$k" \
    --cpu-rounds 4 --cpu-txns "$cpu_txns" \
    --json "$out/$cell.json" "$@" 2>&1 | tee "$out/$cell.txt"

kill "$hp" "$bp" 2>/dev/null || true
sleep 1
