#!/bin/bash
# CP close extra schedule (budget had room after the main pass): M5 on base + repeats,
# M3 scenario 0 second pair, scenario 2 third pair.
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
trap 'kill $HBPID 2>/dev/null; touch $SP/run/host.stop2' EXIT
phase "START-EXTRA"
rm -f $SP/run/host.stop2; bash "$SP/host_sampler.sh" "$OUTROOT/host-extra.log" "$SP/run/host.stop2" &
T0=$(date +%s)
step() { local name=$1; shift; phase "BEGIN $name"; "$@" > "$OUTROOT/$name.console.txt" 2>&1; local rc=$?; phase "END $name rc=$rc elapsed=$(( $(date +%s) - T0 ))s"; }
BB=$SP/bin/kds_server-base; BC=$SP/bin/kds_server-close
m3() { local l=$1 b=$2 sk=$3; step m3-$l env "$sk=1" BIN="$b" WORKROOT="$RUN/m3-$l" OUT="$OUTROOT/m3-$l" HOST_SAMPLER=/nonexistent bash "$SP/m3/run_m3.sh"; }
m5() { local l=$1 b=$2; step m5-$l env M5_WORKDIR_BASE="$RUN/m5-$l" M5_OPS=1000 M5_BLOCKS=6 M5_BIN=$b bash "$SP/m5/run_m5.sh"; }
m5 base-a $BB
m5 close-b $BC
m5 base-b $BB
m3 s0-base-b $BB SKIP_S2
m3 s0-close-b $BC SKIP_S2
m3 s2-base-c $BB SKIP_S0
m3 s2-close-c $BC SKIP_S0
phase "DONE-EXTRA elapsed=$(( $(date +%s) - T0 ))s"
