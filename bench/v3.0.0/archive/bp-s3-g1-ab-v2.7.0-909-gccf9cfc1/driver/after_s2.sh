#!/bin/bash
# BP-S3's two A/B cells the stale WAL directory refused (bn_census fixed to
# remove it), run once BQ-S2's matrix has released the host. Gated like the
# chain; writes out/ab/bn-r<n>.json and touches out/after_s2.done.
set -u
R=/home/cdkbs/bench-runs/bp-s1/v2
OUT=$R/out
WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
until grep -q 'chain DONE' /home/cdkbs/bench-runs/bq/s2/out/watchdog.log 2>/dev/null; do
  date +%s > $OUT/after_s2.beat; sleep 30
done
for r in 2 3; do
  f=$OUT/ab/bn-r$r.json
  [ -s $f ] && continue
  while pgrep -af 'cc1plus|cmake --build|ctest|kds_tests|tools/bl_mixed.py --bin' | grep -v -e pgrep -e shell-snapshots > /dev/null \
        || ! awk '{ exit !($1 <= 1.6) }' /proc/loadavg; do
    date +%s > $OUT/after_s2.beat; sleep 10
  done
  echo "$(date -u '+%F %T') ab bn r$r (after s2) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $OUT/hoststamps.log
  rm -rf $R/work/bn
  taskset -c 4 python3 $WT/tools/bn_census.py --bin $R/bin/kds_server.a --bin-b $R/bin/kds_server.b \
    --workdir $R/work/bn --port 32011 --pin-server 2 --pin-driver 4 --rows 10000 --ops 3000 --range-ops 60 \
    --blocks 12 --runs 1 --only select-pk,update-lit,update-range --out $f.tmp > $OUT/ab/bn-r$r.log 2>&1
  rc=$?; [ $rc = 0 ] && mv $f.tmp $f
  echo "$(date -u '+%F %T') after ab bn r$r rc=$rc" >> $OUT/hoststamps.log
done
touch $OUT/after_s2.done
