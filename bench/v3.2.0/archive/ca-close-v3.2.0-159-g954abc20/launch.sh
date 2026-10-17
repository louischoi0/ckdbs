#!/bin/bash
D=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-close-v3.2.0-159-g954abc20
date -u > $D/start.txt
nohup $D/ca-close-run.sh $D/runs > $D/run.out 2>&1 &
