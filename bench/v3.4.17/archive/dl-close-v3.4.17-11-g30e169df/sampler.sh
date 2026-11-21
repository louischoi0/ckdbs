#!/bin/bash
D=/home/louis/bench-runs/dl-close
while [ ! -f $D/driver.end ]; do
  n=$(ps -eo args | grep -E "cc1plus|cmake --build|ctest|kds_|\.py" | grep -v -E "grep|sampler|dl-close/kds_|dl_close|build-sandbox|sandbox_server|networkd|unattended" | wc -l)
  echo "$(date -u +%T) load=$(cut -d' ' -f1 /proc/loadavg) foreign=$n $(cat $D/run.status 2>/dev/null)" >> $D/sampler.log
  sleep 5
done
