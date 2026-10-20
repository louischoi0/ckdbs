#!/bin/bash
# Copies the run's scripts, driver summaries and logs (never data files or the binaries) into the bench archive.
R=/home/cdkbs/bench-runs/ce-close-v3.3.0-52-g9e568b44
A=/home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply/bench/v3.3.0/archive/ce-close-v3.3.0-52-g9e568b44
cd $R
python3 windows.py > windows.txt
python3 analyze_close.py > analysis.md
python3 analyze_census.py >> analysis.md
python3 analyze_drain.py > analysis-drain.md
mkdir -p $A/rowcost $A/json $A/logs $A/conf
cp lib.sh monitor.sh watchdog.sh watchdog2.sh cellvalid.py run_close.sh run_drain.sh build.sh waiter.sh archive.sh analyze_close.py analyze_census.py analyze_drain.py windows.py verify.py analysis.md analysis-drain.md windows.txt binary.sha256 source-binary-mtime-A.txt source-binary-mtime-B.txt start-time.txt timeline watchdog.log watchdog2.log build.t0 build.t1 build.rc $A/
cp rowcost/ce_s0_rows.py rowcost/rowcost.py rowcost/kdsrow.py $A/rowcost/
gzip -c monitor.log > $A/monitor.log.gz
for f in rowcost/cells-p*.jsonl rowcost/drain-p*.jsonl rowcost/drain-p3-B.jsonl.contaminated; do gzip -c $f > $A/rowcost/$(basename $f).gz; done
cp json/*.json $A/json/
cp logs/* $A/logs/
cp conf/*.conf $A/conf/
cp /home/cdkbs/ckdbs/.claude/worktrees/ce-streaming-reply/tools/ttfb_probe.py $A/
tail -3 build.log > $A/build-tail.log
du -sh $A
