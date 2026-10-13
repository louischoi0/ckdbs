#!/bin/bash
# Copy the run's raw output into the worktree's bench archive (gzipped json and logs, scripts, hashes, host record).
R=/home/cdkbs/bench-runs/bt-close
D=/home/cdkbs/ckdbs/.claude/worktrees/bt-accept-tuple-row/bench/v3.0.0/archive/bt-close-v2.7.0-997-g4274ed81
mkdir -p $D/scripts
for p in p1a p1b p2 p3 p4 p5 p6 p7 p8 p9; do
  gzip -9 -c $R/out/$p.json > $D/$p.json.gz
  gzip -9 -c $R/out/$p.log > $D/$p.log.gz
done
cp $R/out/binaries.txt $R/out/host.txt $R/out/hostwatch.log $D/
cp $R/progress $D/progress.log
cp $R/watch.log $D/watch.log
cp $R/build.out $D/build.out
cp $R/cmake.log $D/cmake.log
gzip -9 -c $R/build-B.log > $D/build.log.gz
cp $R/scripts/build.sh $R/scripts/run.sh $R/scripts/run2.sh $R/scripts/watch.sh $R/scripts/watch2.sh $R/scripts/poll.sh \
   $R/scripts/hostinfo.sh $R/scripts/bt_close_scan.py $R/scripts/bt_close_bn.py $R/scripts/ident.py $R/scripts/hostsummary.py \
   $R/scripts/assemble.py $R/scripts/bt_close.tmpl $R/scripts/unclean.py $R/scripts/count_bad.py $R/scripts/archive.sh $R/scripts/p1a.txt $D/scripts/
ls -la $D | head -40
du -sh $D
