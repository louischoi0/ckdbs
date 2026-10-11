#!/bin/bash
# Print, for my running kds_server-B (if any), each thread's CPU and affinity. Waits up to 60 s for one to appear.
for i in $(seq 1 60); do
  pid=$(pgrep -x kds_server-B | head -n 1)
  if [ -n "$pid" ]; then
    for t in /proc/$pid/task/*; do
      tid=$(basename $t)
      echo "tid $tid $(cat $t/comm) psr=$(awk '{print $39}' $t/stat) $(taskset -cp $tid)"
    done
    exit 0
  fi
  sleep 1
done
echo "no server seen"
