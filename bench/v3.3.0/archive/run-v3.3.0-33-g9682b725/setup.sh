#!/bin/bash
WT=/home/cdkbs/ckdbs/.claude/worktrees/bench-scenarios-1-4
S=$WT/bench/v3.1.0/archive/run-v3.1.0-30-gcd7c7c4e
R=/home/cdkbs/bench-runs/v3.3.0-33-g9682b725
mkdir -p $R/analysis
for f in lib.sh monitor.sh watchdog.sh cpusample.sh; do
  sed -e 's#v3.1.0-30-gcd7c7c4e#v3.3.0-33-g9682b725#g' -e 's#worktrees/bench-v3.1.0-scenarios/tools#worktrees/bench-scenarios-1-4/tools#' -e 's#run_all5.sh#run_all.sh#' $S/$f > $R/$f
done
for f in mdlib.py cellvalid.py; do
  sed -e 's#v3.1.0-30-gcd7c7c4e#v3.3.0-33-g9682b725#g' $S/analysis/$f > $R/analysis/$f
done
grep -n "TOOLS=\|RUN=" $R/lib.sh
