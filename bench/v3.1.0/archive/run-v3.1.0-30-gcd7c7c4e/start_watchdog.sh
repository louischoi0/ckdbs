#!/bin/bash
RUN=/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e
setsid nohup bash "$RUN/watchdog.sh" > /dev/null 2>&1 < /dev/null &
