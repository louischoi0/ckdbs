#!/bin/bash
# Host-state sampler for the CP-S0 run (bench/README.md rule 4, CI-S0's
# host.log): every 2 s, the UTC time, /proc/loadavg and the count and
# names of compiler / test / cmake processes on the host. Stops when the
# stop file appears.
OUT="$1"; STOP="$2"
while [ ! -e "$STOP" ]; do
  procs=$(pgrep -a -f 'cc1plus|cmake --build|ctest|kds_tests|clang\+\+|g\+\+' | grep -v 'host_sampler' | grep -v 'pgrep' | wc -l)
  names=$(pgrep -a -f 'cc1plus|cmake --build|ctest|kds_tests' | grep -v 'host_sampler' | grep -v 'pgrep' | awk '{print $2}' | sort | uniq -c | tr '\n' ' ')
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') load=$(cut -d' ' -f1-3 /proc/loadavg) procs=$procs $names" >> "$OUT"
  sleep 2
done
