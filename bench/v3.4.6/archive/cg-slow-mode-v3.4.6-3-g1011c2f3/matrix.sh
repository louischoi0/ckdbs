#!/bin/bash
# CG slow-mode attribution: A (f44846ef, engine a), B (6775aec1, engine b),
# Ba (6775aec1, engine a - B's binary through A's phases). cores=1, relaxed,
# server pinned to CPU 4 (physical core 4-5), driver to CPU 6; 10 reps,
# rotated (A B Ba / B Ba A / Ba A B). A cell whose JSON exists is skipped, so
# a relaunch resumes. Each cell samples competitors every 0.5 s while it runs.
set -u
WT=/home/cdkbs/ckdbs/.claude/worktrees/cg-slow-mode-attribution
BIN=/home/cdkbs/bench-runs/cg/bin
RUN=/home/cdkbs/bench-runs/cg-slow/run
OUT=$1
PROG=$OUT/progress.txt
PORT=15611
mkdir -p "$OUT" "$RUN"
competing() {
  echo $(( $(pgrep -c -x kds_tests) + $(pgrep -c -x cc1plus) + $(pgrep -c -x ctest) ))
}
quiet() {
  local waited=0
  while [ "$(competing)" -gt 0 ] || [ "$(cut -d. -f1 /proc/loadavg)" -ge 2 ]; do
    sleep 5; waited=$((waited + 5)); touch "$PROG"
    [ $waited -ge 900 ] && break
  done
  [ $waited -gt 0 ] && echo "$(date -u +%T) gate waited ${waited}s" >> "$PROG"
}
cell() {  # arm rep
  local arm=$1 rep=$2 tag="cg-$1-r$2" bin engine
  [ -f "$OUT/$tag.json" ] && return
  case $arm in
    A)  bin=$BIN/kds_server.f44846ef; engine=a ;;
    B)  bin=$BIN/kds_server.6775aec1; engine=b ;;
    Ba) bin=$BIN/kds_server.6775aec1; engine=a ;;
  esac
  local wd=$RUN/$tag
  quiet
  rm -rf "$wd"; mkdir -p "$wd"
  printf 'data_file = %s/d.db\nport = %s\ncores = 1\ndurability = relaxed\nbuffer_pool_frames = 131072\nlog_file = s.log\nlog_dir = %s\nlog_level = warn\n' \
    "$wd" "$PORT" "$wd" > "$wd/s.conf"
  echo "$(date -u +%T) start $tag load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$PROG"
  taskset -c 4 "$bin" --config "$wd/s.conf" > "$wd/s.stderr" 2>&1 &
  local pid=$!
  for i in $(seq 1 300); do
    if python3 -c "import socket,sys;s=socket.socket();sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then break; fi
    sleep 0.1
  done
  ( while kill -0 "$pid" 2>/dev/null; do echo "$(date -u +%T.%N) $(competing) $(cut -d' ' -f1 /proc/loadavg)"; sleep 0.5; done ) > "$OUT/$tag.watch" &
  local watcher=$!
  taskset -c 6 python3 "$WT/tools/cg_close_benchmark.py" --port "$PORT" --engine "$engine" \
    --sizes 1000,10000 --ops 1000 --seed "$rep" --server-pid "$pid" --watch-cpus 4,5 \
    --json "$OUT/$tag.json.tmp" > "$OUT/$tag.console" 2>&1
  local rc=$?
  kill "$pid"; wait "$pid" 2>/dev/null
  kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
  [ $rc -eq 0 ] && mv "$OUT/$tag.json.tmp" "$OUT/$tag.json"
  echo "$(date -u +%T) done $tag rc=$rc maxcompeting=$(awk '{print $2}' "$OUT/$tag.watch" | sort -n | tail -1)" >> "$PROG"
  rm -rf "$wd"
}
for rep in $(seq 1 10); do
  case $(( (rep - 1) % 3 )) in
    0) order="A B Ba" ;;
    1) order="B Ba A" ;;
    2) order="Ba A B" ;;
  esac
  for arm in $order; do cell "$arm" "$rep"; done
done
echo "$(date -u +%T) MATRIX-END" >> "$PROG"
touch "$OUT/done"
