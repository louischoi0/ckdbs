#!/bin/bash
# CG-S3 close: A (f44846ef, v3.4.3) vs B (6775aec1), cores=1, relaxed; 10 reps, A B / B A alternating.
set -u
WT=/home/cdkbs/ckdbs/.claude/worktrees/cg-on-conflict-do-update
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
BIN=/home/cdkbs/bench-runs/cg/bin
RUN=/home/cdkbs/bench-runs/cg/run
OUT=$1
PROG=$SP/cg-matrix.progress
PORT=15601
mkdir -p "$OUT" "$RUN"
: > "$PROG"
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
  done
  [ $waited -gt 0 ] && echo "$(date -u +%T) gate waited ${waited}s" >> "$PROG"
}
cell() {  # arm rep
  local arm=$1 rep=$2 tag="cg-$1-r$2" bin engine
  [ "$arm" = A ] && { bin=$BIN/kds_server.f44846ef; engine=a; }
  [ "$arm" = B ] && { bin=$BIN/kds_server.6775aec1; engine=b; }
  local wd=$RUN/$tag
  quiet
  rm -rf "$wd"; mkdir -p "$wd"
  printf 'data_file = %s/d.db\nport = %s\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 131072\nlog_file = s.log\nlog_dir = %s\nlog_level = warn\n' \
    "$wd" "$PORT" "$wd" > "$wd/s.conf"
  echo "$(date -u +%T) start $tag load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$PROG"
  "$bin" --config "$wd/s.conf" > "$wd/s.stderr" 2>&1 &
  local pid=$!
  for i in $(seq 1 300); do
    if python3 -c "import socket,sys;s=socket.socket();sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then break; fi
    sleep 0.1
  done
  python3 "$WT/tools/cg_close_benchmark.py" --port "$PORT" --engine "$engine" --sizes 1000,10000 \
    --ops 1000 --seed "$rep" --json "$OUT/$tag.json" > "$OUT/$tag.console" 2>&1
  local rc=$?
  kill "$pid"; wait "$pid" 2>/dev/null
  echo "$(date -u +%T) done $tag rc=$rc" >> "$PROG"
  rm -rf "$wd"
}
for rep in 1 2 3 4 5 6 7 8 9 10; do
  if [ $((rep % 2)) -eq 1 ]; then cell A $rep; cell B $rep; else cell B $rep; cell A $rep; fi
done
echo "$(date -u +%T) MATRIX-END" >> "$PROG"
