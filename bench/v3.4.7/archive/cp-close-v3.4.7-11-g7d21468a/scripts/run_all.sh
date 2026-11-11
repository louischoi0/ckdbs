#!/bin/bash
# CP close measurement schedule (interleaved A/B, base cc54a296 vs close 7d21468a).
# Status file + heartbeat for the 60 s watchdog.
set -uo pipefail
SP=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RUN=/home/louis/bench-runs/cp-close/$STAMP
OUTROOT=$SP/run/$STAMP
mkdir -p "$RUN" "$OUTROOT"
STATUS=$SP/run/status.txt; HB=$SP/run/heartbeat
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
BB=$SP/bin/kds_server-base; BC=$SP/bin/kds_server-close

step m1 env RUN_ROOT="$RUN/m1" OUT="$OUTROOT/m1" CELLS=m1 M1_REPS=3 M1_OPS=40000 bash "$SP/m1m4/run_m1m4.sh"
step m4 env RUN_ROOT="$RUN/m4" OUT="$OUTROOT/m4" CELLS=m4 M4_OPS=4000 bash "$SP/m1m4/run_m1m4.sh"
step m2 env M2_WORKDIR="$RUN/m2/work" M2_OUT="$OUTROOT/m2" M2_RUNS=5 M2_HOST_WAIT=5 bash "$SP/m2/run_m2.sh"
m3() { # label bin skipvar
  local l=$1 b=$2 sk=$3
  step m3-$l env "$sk=1" BIN="$b" WORKROOT="$RUN/m3-$l" OUT="$OUTROOT/m3-$l" HOST_SAMPLER=/nonexistent bash "$SP/m3/run_m3.sh"
}
m3 s2-base-a $BB SKIP_S0
m3 s2-close-a $BC SKIP_S0
m3 s0-close-a $BC SKIP_S2
m3 s0-base-a $BB SKIP_S2
if [ $(( $(date +%s) - T0 )) -lt 1500 ]; then
  m3 s2-close-b $BC SKIP_S0
  m3 s2-base-b $BB SKIP_S0
else
  phase "CUT m3 s2 second pair (budget)"
fi
if [ $(( $(date +%s) - T0 )) -lt 2000 ]; then
  step m5 env M5_WORKDIR_BASE="$RUN/m5" M5_OPS=1000 M5_BLOCKS=6 M5_BIN=$BC bash "$SP/m5/run_m5.sh"
else
  phase "CUT m5 (budget)"
fi
phase "DONE elapsed=$(( $(date +%s) - T0 ))s"
