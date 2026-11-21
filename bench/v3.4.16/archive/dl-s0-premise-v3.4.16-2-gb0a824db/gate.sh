#!/bin/bash
# waits until no build/suite runs, then 45 s settle; writes gate.status
S=/home/louis/bench-runs/dl-s0/gate.status
t0=$(date +%s)
while pgrep -f "cc1plus|cmake --build|ctest" | grep -v -w $$ >/dev/null; do
  echo "held $(( $(date +%s)-t0 ))s $(date -u +%T)" > $S; sleep 10; done
echo "clear $(date -u +%T) held=$(( $(date +%s)-t0 ))" > $S
sleep 45
echo "settled $(date -u +%T) held=$(( $(date +%s)-t0 )) load=$(cut -d' ' -f1 /proc/loadavg)" > $S
