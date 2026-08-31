#!/usr/bin/env bash
set -uo pipefail
ROOT=/home/ubuntu/bench-xd
CELLS=$ROOT/cells
OUT=$ROOT/out
SERVER=$ROOT/kds_server-951a91a
WORKTREE=/home/ubuntu/ckdbs/.claude/worktrees/measure-v2.7.1
DRIVER=$WORKTREE/bench/wal_sync_decomposition_probe.py
WAITQ=$WORKTREE/bench/wait_quiet.sh

PORT=15950

run_cell() {
    local label=$1; shift
    local attempt=1
    local max_attempts=10
    while true; do
        PORT=$((PORT+1))
        rm -rf "${CELLS:?}/$label"
        "$WAITQ"
        echo "=== $label (attempt $attempt, port $PORT) $(date -u +%FT%TZ) ===" | tee -a "$OUT/rerun.log"
        python3 "$DRIVER" --server "$SERVER" --workdir "$CELLS" --label "$label" --port "$PORT" \
            --json "$OUT/$label.json" --force --require-shipped-rate 0.97 "$@" > "$OUT/$label.log" 2>&1
        rc=$?
        if [[ $rc -eq 0 ]]; then
            echo "OK $label rc=0 (attempt $attempt)" | tee -a "$OUT/rerun.log"
            return 0
        elif [[ $rc -eq 42 && $attempt -lt $max_attempts ]]; then
            echo "RETRY $label" | tee -a "$OUT/rerun.log"
            attempt=$((attempt+1))
            continue
        else
            echo "FAILED $label rc=$rc" | tee -a "$OUT/rerun.log"
            tail -60 "$OUT/$label.log" | tee -a "$OUT/rerun.log"
            return 1
        fi
    done
}

SCALE=(--organizations 2000 --ships 200 --operations 2000 --cargos 20000 --seed 1)

run_cell xd2-pl-c8-b8-allx --cores 8 --peer-listeners on --durability group --bookers 8 --bookings 5000 --verify 25 "${SCALE[@]}" || exit 1
run_cell xd4-pl-c8-b2-allx --cores 8 --peer-listeners on --durability group --bookers 2 --bookings 5000 "${SCALE[@]}" || exit 1
run_cell xd4-pl-c8-b4-allx --cores 8 --peer-listeners on --durability group --bookers 4 --bookings 5000 "${SCALE[@]}" || exit 1

echo "RERUN DONE $(date -u +%FT%TZ)" | tee -a "$OUT/rerun.log"
