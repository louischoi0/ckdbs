#!/bin/bash
cd /home/cdkbs/bench-runs/bw-update-prof
W=/home/cdkbs/ckdbs/.claude/worktrees/bw-instant-add-drop-column
for r in 1 2 3; do for n in A B; do
 echo "$(date -u +%T) run $n$r" >> progress.txt
 rm -rf wd-$n$r; mkdir -p wd-$n$r out
 python3 $W/tools/bn_census.py --bin build-$n/kds_server --workdir wd-$n$r --port 3193$r --runs 0 \
   --sampler $PWD/sampler.so --sampler-dir $PWD/out/$n$r --profile-only update-range --profile-range-ops 8000 \
   --out out/$n$r.json > out/$n$r.log 2>&1
 echo "$(date -u +%T) done $n$r rc=$?" >> progress.txt
 rm -rf wd-$n$r
done; done
echo "$(date -u +%T) all done" >> progress.txt
