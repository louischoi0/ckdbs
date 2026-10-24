#!/bin/sh
# Launch matrix.sh detached with a 60 s watchdog that relaunches it when it
# dies or its progress file goes stale past 300 s. Usage: sh run.sh OUT
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
mkdir -p "$OUT"
rm -f "$OUT/done" "$OUT/watchdog.done"
cat > "$OUT/watchdog.sh" <<EOF
#!/bin/sh
launch() { nohup bash "$HERE/matrix.sh" "$OUT" > "$OUT/matrix.log" 2>&1 & echo \$! > "$OUT/matrix.pid"; touch "$OUT/progress.txt"; }
launch
while [ ! -f "$OUT/done" ]; do
  sleep 60
  [ -f "$OUT/done" ] && break
  PID=\$(cat "$OUT/matrix.pid")
  AGE=\$(( \$(date +%s) - \$(stat -c %Y "$OUT/progress.txt") ))
  echo "\$(date -u '+%H:%M:%S') pid=\$PID age=\${AGE}s results=\$(ls "$OUT"/*.json 2>/dev/null | wc -l)" >> "$OUT/watchdog.log"
  if ! kill -0 "\$PID" 2>/dev/null || [ "\$AGE" -gt 300 ]; then
    echo "\$(date -u '+%H:%M:%S') relaunch" >> "$OUT/watchdog.log"
    kill "\$PID" 2>/dev/null
    launch
  fi
done
touch "$OUT/watchdog.done"
EOF
nohup sh "$OUT/watchdog.sh" > /dev/null 2>&1 &
