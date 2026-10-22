#!/bin/bash
# BL-S5 statement-path overhead: A (0af06bbc) vs B (f4bad273) at cores 1 and 4,
# and B vs N (B, balancer off) at cores 4. 5 reps, arm order rotated per rep.
set -u
WT=/home/cdkbs/ckdbs/.claude/worktrees/bl-s4-partition
SP=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bs-test-hardening/f973f5e5-b744-42a3-8ccb-fd3d80d2584f/scratchpad
BIN=/home/cdkbs/bench-runs/bl/bin
RUN=/home/cdkbs/bench-runs/bl/s5
OUT=$1
PROG=$SP/s5-stmt.progress
PORT=15591
mkdir -p "$OUT" "$RUN"
: > "$PROG"
# The quiet gate: no other session's test binary or compiler, and a 1-minute
# load under 2, before every cell (another session's suite overlapped once).
quiet() {
  local waited=0
  while pgrep -x kds_tests > /dev/null || pgrep -x cc1plus > /dev/null ||
        [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5))
  done
  [ $waited -gt 0 ] && echo "$(date -u +%T) gate waited ${waited}s" >> "$PROG"
}
bin_of() { case $1 in A) echo "$BIN/kds_server.0af06bbc";; B|N) echo "$BIN/kds_server.f4bad273-nobal";; esac; }
cell() {  # arm cores rep
  local arm=$1 cores=$2 rep=$3 tag="stmt-$1-k$2-r$3"
  local wd=$RUN/$tag bin
  bin=$(bin_of "$arm")
  [ "$arm" = A ] && bin=$BIN/kds_server.0af06bbc
  [ "$arm" = B ] && bin=$BIN/kds_server.f4bad273
  quiet
  rm -rf "$wd"; mkdir -p "$wd"
  printf 'data_file = %s/d.db\nport = %s\ncores = %s\ndurability = relaxed\nbuffer_pool_frames = 131072\nlog_file = s.log\nlog_dir = %s\nlog_level = warn\n' \
    "$wd" "$PORT" "$cores" "$wd" > "$wd/s.conf"
  echo "$(date -u +%T) start $tag load=$(cut -d' ' -f1-3 /proc/loadavg) cc=$(pgrep -c cc1plus)" >> "$PROG"
  if [ "$arm" = N ]; then KDS_BL_NO_BALANCE=1 "$bin" --config "$wd/s.conf" > "$wd/s.stderr" 2>&1 &
  else "$bin" --config "$wd/s.conf" > "$wd/s.stderr" 2>&1 & fi
  local pid=$!
  for i in $(seq 1 300); do
    if python3 -c "import socket,sys;s=socket.socket();sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then break; fi
    sleep 0.1
  done
  python3 "$WT/tools/a1_close_benchmark.py" --port "$PORT" --mode stmt --sizes 200,1000,10000 \
    --ops 1000 --scan-ops 20 --seed "$rep" --json "$OUT/$tag.json" > "$OUT/$tag.console" 2>&1
  local rc=$?
  kill "$pid"; wait "$pid" 2>/dev/null
  echo "$(date -u +%T) done $tag rc=$rc" >> "$PROG"
  rm -rf "$wd"
}
for rep in 1 2 3 4 5; do
  if [ $((rep % 2)) -eq 1 ]; then cell A 1 $rep; cell B 1 $rep; else cell B 1 $rep; cell A 1 $rep; fi
  case $((rep % 3)) in
    1) cell A 4 $rep; cell B 4 $rep; cell N 4 $rep;;
    2) cell B 4 $rep; cell N 4 $rep; cell A 4 $rep;;
    0) cell N 4 $rep; cell A 4 $rep; cell B 4 $rep;;
  esac
done
echo "$(date -u +%T) STMT-END" >> "$PROG"
