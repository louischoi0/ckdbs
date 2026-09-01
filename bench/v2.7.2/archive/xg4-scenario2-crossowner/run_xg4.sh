#!/usr/bin/env bash
# XG4: scenario 2 whole, typed client, per-leg. The cells XE §4.1 could not
# run at all.
#
# **`--require-shipped` on every `pl` cell, and the rc=42 retry with it.**
# Without it a `pl` cell whose booker happened to land on the
# relation-owning core reports a *local* booking under a cross-owner label -
# `landed_local: true`, zero shipped statements, `syncs_per_booking` 1.00 -
# and the whole file would be wrong in the one direction nobody checks. The
# first draft of this harness had exactly that, caught by reading a cell
# rather than by any failure.
set -uo pipefail
S=/tmp/claude-1000/-home-cdkbs-ckdbs/4e50e866-8005-43e4-b524-1d53355b0c93/scratchpad
WT=/home/cdkbs/ckdbs/.claude/worktrees/xf
ROOT=$S/bench-xg4m
OUT=$ROOT/out
CELLS=$ROOT/cells
DRIVER=$WT/bench/wal_sync_decomposition_probe.py
WAITQ=$WT/bench/wait_quiet.sh
SERVER=$S/arms/kds_server-xg4
mkdir -p "$OUT" "$CELLS"

PORT=17400
SCALE=(--organizations 500 --ships 50 --operations 500 --cargos 5000 --seed 1)

# $1 label, $2 "require"|"plain", rest: driver args
run_cell() {
    local label=$1; shift
    local mode=$1; shift
    local attempt=1 max=8
    local extra=()
    [[ $mode == require ]] && extra=(--require-shipped)
    while true; do
        PORT=$((PORT+1))
        rm -rf "${CELLS:?}/$label"
        "$WAITQ" > /dev/null
        echo "=== $label (port $PORT, attempt $attempt) $(date -u +%FT%TZ) ===" >> "$OUT/run.log"
        timeout 900 python3 "$DRIVER" --server "$SERVER" --workdir "$CELLS" --label "$label" \
            --port "$PORT" --json "$OUT/$label.json" --force "${extra[@]}" "$@" \
            > "$OUT/$label.log" 2>&1
        local rc=$?
        if [[ $rc -eq 0 ]]; then
            echo "OK $label (attempt $attempt)" >> "$OUT/run.log"
            return 0
        elif [[ $rc -eq 42 && $attempt -lt $max ]]; then
            # The booker landed on the owning core: nothing shipped, so the
            # cell is not the cell. A fresh port re-rolls which core accepts.
            attempt=$((attempt+1)); continue
        else
            echo "FAILED $label rc=$rc after $attempt attempt(s)" >> "$OUT/run.log"
            tail -12 "$OUT/$label.log" >> "$OUT/run.log"
            return 1
        fi
    done
}

for r in 1 2 3; do
    for b in 1 8; do
        run_cell "c1-b${b}-r${r}" plain --cores 1 --peer-listeners off --durability group \
            --bookers "$b" --bookings 1000 "${SCALE[@]}"
        for c in 2 4; do
            run_cell "c${c}-pl-b${b}-r${r}" require --cores "$c" --peer-listeners on \
                --durability group --bookers "$b" --bookings 1000 "${SCALE[@]}"
            run_cell "c${c}-nopl-b${b}-r${r}" plain --cores "$c" --peer-listeners off \
                --durability group --bookers "$b" --bookings 1000 "${SCALE[@]}"
        done
    done
done
echo "ALL DONE $(date -u +%FT%TZ)" >> "$OUT/run.log"
