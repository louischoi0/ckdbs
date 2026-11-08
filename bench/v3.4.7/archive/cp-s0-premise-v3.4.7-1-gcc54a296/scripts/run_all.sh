#!/bin/bash
# CP-S0 master schedule: the six cells in the order's priority (M1, M3, M2,
# M5, M4, M6 - M6's scripted half placed before M4 because it is short and
# is the cell that corrects known-gaps.md), each cell's own run script
# invoked with sizes cut to fit the 20-minute stage budget. Writes a phase
# line per step to $STATUS and a heartbeat every 20 s for the 60 s watchdog.
set -uo pipefail
SP=/tmp/claude-1000/-home-louis-ckdbs/1c794fd2-a838-4425-a96a-e3b26a73757b/scratchpad/cp-s0
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN=/home/louis/bench-runs/cp-s0/$STAMP          # ext4 (/dev/nvme0n1p1); the scratchpad is tmpfs
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
phase "START run=$RUN out=$OUTROOT"
rm -f "$SP/run/host.stop"; bash "$SP/host_sampler.sh" "$OUTROOT/host.log" "$SP/run/host.stop" &

T0=$(date +%s)
step() { local name=$1; shift; phase "BEGIN $name"; "$@" > "$OUTROOT/$name.console.txt" 2>&1; local rc=$?; phase "END $name rc=$rc elapsed=$(( $(date +%s) - T0 ))s"; }

# M1 (+ the proof arm): base vs null-table, one session, relaxed, interleaved.
step m1 env RUN_ROOT="$RUN/m1m4" OUT="$OUTROOT/m1m4-m1" CELLS=prove,m1 M1_REPS=1 M1_OPS=12000 PROVE_OPS=300 \
    bash "$SP/m1m4/run_m1m4.sh"
# M3: scenario 2 (--bookers 8) and scenario 0 (--fk --txn), group, the views read after each.
step m3 env WORKROOT="$RUN/m3" OUT="$OUTROOT/m3" HOST_SAMPLER=/nonexistent \
    bash "$SP/m3/run_m3.sh"
# M2: the scan's slice IS, base vs no-slice (bm_scan.py census, unmodified).
step m2 env M2_WORKDIR="$RUN/m2/work" M2_OUT="$OUTROOT/m2" M2_RUNS=5 M2_HOST_WAIT=5 \
    bash "$SP/m2/run_m2.sh"
# M5: a held range fence beside uncovered point writers.
step m5 env M5_WORKDIR_BASE="$RUN/m5" M5_OPS=1000 M5_BLOCKS=6 \
    bash "$SP/m5/run_m5.sh"
# M6 (scripted half): release before the group sync, with the strict control.
step m6 env RUN_ROOT="$RUN/m6" OUT="$OUTROOT/m6" \
    bash "$SP/m6/run_m6_script.sh"
# M4: contended against disjoint at 8 sessions, base only.
step m4 env RUN_ROOT="$RUN/m1m4" OUT="$OUTROOT/m1m4-m4" CELLS=m4 M4_OPS=4000 \
    bash "$SP/m1m4/run_m1m4.sh"

phase "DONE elapsed=$(( $(date +%s) - T0 ))s"
