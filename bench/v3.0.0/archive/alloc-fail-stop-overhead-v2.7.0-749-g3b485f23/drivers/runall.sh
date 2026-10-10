#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/af
cd /home/cdkbs/bench-runs/af
date -u '+%F %T' > $S/start-main.txt
python3 -u $S/af_run.py 8 1000 /home/cdkbs/bench-runs/af/res > $S/run-main.log 2>&1
date -u '+%F %T' > $S/end-main.txt
date -u '+%F %T' > $S/start-swap.txt
AT_SWAP=1 python3 -u $S/af_run.py 5 1000 /home/cdkbs/bench-runs/af/res-swap > $S/run-swap.log 2>&1
date -u '+%F %T' > $S/end-swap.txt
echo done > $S/runall.done
