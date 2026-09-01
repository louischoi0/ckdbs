#!/usr/bin/env bash
# Starts one kds_server on a fresh data directory and prints its pid.
# OPT-003's own copy of bench/run_ab_server.sh (that script is not touched -
# this role's task explicitly keeps this proposal's driver and orchestration
# under CIP/OPT-003-walk-read-access/archive/, nothing in bench/), extended
# with a checkpoint_interval_ms argument: OPT-003's instrument disables the
# timer cadence and drives every checkpoint explicitly via SYNC, so the
# write-amplification delta is attributable to a statement the driver issued
# rather than to a background timer racing the measurement.
#
#   run_ab_server_opt003.sh <binary> <port> <dir> <durability> <checkpoint_interval_ms>
set -euo pipefail

binary="$1"; port="$2"; dir="$3"; durability="$4"; ckpt_ms="$5"

rm -rf "$dir"
mkdir -p "$dir/wal"
cat > "$dir/kds.conf" <<EOF
data_file = $dir/kds.db
wal_dir = $dir/wal
port = $port
durability = $durability
checkpoint_interval_ms = $ckpt_ms
cores = 1
log_level = warn
log_dir = $dir
log_file = kds.log
EOF

"$binary" --config "$dir/kds.conf" > "$dir/stdout.log" 2>&1 &
pid=$!

for _ in $(seq 1 3000); do
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "server exited during startup:" >&2
        tail -20 "$dir/stdout.log" >&2
        exit 1
    fi
    if grep -q "listening on" "$dir/stdout.log" 2>/dev/null; then
        echo "$pid"
        exit 0
    fi
    sleep 0.01
done
echo "no listener within 30s:" >&2
tail -20 "$dir/stdout.log" >&2
kill -9 "$pid" 2>/dev/null || true
exit 1
