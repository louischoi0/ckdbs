#!/bin/bash
D=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-close-v3.2.0-159-g954abc20
date -u > $D/prof-start.txt
( $D/ca-close-prof.sh $D/prof > $D/prof.out 2>&1
  $D/ca-close-s3reads.sh $D/prof > $D/prof-s3.out 2>&1 ) &
