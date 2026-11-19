#!/bin/bash
# 60 s watchdog over the progress file (never a process pattern). Exits 0 at
# the end marker, 3 when the last line has not moved for 4 minutes (orphaned).
P=$1
last=""; stall=0
while true; do
  sleep 60
  cur=$(tail -1 "$P" 2>/dev/null)
  if [ "$cur" = "$last" ]; then stall=$((stall + 1)); else stall=0; fi
  last=$cur
  echo "watchdog $(date -u +%T): ${cur:-<no progress yet>} (unchanged ${stall}m)"
  fail=$(grep 'rc=[1-9]' "$P" 2>/dev/null | tail -1)
  [ -n "$fail" ] && echo "FAILED CELL: $fail"
  case "$cur" in *CZCLOSE-END*) exit 0;; esac
  [ "$stall" -ge 4 ] && { echo "STALLED"; exit 3; }
done
