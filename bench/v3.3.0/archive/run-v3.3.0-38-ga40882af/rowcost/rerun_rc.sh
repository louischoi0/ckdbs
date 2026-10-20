#!/bin/bash
# Re-run the three contaminated row-cost cells (the sweeps are resumable by (rep, size, mode)).
RC=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost
echo $$ > $RC/master-rerun.pid
bash $RC/run_rowcost.sh
bash $RC/run_send.sh
touch $RC/rerun.done
