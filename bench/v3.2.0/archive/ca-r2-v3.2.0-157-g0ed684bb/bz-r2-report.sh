#!/bin/bash
R=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer
cd $R/bench/v3.2.0/archive/ca-r2-v3.2.0-157-g0ed684bb/prof
for f in prof-c13-s3reads-r1/s3 prof-c13-s3-r1/s3 prof-c13-s4-r1/s4-off prof-c13-s4-r1/s4-on; do
  python3 $R/tools/kds_sampler_report.py $f.dump.0 --top 30 --parts $R/bench/v3.2.0/archive/ca-harness/parts.json > $f.report.txt
done
