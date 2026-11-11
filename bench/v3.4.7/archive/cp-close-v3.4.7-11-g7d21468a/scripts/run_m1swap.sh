#!/bin/bash
# M1 repeat with the driver's primary/AB label roles swapped (close is the primary label, runs first in even blocks).
set -uo pipefail
SP=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close
STAMP=$(cat $SP/run/current_stamp)
RUN=/home/louis/bench-runs/cp-close/$STAMP
OUTROOT=$SP/run/$STAMP
STATUS=$SP/run/status.txt; HB=$SP/run/heartbeat
now() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
phase() { echo "$(now) $*" | tee -a "$STATUS"; }
( while :; do echo "$(now) $(tail -1 "$STATUS" 2>/dev/null)" > "$HB"; sleep 20; done ) &
HBPID=$!
trap 'kill $HBPID 2>/dev/null; touch $SP/run/host.stop3' EXIT
phase "START-M1SWAP"
rm -f $SP/run/host.stop3; bash "$SP/host_sampler.sh" "$OUTROOT/host-m1swap.log" "$SP/run/host.stop3" &
phase "BEGIN m1swap"
env RUN_ROOT="$RUN/m1swap" OUT="$OUTROOT/m1swap" CELLS=m1 M1_REPS=3 M1_OPS=40000 bash "$SP/m1m4/run_m1m4_swapped.sh" > "$OUTROOT/m1swap.console.txt" 2>&1
phase "END m1swap rc=$?"
phase "DONE-M1SWAP"
