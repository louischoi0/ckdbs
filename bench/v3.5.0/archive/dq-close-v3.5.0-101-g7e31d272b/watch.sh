#!/bin/bash
# usage: watch.sh heartbeat_file pid_file out_file ; every 60 s writes a state line
HB=$1; PIDF=$2; OUT=$3
while true; do
  now=$(date +%s)
  mt=$(stat -c %Y "$HB" 2>/dev/null || echo 0)
  pid=$(cat "$PIDF" 2>/dev/null)
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then alive=alive; else alive=gone; fi
  echo "$(date -u +%H:%M:%S) driver=$alive heartbeat_age=$((now-mt))s" >> "$OUT"
  [ "$alive" = gone ] && exit 0
  sleep 60
done
