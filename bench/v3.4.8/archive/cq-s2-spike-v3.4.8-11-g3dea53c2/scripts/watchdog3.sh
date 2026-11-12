#!/usr/bin/env bash
# The 60 s watchdog: polls the schedule's heartbeat *file*, never a process
# pattern. Exits when the schedule says DONE, or when the heartbeat is older
# than STALL seconds (the caller is then notified and resumes).
#   OUT=<outputs> ./watchdog.sh
STALL=${STALL:-150}
until [ -f "$OUT/heartbeat" ]; do sleep 10; done
while true; do
  sleep 60
  now=$(date +%s)
  beat=$(cat "$OUT/heartbeat" 2>/dev/null || echo 0); beat=${beat%.*}
  last=$(tail -1 "$OUT/status.txt" 2>/dev/null)
  echo "$(date -u +%H:%M:%S) age=$((now-beat))s last='$last'" >> "$OUT/watchdog.txt"
  if grep -q ' DONE ' "$OUT/status.txt" 2>/dev/null; then echo "watchdog: DONE"; exit 0; fi
  if [ $((now-beat)) -gt "$STALL" ]; then echo "watchdog: STALLED age=$((now-beat))s last='$last'"; exit 1; fi
done
