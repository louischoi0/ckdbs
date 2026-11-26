#!/bin/bash
# exit when no real cc1plus / ctest / cmake process (exact names) has existed for 90 s running
q=0
while [ $q -lt 6 ]; do
  if pgrep -x cc1plus >/dev/null || pgrep -x ctest >/dev/null || pgrep -x cmake >/dev/null || pgrep -x make >/dev/null; then q=0; else q=$((q+1)); fi
  sleep 15
done
echo quiet at $(date -u +%T)
