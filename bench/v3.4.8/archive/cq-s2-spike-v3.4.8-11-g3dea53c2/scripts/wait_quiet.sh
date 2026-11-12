#!/usr/bin/env bash
# Wait until no build or test runs on the host and the 1-minute load is
# under 1.5, or until LIMIT polls of 5 s have passed.
LIMIT=${1:-96}
n=0
while [ $n -lt "$LIMIT" ]; do
  busy=$(pgrep -f 'cc1plus|cmake --build|ctest|kds_tests' | wc -l)
  load=$(cut -d' ' -f1 /proc/loadavg)
  if [ "$busy" -eq 0 ] && awk -v l="$load" 'BEGIN{exit !(l<1.5)}'; then break; fi
  sleep 5; n=$((n+1))
done
echo "waited $((n*5))s load $(cat /proc/loadavg) competing $(pgrep -f 'cc1plus|cmake --build|ctest|kds_tests' | wc -l)"
