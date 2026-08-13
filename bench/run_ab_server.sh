#!/usr/bin/env bash
# Starts one kds_server on a fresh data directory and prints its pid.
#
#   run_ab_server.sh <binary> <port> <dir> <durability>
#
# Fresh directory per invocation, by rule: catalog rows are never reclaimed and
# undo never purges, so a second run on one file is not a repeat of the first.
# The version floor raised at 67ce947 also makes a shared file impossible across
# the two sides of this A/B - a v1 segment is refused outright.
set -euo pipefail

binary="$1"; port="$2"; dir="$3"; durability="$4"

rm -rf "$dir"
mkdir -p "$dir/wal"
cat > "$dir/kds.conf" <<EOF
data_file = $dir/kds.db
wal_dir = $dir/wal
port = $port
durability = $durability
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
