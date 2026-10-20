#!/bin/bash
# Copies the run's scripts, driver summaries and logs (never data files, the binary or the tools snapshot) into the bench archive.
R=/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b
A=/home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply/bench/v3.3.0/archive/ce-s0-v3.3.0-50-g1f57f37b
mkdir -p $A/rowcost $A/logs $A/json $A/conf
for f in lib.sh monitor.sh cellvalid.py run_ce_s0.sh watchdog.sh start.sh wait.sh analyze.py stamp.py early.py sens.py \
         analyze_plans.py run_analyze.sh waiter.sh smoke.sh archive.sh binary.sha256 source-binary-mtime.txt start-time.txt timeline watchdog.log analysis.md; do
    cp $R/$f $A/
done
cp $R/rowcost/ce_s0_rows.py $R/rowcost/rowcost.py $R/rowcost/kdsrow.py $A/rowcost/
gzip -c $R/rowcost/cells.jsonl > $A/rowcost/cells.jsonl.gz
gzip -c $R/rowcost/cells.jsonl.contaminated > $A/rowcost/cells.contaminated.jsonl.gz
cp $R/json/*.json $A/json/
cp $R/logs/* $A/logs/
cp $R/conf/*.conf $A/conf/
gzip -c $R/monitor.log > $A/monitor.log.gz
cp /home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply/tools/ttfb_probe.py $A/ttfb_probe.py
sha256sum $R/kds_server
du -sh $A
ls $A
