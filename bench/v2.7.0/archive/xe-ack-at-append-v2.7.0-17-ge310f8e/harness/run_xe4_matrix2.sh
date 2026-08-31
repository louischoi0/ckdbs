#!/usr/bin/env bash
set -uo pipefail
ROOT=/home/ubuntu/bench-xe
CELLS=$ROOT/cells
XOWNER=$ROOT/xowner
OUT=$ROOT/out
WORKTREE=/home/ubuntu/ckdbs/.claude/worktrees/measure-v2.7.1
DRIVER_S2=$WORKTREE/bench/wal_sync_decomposition_probe.py
DRIVER_XO=$WORKTREE/bench/xe4_crossowner_commit_probe.py
WAITQ=$WORKTREE/bench/wait_quiet.sh
mkdir -p "$CELLS" "$XOWNER" "$OUT"

PORT=16100

declare -A SERVER
SERVER[base]=$ROOT/kds_server-base
SERVER[xe1]=$ROOT/kds_server-xe1

run_s2_cell() {
    # nopl cells: scenario 2's own driver, unaffected by the KWP shipped-
    # read refusal (peer_listeners=off, everything local to core 0).
    local label=$1; shift
    local server=$1; shift
    local attempt=1
    local max_attempts=4
    while true; do
        PORT=$((PORT+1))
        rm -rf "${CELLS:?}/$label"
        "$WAITQ"
        echo "=== $label (attempt $attempt, port $PORT) $(date -u +%FT%TZ) ===" | tee -a "$OUT/run.log"
        python3 "$DRIVER_S2" --server "$server" --workdir "$CELLS" --label "$label" --port "$PORT" \
            --json "$OUT/$label.json" --force "$@" > "$OUT/$label.log" 2>&1
        rc=$?
        if [[ $rc -eq 0 ]]; then
            echo "OK $label rc=0" | tee -a "$OUT/run.log"
            return 0
        elif [[ $rc -eq 42 && $attempt -lt $max_attempts ]]; then
            attempt=$((attempt+1)); continue
        else
            echo "FAILED $label rc=$rc" | tee -a "$OUT/run.log"
            tail -60 "$OUT/$label.log" | tee -a "$OUT/run.log"
            return 1
        fi
    done
}

run_xo_cell() {
    # pl cells: the substitute cross-owner commit-latency probe.
    local label=$1; shift
    local server=$1; shift
    PORT=$((PORT+1))
    rm -rf "${XOWNER:?}/$label"
    "$WAITQ"
    echo "=== $label (port $PORT) $(date -u +%FT%TZ) ===" | tee -a "$OUT/run.log"
    python3 "$DRIVER_XO" --server "$server" --workdir "$XOWNER" --label "$label" --port "$PORT" \
        --json "$OUT/$label.json" "$@" > "$OUT/$label.log" 2>&1
    rc=$?
    if [[ $rc -eq 0 ]]; then
        echo "OK $label rc=0" | tee -a "$OUT/run.log"
        return 0
    else
        echo "FAILED $label rc=$rc" | tee -a "$OUT/run.log"
        tail -60 "$OUT/$label.log" | tee -a "$OUT/run.log"
        return 1
    fi
}

SCALE=(--organizations 2000 --ships 200 --operations 2000 --cargos 20000 --seed 1)

for arm in base xe1; do
    S=${SERVER[$arm]}
    for r in 1 2 3; do
        run_s2_cell xe4-group-nopl-b1-${arm}-r${r} "$S" \
            --cores 8 --peer-listeners off --durability group --bookers 1 --bookings 5000 "${SCALE[@]}" || exit 1
        run_s2_cell xe4-group-nopl-b8-${arm}-r${r} "$S" \
            --cores 8 --peer-listeners off --durability group --bookers 8 --bookings 5000 "${SCALE[@]}" || exit 1
        run_xo_cell xe4xo-group-b1-${arm}-r${r} "$S" \
            --cores 2 --durability group --concurrency 1 --txns 2000 || exit 1
        run_xo_cell xe4xo-group-b8-${arm}-r${r} "$S" \
            --cores 2 --durability group --concurrency 8 --txns 4000 || exit 1
    done
    run_xo_cell xe4xo-strict-b1-${arm}-r1 "$S" \
        --cores 2 --durability strict --concurrency 1 --txns 2000 || exit 1
    run_xo_cell xe4xo-relaxed-b8-${arm}-r1 "$S" \
        --cores 2 --durability relaxed --concurrency 8 --txns 4000 || exit 1
done

echo "ALL DONE $(date -u +%FT%TZ)" | tee -a "$OUT/run.log"
