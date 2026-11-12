#!/usr/bin/env bash
# CQ-S2's schedule (workorder-cq-column-reservation.md section 6, at
# CQ-Q15 (a)'s scope). Three servers alternate, one driver call each:
#   plain  - the spike's binary, no mark: S0's arms as they are today
#   marked - the spike's binary, `cq_acct.c` marked: the hot arm's debit
#            is a column reservation ("reserved")
#   base   - the binary built from the commit the spike started from
#            (the off-path A/B, CQ-R1)
#   ROOT=<worktree> OUT=<outputs> RUN=<block-device dir> BIN=<spike server>
#   BASE=<base server> DESCRIBE=<describe> ./run_s2.sh [smoke]
set -uo pipefail
SRV=$ROOT/bench/v3.4.7/archive/cp-s0-premise-v3.4.7-1-gcc54a296/scripts/m1m4_srv.py
DRV=$ROOT/tools/cq_s0_premise.py
MODE=${1:-full}
MARK='cq_acct.c:-1000000000000'
SECS=6; REPS=3; PAUSE_SECS=20; KS=2,8
if [ "$MODE" = smoke ]; then SECS=1; REPS=1; PAUSE_SECS=4; KS=8; fi
mkdir -p "$OUT" "$RUN"
say() { echo "$(date -u +%H:%M:%S) $*" >> "$OUT/status.txt"; date +%s > "$OUT/heartbeat"; }
start() { # label durability port binary mark-or-empty
  if [ -n "$5" ]; then export KDS_CQ_SPIKE_MARK="$5"; else unset KDS_CQ_SPIKE_MARK; fi
  python3 "$SRV" start --binary "$4" --workdir "$RUN/$1" --port "$3" --cores 4 \
     --taskset 2-5 --durability "$2" --label "$1" > "$OUT/$1.start.json" 2>"$OUT/$1.start.err" \
     || { say "FAILED to start $1"; cat "$OUT/$1.start.err" >> "$OUT/status.txt"; return 1; }
  unset KDS_CQ_SPIKE_MARK
  PID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['pid'])" "$OUT/$1.start.json")
}
stop() { python3 "$SRV" stop --workdir "$RUN/$1" > "$OUT/$1.stop.json" 2>&1; cp "$RUN/$1/server.json" "$OUT/$1.server.json"; }
drv() { taskset -c 0,1,6,7 python3 "$DRV" --server-pid "$PID" --heartbeat "$OUT/heartbeat" --status "$OUT/status.txt" "$@"; }
cell() { # label durability port binary mark json-name driver-args...
  local label=$1 dur=$2 port=$3 bin=$4 mark=$5 name=$6; shift 6
  start "$label" "$dur" "$port" "$bin" "$mark" || return
  drv --port "$port" --label "$label" --json "$OUT/$name.json" "$@" > "$OUT/$name.txt" 2>&1 || say "$name driver exit $?"
  stop "$label"
}

say "schedule start mode=$MODE describe=$DESCRIBE load=$(cut -d' ' -f1-3 /proc/loadavg)"
for r in $(seq 1 $REPS); do
  cell plain-$r  group 15550 "$BIN"  ""      m1-plain-$r  --k $KS --arms hot,hot-last-piped,disjoint --reps 1 --seconds $SECS
  cell marked-$r group 15551 "$BIN"  "$MARK" m1-marked-$r --k $KS --arms hot --reps 1 --seconds $SECS
  cell base-$r   group 15552 "$BASE" ""      ab-base-$r   --k $KS --arms hot,hot-last-piped,disjoint --reps 1 --seconds $SECS
done
cell marked-pause group 15553 "$BIN" "$MARK" m4p-marked --k 0 --arms hot --reps 1 --seconds $PAUSE_SECS --pause-ms 1200
cell plain-pause  group 15554 "$BIN" ""      m4p-plain  --k 0 --arms hot --reps 1 --seconds $PAUSE_SECS --pause-ms 1200
cell plain-strict  strict 15555 "$BIN" ""      m2-plain  --k 8 --arms hot,hot-last-piped,disjoint --reps 2 --seconds $SECS
cell marked-strict strict 15556 "$BIN" "$MARK" m2-marked --k 8 --arms hot --reps 2 --seconds $SECS
say "DONE load=$(cut -d' ' -f1-3 /proc/loadavg)"
