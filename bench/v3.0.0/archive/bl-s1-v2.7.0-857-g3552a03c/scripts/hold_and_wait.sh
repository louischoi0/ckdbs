#!/bin/sh
# Takes this session's pause (`hold`) and waits for the run in progress, if
# any, to end - so a build that follows lands between runs, never inside one.
touch /home/cdkbs/bench-runs/bl/hold
while ps -eo args | grep -E "^python3 .*(bl_mixed|ba_census)\.py --bin" > /dev/null; do sleep 2; done
echo "held; no run in progress"
