#!/bin/bash
cd /home/louis/bench-runs/dh-close
while ! grep -q finished run.out 2>/dev/null; do
  sleep 60
  age=$(( $(date +%s) - $(stat -c %Y wd/progress.log 2>/dev/null || echo 0) ))
  echo "$(date -u +%T) progress age ${age}s $(tail -1 wd/progress.log 2>/dev/null | cut -c1-90)" >> watchdog.log
  if [ -f build.done ] && [ $age -gt 240 ] && ! kill -0 $(cat run.pid) 2>/dev/null && ! grep -q finished run.out 2>/dev/null; then
    echo "$(date -u +%T) orphaned: resuming" >> watchdog.log
    nohup bash run.sh >/dev/null 2>&1 &
  fi
done
