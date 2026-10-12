#!/bin/sh
nohup bash /home/cdkbs/bench-runs/bp-s1/v2/after_s2.sh > /home/cdkbs/bench-runs/bp-s1/v2/out/after_s2.console 2>&1 &
echo $! > /home/cdkbs/bench-runs/bp-s1/v2/out/after_s2.pid
echo "after_s2 pid $!"
