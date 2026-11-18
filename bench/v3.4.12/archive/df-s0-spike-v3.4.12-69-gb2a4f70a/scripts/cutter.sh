#!/bin/bash
# Budget cut: w4 does not fit the 20-minute measurement budget; stop it the moment it starts.
R=/home/louis/bench-runs/df-s0
until grep -q "phase start w4" $R/progress.txt; do sleep 2; done
pkill -f "bn_census.py .*--port 34051"
pkill -f "df-s0/run.sh"
sleep 3
pkill -f "bn_census.py .*--port 34051"
echo "$(date -u '+%F %T UTC') w4 CUT for budget (killed at its start, nothing kept)" >> $R/progress.txt
echo "$(date -u '+%F %T UTC') DONE" >> $R/progress.txt
