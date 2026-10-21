#!/bin/sh
# CF-S0 launcher: the driver detached, plus a detached 60 s watchdog that
# relaunches it when it dies or its progress file goes stale past 300 s.
# The driver skips every (arm, rep) whose result file exists, so a relaunch
# resumes. Usage: sh run.sh BINARY OUT [driver args...]
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$1; OUT=$2; shift 2
DATA=/home/cdkbs/bench-runs/cf/data
mkdir -p "$OUT"
rm -f "$OUT/done" "$OUT/watchdog.done"
cat > "$OUT/launch.sh" <<EOF
#!/bin/sh
python3 "$HERE/cf_s0.py" --binary "$BIN" --out "$OUT" --data "$DATA" $* >> "$OUT/driver.log" 2>&1 &
echo \$! > "$OUT/driver.pid"
touch "$OUT/progress"
EOF
cat > "$OUT/watchdog.sh" <<EOF
#!/bin/sh
sh "$OUT/launch.sh"
while [ ! -f "$OUT/done" ]; do
  sleep 60
  [ -f "$OUT/done" ] && break
  PID=\$(cat "$OUT/driver.pid")
  AGE=\$(( \$(date +%s) - \$(stat -c %Y "$OUT/progress") ))
  echo "\$(date -u '+%H:%M:%S') pid=\$PID age=\${AGE}s results=\$(ls "$OUT"/*-r*.json 2>/dev/null | wc -l)" >> "$OUT/watchdog.log"
  if ! kill -0 "\$PID" 2>/dev/null || [ "\$AGE" -gt 300 ]; then
    echo "\$(date -u '+%H:%M:%S') relaunch (alive=\$(kill -0 \$PID 2>/dev/null && echo 1 || echo 0) age=\$AGE)" >> "$OUT/watchdog.log"
    kill "\$PID" 2>/dev/null
    for S in \$(pgrep -f "$OUT/[A-C]-r"); do kill "\$S" 2>/dev/null; done
    sh "$OUT/launch.sh"
  fi
done
touch "$OUT/watchdog.done"
EOF
nohup sh "$OUT/watchdog.sh" > /dev/null 2>&1 &
