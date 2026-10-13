#!/bin/sh
# After run2.sh: the layout probe on the every-row UPDATE (A, A', B, A-al64; 5 runs), only if it can finish by 05:50 UTC.
WT=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
B=/home/cdkbs/bench-runs/bm-s6/bin
R=/home/cdkbs/bench-runs/bm-s6
DEADLINE=$(date -u -d '2026-10-13 05:50:00' +%s)
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
mkdir -p $R/bnpad2
now=$(date +%s)
if [ $((now + 215)) -gt $DEADLINE ]; then echo "=== bnpad2 NOT STARTED $(date -u +%H:%M:%S): cannot finish by 05:50"; touch $R/flags/bnpad2.skipped; exit 0; fi
echo "=== bnpad2 start $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)"
python3 $WT/tools/bn_census.py --arm A=$B/kds_server-A --arm A2=$B/kds_server-A2 --arm B=$B/kds_server-B \
    --arm Apad=$B/kds_server-Apad --workdir $R/bnpad2 --port 32851 --runs 5 --pin-server 2 --pin-driver 4 \
    --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 200 --range-ops 240 --blocks 12 \
    --only update-range --quiet-load 2 --quiet-wait 900 $GATE \
    --out $R/bnpad2/bn.json > $R/bnpad2/bn.log 2>&1
rc=$?
echo "=== bnpad2 end $(date -u +%H:%M:%S) rc=$rc"
if [ $rc -eq 0 ]; then touch $R/flags/bnpad2.done; else touch $R/flags/bnpad2.failed; fi
