#!/bin/bash
# Profiles the every-row UPDATE under the LD_PRELOAD sampler on both sides of
# the BQ merge (ccf9cfc1, 6139c2de), the profile pass only, one after the
# other on a quiet host; reports the top functions of each.
set -u
C=/home/cdkbs/bench-runs/close
WT=/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue
OUT=$C/out/profile
mkdir -p $OUT
SO=/home/cdkbs/bench-runs/bp-s1/bin/libkdssampler.so
for side in "pre /home/cdkbs/bench-runs/bp-s1/v2/bin/kds_server.b" "post $C/bin/kds_server.6139c2de"; do
  set -- $side
  [ -f $OUT/$1.done ] && continue
  while ! awk '{ exit !($1 <= 1.6) }' /proc/loadavg; do sleep 10; done
  rm -rf $C/work/prof $OUT/$1
  taskset -c 4 python3 $WT/tools/bn_census.py --bin $2 --workdir $C/work/prof --port 32161 \
    --pin-server 2 --pin-driver 4 --rows 10000 --ops 3000 --range-ops 60 --runs 1 \
    --sampler $SO --sampler-dir $OUT/$1 --profile-ops 3000 --profile-range-ops 120 \
    --profile-only update-range --only update-range --out $OUT/$1.json > $OUT/$1.log 2>&1
  echo "rc=$?" >> $OUT/$1.log
  touch $OUT/$1.done
done
for s in pre post; do
  for d in $OUT/$s/dump*; do
    echo "=== $s $d"
    python3 $WT/tools/kds_sampler_report.py "$d" --top 25 2>&1 | head -40
  done
done > $OUT/report.txt
touch $OUT/all.done
