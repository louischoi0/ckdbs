#!/bin/bash
# Exits (re-invoking the session) when stage A is done, when everything is done, or after 20 minutes.
R=/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b
WANT=${1:-rowcost}
for i in $(seq 1 120); do
    [ -e $R/all.done ] && { echo "all done"; exit 0; }
    [ "$WANT" = rowcost ] && [ -e $R/done/rowcost ] && { echo "stage A done"; exit 0; }
    sleep 10
done
echo timeout
