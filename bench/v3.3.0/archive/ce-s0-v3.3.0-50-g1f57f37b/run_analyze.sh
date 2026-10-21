#!/bin/bash
# One fresh server, the row-cost table loaded, ANALYZE of each sweep statement. Not a measurement of time.
source /home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b/lib.sh
export PYTHONPATH=$TOOLS
PORT=15763
start_server plans $PORT 1 group 2 || exit 1
python3 $RUN/rowcost/ce_s0_rows.py load --kds-port $PORT > $RUN/logs/plans.load 2>&1
python3 $RUN/analyze_plans.py $PORT $RUN/logs/analyze-plans.txt
stop_server plans
touch $RUN/done/plans
