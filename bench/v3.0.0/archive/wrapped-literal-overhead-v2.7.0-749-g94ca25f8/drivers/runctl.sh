#!/bin/bash
cd /home/cdkbs/bench-runs/lw
R=/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/lw
sha256sum kds_server-C >> binaries.sha256
date -u '+%F %T' > start-ctl.txt
LW_BINB=C python3 $R/lw_run.py 8 10000,1000 /home/cdkbs/bench-runs/lw/res-ctl > run-ctl.log 2>&1
LW_BINB=C LW_SWAP=1 python3 $R/lw_run.py 8 10000 /home/cdkbs/bench-runs/lw/res-ctl-swap > run-ctl-swap.log 2>&1
date -u '+%F %T' > end-ctl.txt
python3 $R/../nh/analyze.py /home/cdkbs/bench-runs/lw/res-ctl
python3 $R/../nh/analyze.py /home/cdkbs/bench-runs/lw/res-ctl-swap
