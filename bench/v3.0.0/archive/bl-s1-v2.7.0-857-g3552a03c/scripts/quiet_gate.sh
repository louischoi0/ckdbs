#!/bin/sh
# Blocks until the host is quiet: no compiler, build or test run of any
# session, a 1-minute load below 4.5, and no `hold` file (this session's own
# pause, taken around its builds). bench/README.md rule 4, enforced rather
# than only recorded: this host is shared with other sessions.
while true; do
  busy=$(pgrep -f "cc1plus|cmake --build|ctest|kds_tests|gtest" | wc -l)
  load=$(cut -d' ' -f1 /proc/loadavg)
  if [ ! -f /home/cdkbs/bench-runs/bl/hold ] && [ "$busy" -eq 0 ] && awk -v l="$load" 'BEGIN{exit !(l < 4.5)}'; then
    exit 0
  fi
  sleep 15
done
