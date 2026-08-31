#!/usr/bin/env bash
set -uo pipefail
ROOT=/home/ubuntu/bench-xd
CELLS=$ROOT/cells
OUT=$ROOT/out
SERVER=$ROOT/kds_server-951a91a
WORKTREE=/home/ubuntu/ckdbs/.claude/worktrees/measure-v2.7.1
DRIVER=$WORKTREE/bench/wal_sync_decomposition_probe.py
WAITQ=$WORKTREE/bench/wait_quiet.sh
mkdir -p "$CELLS" "$OUT"

PORT=15900

run_cell() {
    local label=$1; shift
    local require_shipped=$1; shift
    local attempt=1
    local max_attempts=6
    while true; do
        PORT=$((PORT+1))
        rm -rf "${CELLS:?}/$label"
        local extra=(--force)
        if [[ "$require_shipped" == "yes" ]]; then extra+=(--require-shipped); fi
        "$WAITQ"
        echo "=== $label (attempt $attempt, port $PORT) $(date -u +%FT%TZ) ===" | tee -a "$OUT/run.log"
        echo "uptime: $(uptime)" | tee -a "$OUT/run.log"
        ( while true; do cut -d' ' -f1-3 /proc/loadavg; sleep 3; done ) > "$CELLS/${label}.load" &
        local sampler=$!
        python3 "$DRIVER" --server "$SERVER" --workdir "$CELLS" --label "$label" --port "$PORT" \
            --json "$OUT/$label.json" "${extra[@]}" "$@" > "$OUT/$label.log" 2>&1
        rc=$?
        kill "$sampler" 2>/dev/null || true
        echo "load samples max: $(sort -g "$CELLS/${label}.load" 2>/dev/null | tail -1)" | tee -a "$OUT/run.log"
        if [[ $rc -eq 0 ]]; then
            echo "OK $label rc=0" | tee -a "$OUT/run.log"
            return 0
        elif [[ $rc -eq 42 && $attempt -lt $max_attempts ]]; then
            echo "RETRY $label (landed_local)" | tee -a "$OUT/run.log"
            attempt=$((attempt+1))
            continue
        else
            echo "FAILED $label rc=$rc" | tee -a "$OUT/run.log"
            tail -60 "$OUT/$label.log" | tee -a "$OUT/run.log"
            return 1
        fi
    done
}

SCALE=(--organizations 2000 --ships 200 --operations 2000 --cargos 20000 --seed 1)

# ---- XD2: sync accounting -------------------------------------------------
run_cell xd2-pl-c8-b1   yes --cores 8 --peer-listeners on  --durability group --bookers 1 --bookings 5000 --verify 25 "${SCALE[@]}" || exit 1
run_cell xd2-pl-c8-b8   no  --cores 8 --peer-listeners on  --durability group --bookers 8 --bookings 5000 --verify 25 "${SCALE[@]}" || exit 1
run_cell xd2-nopl-c8-b1 no  --cores 8 --peer-listeners off --durability group --bookers 1 --bookings 5000 "${SCALE[@]}" || exit 1
run_cell xd2-nopl-c8-b8 no  --cores 8 --peer-listeners off --durability group --bookers 8 --bookings 5000 "${SCALE[@]}" || exit 1

# ---- XD3: tmpfs ablation ---------------------------------------------------
WALDIR=/dev/shm/bench-xd-wal
mkdir -p "$WALDIR"
run_cell xd3-tmpfs-pl-c8-b1         yes --cores 8 --peer-listeners on --durability group   --bookers 1 --bookings 5000 --wal-dir "$WALDIR" "${SCALE[@]}" || exit 1
run_cell xd3-tmpfs-pl-c8-b8         no  --cores 8 --peer-listeners on --durability group   --bookers 8 --bookings 5000 --wal-dir "$WALDIR" "${SCALE[@]}" || exit 1
run_cell xd3-tmpfs-pl-c8-b8-relaxed no  --cores 8 --peer-listeners on --durability relaxed --bookers 8 --bookings 5000 --wal-dir "$WALDIR" "${SCALE[@]}" || exit 1

# ---- XD4: queueing curve (b1/b8 reused from XD2's xd2-pl-c8-b1/b8) --------
run_cell xd4-pl-c8-b2 no --cores 8 --peer-listeners on --durability group --bookers 2 --bookings 5000 "${SCALE[@]}" || exit 1
run_cell xd4-pl-c8-b4 no --cores 8 --peer-listeners on --durability group --bookers 4 --bookings 5000 "${SCALE[@]}" || exit 1

# ---- XD5: strict pair, 3 repeats each --------------------------------------
run_cell xd5-strict-c1-r1   no  --cores 1 --durability strict --bookers 1 --bookings 5000 --verify 25 "${SCALE[@]}" || exit 1
run_cell xd5-strict-c1-r2   no  --cores 1 --durability strict --bookers 1 --bookings 5000 "${SCALE[@]}" || exit 1
run_cell xd5-strict-c1-r3   no  --cores 1 --durability strict --bookers 1 --bookings 5000 "${SCALE[@]}" || exit 1
run_cell xd5-strict-c8pl-r1 yes --cores 8 --peer-listeners on --durability strict --bookers 1 --bookings 5000 --verify 25 "${SCALE[@]}" || exit 1
run_cell xd5-strict-c8pl-r2 yes --cores 8 --peer-listeners on --durability strict --bookers 1 --bookings 5000 "${SCALE[@]}" || exit 1
run_cell xd5-strict-c8pl-r3 yes --cores 8 --peer-listeners on --durability strict --bookers 1 --bookings 5000 "${SCALE[@]}" || exit 1

echo "ALL DONE $(date -u +%FT%TZ)" | tee -a "$OUT/run.log"
