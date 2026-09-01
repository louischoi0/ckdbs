#!/usr/bin/env bash
# Starts one kds_server *under strace* on a fresh data directory, tracing
# only the syscalls the bytes instrument needs, and prints the traced
# server's own pid (strace's child, not strace itself).
#
# Why: /proc/<pid>/io's write_bytes is process-wide and cannot separate the
# data file's own writeback from the WAL's - and the WAL dominates it by
# two orders of magnitude here (every statement's redo record gets its own
# pwrite+fdatasync under durability=group with one statement in flight, on
# a *separate* fd from the data file). ptrace-attaching to a running server
# is refused in this sandbox (PTRACE_SEIZE: Operation not permitted), so
# tracing has to start at exec - which is what this script does instead of
# run_ab_server_opt003.sh's plain launch.
#
#   run_ab_server_strace.sh <binary> <port> <dir> <durability> <checkpoint_interval_ms>
#
# Writes <dir>/strace.out (openat/pwrite64/fsync/fdatasync, -ttt timestamps -
# seconds.microseconds since the epoch, directly comparable to Python's own
# time.time() with no timezone/midnight-rollover ambiguity).
# The driver identifies the data-file fd from the openat records rather than
# assuming a fixed number, and buckets pwrite64(<that fd>, ...) bytes into
# rounds by wall-clock timestamp.
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

strace -f -ttt -e trace=openat,pwrite64,fsync,fdatasync -o "$dir/strace.out" \
    "$binary" --config "$dir/kds.conf" > "$dir/stdout.log" 2>&1 &
strace_pid=$!

for _ in $(seq 1 3000); do
    if ! kill -0 "$strace_pid" 2>/dev/null; then
        echo "strace/server exited during startup:" >&2
        tail -20 "$dir/stdout.log" >&2
        exit 1
    fi
    if grep -q "listening on" "$dir/stdout.log" 2>/dev/null; then
        # The traced binary is strace's child, not $strace_pid itself.
        server_pid=$(pgrep -P "$strace_pid" -f "$(basename "$binary")" | head -1)
        if [ -z "$server_pid" ]; then
            echo "could not resolve traced server pid under strace $strace_pid" >&2
            exit 1
        fi
        echo "$server_pid"
        exit 0
    fi
    sleep 0.01
done
echo "no listener within 30s:" >&2
tail -20 "$dir/stdout.log" >&2
kill -9 "$strace_pid" 2>/dev/null || true
exit 1
