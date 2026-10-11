#!/bin/bash
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
rm -f $SP/run/runner.done
nohup setsid bash -c "python3 $SP/runner.py null2 >> $SP/run/runner.log 2>&1; touch $SP/run/runner.done" > /dev/null 2>&1 < /dev/null &
echo "launched $!"
