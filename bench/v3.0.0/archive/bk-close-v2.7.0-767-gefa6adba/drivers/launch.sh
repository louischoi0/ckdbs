#!/bin/bash
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
nohup setsid bash $SP/run_all.sh > $SP/run_all.out 2>&1 < /dev/null &
echo "launched $!"
