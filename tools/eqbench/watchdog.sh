#!/bin/bash
# watchdog.sh <out-dir> <stall-minutes> -- <matrix.py arguments>
# Every 60 s: if <out-dir>/<pass>.done exists, exit; if matrix.py's pid (from
# its pid file, never a process pattern) is gone, or its status files have not
# moved for <stall-minutes>, kill what is left and launch it again with the
# same arguments - it resumes at the first cell with no JSON.
set -u
out=$1; stall=$2; shift 3
pass=timed; prev=""
for a in "$@"; do [ "$prev" = "--pass" ] 2>/dev/null && pass=$a; prev=$a; done
launch() {
  nohup python3 "$(dirname "$0")/matrix.py" "$@" >> "$out/matrix.log" 2>&1 &
  echo "$(date -u +%FT%TZ) launched $!" >> "$out/watchdog.log"
}
mkdir -p "$out"
[ -e "$out/$pass.done" ] && exit 0
pid=$(cat "$out/matrix.pid" 2>/dev/null || echo 0)
kill -0 "$pid" 2>/dev/null || launch "$@"
while true; do
  sleep 60
  [ -e "$out/$pass.done" ] && { echo "$(date -u +%FT%TZ) done" >> "$out/watchdog.log"; exit 0; }
  pid=$(cat "$out/matrix.pid" 2>/dev/null || echo 0)
  newest=$(stat -c %Y "$out/status" "$out/status.cell" 2>/dev/null | sort -n | tail -1)
  age=$(( $(date +%s) - ${newest:-0} ))
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "$(date -u +%FT%TZ) matrix pid $pid gone, resuming" >> "$out/watchdog.log"
    launch "$@"
  elif [ "$age" -gt $(( stall * 60 )) ]; then
    echo "$(date -u +%FT%TZ) status still for ${age}s, restarting" >> "$out/watchdog.log"
    pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; sleep 2
    launch "$@"
  else
    echo "$(date -u +%FT%TZ) alive pid $pid, status age ${age}s" >> "$out/watchdog.log"
  fi
done
