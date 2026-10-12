#!/bin/bash
# The second bisect of the every-row UPDATE move: ccf9cfc1 -> 6139c2de (the BQ
# merge) -> d555cb55 (BP-S4) -> 3e77e7d1 (BP-S6; code-identical at cores = 1
# to S6's c4358960). Three runs each, gated; resumable.
set -u
C=/home/cdkbs/bench-runs/close
WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
OUT=$C/out/rollfix
mkdir -p $OUT
P2=/home/cdkbs/bench-runs/bp-s1/v2/bin/kds_server.b
PM=$C/bin/kds_server.rollfix
PB=$C/bin/kds_server.3e77e7d1
P4=$C/bin/kds_server.d555cb55
P6=$C/bin/kds_server.3e77e7d1
for r in 1 2 3; do
  for pair in "PREMERGE_FIX $P2 $PM" "CLOSE_FIX $PB $PM"; do
    set -- $pair
    f=$OUT/$1-r$r.json
    [ -s $f ] && continue
    while pgrep -af 'cc1plus|cmake --build|ctest|kds_tests|tools/bl_mixed.py --bin' | grep -v -e pgrep -e shell-snapshots > /dev/null \
          || ! awk '{ exit !($1 <= 1.6) }' /proc/loadavg; do
      date +%s > $OUT/beat; sleep 10
    done
    echo "$(date -u '+%F %T') rollfix $1 r$r load=$(cut -d' ' -f1-3 /proc/loadavg)" >> $OUT/stamps.log
    rm -rf $C/work/rollfix
    taskset -c 4 python3 $WT/tools/bn_census.py --bin $2 --bin-b $3 --workdir $C/work/rollfix --port 32151 \
      --pin-server 2 --pin-driver 4 --rows 10000 --ops 3000 --range-ops 60 --blocks 12 --runs 1 \
      --only select-pk,update-range --out $f.tmp > $OUT/$1-r$r.log 2>&1
    rc=$?; [ $rc = 0 ] && mv $f.tmp $f
    echo "$(date -u '+%F %T') after rollfix $1 r$r rc=$rc" >> $OUT/stamps.log
  done
done
touch $OUT/done
