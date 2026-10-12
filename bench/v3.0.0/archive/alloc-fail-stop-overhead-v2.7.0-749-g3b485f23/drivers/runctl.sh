#!/bin/bash
S=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/af
cd /home/cdkbs/bench-runs/af
date -u '+%F %T' > $S/start-aa.txt
AT_SWAP=AA python3 -u $S/af_run.py 5 1000 /home/cdkbs/bench-runs/af/res-aa > $S/run-aa.log 2>&1
date -u '+%F %T' > $S/end-aa.txt
date -u '+%F %T' > $S/start-bb.txt
AT_SWAP=BB python3 -u $S/af_run.py 5 1000 /home/cdkbs/bench-runs/af/res-bb > $S/run-bb.log 2>&1
date -u '+%F %T' > $S/end-bb.txt
echo done > $S/runctl.done
