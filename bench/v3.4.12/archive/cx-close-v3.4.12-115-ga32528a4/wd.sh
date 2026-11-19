#!/bin/bash
# watchdog: every 60 s write liveness of the driver pid to a file
# usage: wd.sh <pid> <out> <statusfile>
pid=$1; out=$2
while kill -0 "$pid" 2>/dev/null; do
  echo "$(date -u +%FT%TZ) alive $(cat "$3" 2>/dev/null)" >> "$out"
  sleep 60
done
echo "$(date -u +%FT%TZ) driver exited: $(cat "$3" 2>/dev/null)" >> "$out"
