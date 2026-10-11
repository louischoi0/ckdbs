#!/bin/bash
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
rm -f $SP/run/runner4.done
nohup setsid bash -c "until [ -f $SP/run/runner3.done ]; do sleep 5; done; python3 $SP/runner.py fresh >> $SP/run/runner.log 2>&1; touch $SP/run/runner4.done" > /dev/null 2>&1 < /dev/null &
echo "launched $!"
