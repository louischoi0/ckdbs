#!/bin/bash
set -uo pipefail
SP=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close
STAMP=$(cat $SP/run/current_stamp)
RUN=/home/louis/bench-runs/cp-close/$STAMP
OUTROOT=$SP/run/$STAMP
mkdir -p "$RUN"
now() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
echo "$(now) START-S0C" >> $SP/run/status.txt
rm -f $SP/run/host.stop4; bash $SP/host_sampler.sh "$OUTROOT/host-s0c.log" $SP/run/host.stop4 &
for pair in "base-c:$SP/bin/kds_server-base" "close-c:$SP/bin/kds_server-close"; do
  l=${pair%%:*}; b=${pair#*:}
  echo "$(now) BEGIN m3-s0-$l" >> $SP/run/status.txt
  env SKIP_S2=1 BIN="$b" WORKROOT="$RUN/m3-s0-$l" OUT="$OUTROOT/m3-s0-$l" HOST_SAMPLER=/nonexistent bash $SP/m3/run_m3.sh > "$OUTROOT/m3-s0-$l.console.txt" 2>&1
  echo "$(now) END m3-s0-$l rc=$?" >> $SP/run/status.txt
done
touch $SP/run/host.stop4
echo "$(now) DONE-S0C" >> $SP/run/status.txt
