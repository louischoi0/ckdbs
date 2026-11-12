#!/usr/bin/env bash
# CQ-S0's schedule (workorder-cq-column-reservation.md section 6). One
# binary; two servers, `group` then `strict`; the arms differ only in what
# the client sends. Heartbeat and status are the driver's; DONE ends it.
#   ROOT=<worktree> OUT=<dir for outputs> RUN=<block-device dir for data> \
#   BIN=<kds_server> DESCRIBE=<describe> ./run_s0.sh [smoke]
set -uo pipefail
SRV=$ROOT/bench/v3.4.7/archive/cp-s0-premise-v3.4.7-1-gcc54a296/scripts/m1m4_srv.py
DRV=$ROOT/tools/cq_s0_premise.py
MODE=${1:-full}
SECS=6; REPS=3; PAUSE_SECS=20; KS=0,2,8
if [ "$MODE" = smoke ]; then SECS=1; REPS=1; PAUSE_SECS=4; KS=0,8; fi
mkdir -p "$OUT" "$RUN"
say() { echo "$(date -u +%H:%M:%S) $*" >> "$OUT/status.txt"; date +%s > "$OUT/heartbeat"; }
start() { # label durability port
  python3 "$SRV" start --binary "$BIN" --workdir "$RUN/$1" --port "$3" --cores 4 \
     --taskset 2-5 --durability "$2" --label "$1" > "$OUT/$1.start.json" 2>"$OUT/$1.start.err" \
     || { say "FAILED to start $1"; cat "$OUT/$1.start.err" >> "$OUT/status.txt"; return 1; }
  PID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['pid'])" "$OUT/$1.start.json")
}
stop() { python3 "$SRV" stop --workdir "$RUN/$1" > "$OUT/$1.stop.json" 2>&1; cp "$RUN/$1/server.json" "$OUT/$1.server.json"; }
drv() { taskset -c 0,1,6,7 python3 "$DRV" --server-pid "$PID" --heartbeat "$OUT/heartbeat" --status "$OUT/status.txt" "$@"; }

say "schedule start mode=$MODE describe=$DESCRIBE load=$(cut -d' ' -f1-3 /proc/loadavg)"
start group group 15540 && {
  drv --port 15540 --label group --k $KS --reps $REPS --seconds $SECS --json "$OUT/m1-group.json" > "$OUT/m1-group.txt" 2>&1 || say "m1 driver exit $?"
  drv --port 15540 --label group-pause --k 0 --arms hot --reps 1 --seconds $PAUSE_SECS --pause-ms 1200 --json "$OUT/m4p-group.json" > "$OUT/m4p-group.txt" 2>&1 || say "m4p driver exit $?"
  stop group
}
start strict strict 15542 && {
  drv --port 15542 --label strict --k 8 --reps $REPS --seconds $SECS --json "$OUT/m2-strict.json" > "$OUT/m2-strict.txt" 2>&1 || say "m2 driver exit $?"
  stop strict
}
say "DONE load=$(cut -d' ' -f1-3 /proc/loadavg)"
