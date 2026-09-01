#!/usr/bin/env bash
# OPT-003 interleaved A/B sweep: 200 / 1000 / 10000 rows.
#
# Arm A binary: .../opt003/run/kds_server_A   (40c5e86, v2.7.0-40-g40c5e86)
# Arm B binary: .../opt003/run/kds_server_B   (31bc482, v2.7.0-41-g31bc482)
#
# Fresh server + fresh data dir per (arm, row count, mode) - rule 6, catalog
# rows are never reclaimed and undo never purges, so a second run on one
# file is not a repeat of the first.
#
# Two server pairs per row count, per opt003_ab.py's own docstring:
#   plain  - untraced servers; raw (unsynced) latency pass + an untraced
#            checkpoint-latency-only pass. The authoritative latency numbers.
#   strace - servers launched under strace (openat/pwrite64/fsync/fdatasync);
#            checkpoint/bytes pass only. The authoritative write-amplification
#            numbers - /proc/<pid>/io cannot separate the data file's own
#            writeback from the WAL's on this workload (see the driver's
#            docstring for the measurement that ruled it out).
#
# checkpoint_interval_ms=0 on both kinds of server: the timer cadence is
# off, so every writeback in this run is driven by the driver's own
# explicit SYNC. durability=group per the task's instruction to measure
# latency "where a checkpoint's writes actually cost something".
set -euo pipefail

SCRATCH=/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/opt003
RUN_PLAIN="$SCRATCH/run_ab_server_opt003.sh"
RUN_STRACE="$SCRATCH/run_ab_server_strace.sh"
DRIVER=/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/CIP/OPT-003-walk-read-access/archive/opt003_ab.py
OUT=/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/CIP/OPT-003-walk-read-access/archive
DATADIR=/home/cdkbs/bench-opt003

PORT_A=15931
PORT_B=15932

for ROWS in 200 1000 10000; do
    echo "==== rows=$ROWS mode=plain ===="
    rm -rf "$DATADIR/plain-a-$ROWS" "$DATADIR/plain-b-$ROWS"
    PID_A=$("$RUN_PLAIN" "$SCRATCH/run/kds_server_A" "$PORT_A" "$DATADIR/plain-a-$ROWS" group 0)
    PID_B=$("$RUN_PLAIN" "$SCRATCH/run/kds_server_B" "$PORT_B" "$DATADIR/plain-b-$ROWS" group 0)
    echo "server A pid=$PID_A  server B pid=$PID_B"

    python3 "$DRIVER" --mode plain \
        --port-a "$PORT_A" --port-b "$PORT_B" \
        --rows "$ROWS" --seed 20260901 \
        --lat-rounds 25 --lat-select 15 --lat-update 15 --lat-delete 4 \
        --ckpt-reps 25 \
        --json "$OUT/run${ROWS}_plain.json" 2>&1 | tee "$OUT/run${ROWS}_plain.log"

    kill "$PID_A" "$PID_B" 2>/dev/null || true
    wait "$PID_A" "$PID_B" 2>/dev/null || true
    rm -rf "$DATADIR/plain-a-$ROWS" "$DATADIR/plain-b-$ROWS"
    echo "==== rows=$ROWS mode=plain done ===="

    echo "==== rows=$ROWS mode=strace ===="
    rm -rf "$DATADIR/strace-a-$ROWS" "$DATADIR/strace-b-$ROWS"
    SPID_A=$("$RUN_STRACE" "$SCRATCH/run/kds_server_A" "$PORT_A" "$DATADIR/strace-a-$ROWS" group 0)
    SPID_B=$("$RUN_STRACE" "$SCRATCH/run/kds_server_B" "$PORT_B" "$DATADIR/strace-b-$ROWS" group 0)
    echo "traced server A pid=$SPID_A  traced server B pid=$SPID_B"

    python3 "$DRIVER" --mode strace \
        --port-a "$PORT_A" --port-b "$PORT_B" \
        --strace-a "$DATADIR/strace-a-$ROWS/strace.out" \
        --strace-b "$DATADIR/strace-b-$ROWS/strace.out" \
        --rows "$ROWS" --seed 20260901 \
        --ckpt-reps 25 \
        --json "$OUT/run${ROWS}_strace.json" 2>&1 | tee "$OUT/run${ROWS}_strace.log"

    # The strace log per side lives at <dir>/strace.out; keep those two
    # (small, text) and drop the rest - never a data file or WAL segment.
    cp "$DATADIR/strace-a-$ROWS/strace.out" "$OUT/run${ROWS}_strace_A.out"
    cp "$DATADIR/strace-b-$ROWS/strace.out" "$OUT/run${ROWS}_strace_B.out"

    # SPID_A/B are the traced binary's own pid (strace's child); its strace
    # parent has to be killed too, or it's left tracing an exited process.
    STRACE_PID_A=$(ps -o ppid= -p "$SPID_A" 2>/dev/null | tr -d ' ')
    STRACE_PID_B=$(ps -o ppid= -p "$SPID_B" 2>/dev/null | tr -d ' ')
    kill "$SPID_A" "$SPID_B" 2>/dev/null || true
    [ -n "${STRACE_PID_A:-}" ] && kill "$STRACE_PID_A" 2>/dev/null || true
    [ -n "${STRACE_PID_B:-}" ] && kill "$STRACE_PID_B" 2>/dev/null || true
    sleep 0.2
    rm -rf "$DATADIR/strace-a-$ROWS" "$DATADIR/strace-b-$ROWS"
    echo "==== rows=$ROWS mode=strace done ===="
done
