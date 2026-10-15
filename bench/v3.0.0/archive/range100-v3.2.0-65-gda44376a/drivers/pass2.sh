#!/bin/bash
# second pass: runs 2-4, gated on no ctest running (up to 200 s)
S=/tmp/claude-1000/-home-cdkbs-ckdbs/0221b531-38a9-4c26-b0d4-39c18d514efa/scratchpad/r100
Q=/home/cdkbs/ckdbs/.claude/worktrees/bench-range100-pg/bench/v3.0.0/archive/range100-v3.2.0-65-gda44376a
export RUNS="2 3 4" GATE=200
bash $Q/drivers/run_all.sh $S/bin/kds_A $S/bin/kds_B /home/cdkbs/r100-work $Q/raw $S/progress
