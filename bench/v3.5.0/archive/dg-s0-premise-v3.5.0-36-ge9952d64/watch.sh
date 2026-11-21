#!/bin/bash
# The 60 s watchdog (CLAUDE.md): polls the run's progress file, never a
# process pattern. Prints one line a minute, ORPHANED when the file has not
# moved for 180 s (a cell runs about 25 s; a gate hold writes every 5 s), and
# FINISHED on the run's last line.
PROG=$1
while true; do
  sleep 60
  if grep -q "ALL DONE" "$PROG" 2>/dev/null; then echo "FINISHED $(tail -1 "$PROG")"; exit 0; fi
  age=$(( $(date +%s) - $(stat -c %Y "$PROG") ))
  if [ "$age" -gt 180 ]; then echo "ORPHANED age=${age}s last=[$(tail -1 "$PROG")]"; exit 1; fi
  echo "ok age=${age}s cells_done=$(grep -c ' done ' "$PROG") last=[$(tail -1 "$PROG" | cut -c1-90)]"
done
