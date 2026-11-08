#!/bin/bash
# CP-S0 second pass inside the same 20-minute budget: M5 re-run with the
# relation suffix fixed (a hyphen in the first pass's name was refused at
# CREATE TABLE), and M1 re-run with three repetitions at a larger ops count
# (the first pass's one rep had its two labels on different sides of core 0,
# which the run script's own check flagged as biased).
set -uo pipefail
SP=/tmp/claude-1000/-home-louis-ckdbs/1c794fd2-a838-4425-a96a-e3b26a73757b/scratchpad/cp-s0
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN=/home/louis/bench-runs/cp-s0/$STAMP
OUTROOT=$SP/run/$STAMP
mkdir -p "$RUN" "$OUTROOT"
STATUS=$SP/run/status.txt
HB=$SP/run/heartbeat
now() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
phase() { echo "$(now) $*" | tee -a "$STATUS"; }
( while :; do echo "$(now) $(tail -1 "$STATUS" 2>/dev/null)" > "$HB"; sleep 20; done ) &
HBPID=$!
trap 'kill $HBPID 2>/dev/null; touch $SP/run/host.stop' EXIT
echo "$STAMP" > "$SP/run/current_stamp"
: > "$STATUS"
phase "START2 run=$RUN out=$OUTROOT"
rm -f "$SP/run/host.stop"; bash "$SP/host_sampler.sh" "$OUTROOT/host.log" "$SP/run/host.stop" &
T0=$(date +%s)
step() { local name=$1; shift; phase "BEGIN $name"; "$@" > "$OUTROOT/$name.console.txt" 2>&1; local rc=$?; phase "END $name rc=$rc elapsed=$(( $(date +%s) - T0 ))s"; }

step m5 env M5_WORKDIR_BASE="$RUN/m5" M5_OPS=1000 M5_BLOCKS=6 \
    bash "$SP/m5/run_m5.sh"
step m1 env RUN_ROOT="$RUN/m1m4" OUT="$OUTROOT/m1m4-m1" CELLS=m1 M1_REPS=3 M1_OPS=40000 \
    bash "$SP/m1m4/run_m1m4.sh"

phase "DONE elapsed=$(( $(date +%s) - T0 ))s"
