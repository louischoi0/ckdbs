#!/bin/bash
# BM-S6 remedy re-measure: prep (copy + hash binaries), then bn_census, then bm_scan bn cell.
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bm-scan-kernel-open/382ae513-4d71-443e-8517-51d482efafba/scratchpad/s6r
W=/home/cdkbs/ckdbs/.claude/worktrees/bm-scan-kernel-open
R=/home/cdkbs/bench-runs/bm-s6r
SRC=/home/cdkbs/bench-runs/bm-s6/bin
DESC=v2.7.0-948-g952c67e5
GATE="--cell-quiet-pct 10 --cell-quiet-wait 60 --cell-clean-pct 20 --cell-foreign-pct 15 --cell-retries 1"
mkdir -p $R/bin $R/bn1000 $R/bn10000 $R/out
cd $W
echo "prep $(date -u +%T)" > $S/progress
# wait for the build
while [ ! -f $S/build.done ]; do sleep 2; done
cat $S/build.done >> $S/progress
grep -q "EXIT 0" $S/build.done || { echo "BUILD FAILED" >> $S/progress; exit 1; }
cp $SRC/kds_server-A $R/bin/kds_server-A
cp $SRC/kds_server-A $R/bin/kds_server-A2
cp $SRC/kds_server-B $R/bin/kds_server-B
cp $S/build/kds_server $R/bin/kds_server-B2
stat -c '%n mtime=%y size=%s' $SRC/kds_server-A $SRC/kds_server-B $S/build/kds_server > $R/out/binaries.txt
(cd $R/bin && sha256sum kds_server-A kds_server-A2 kds_server-B kds_server-B2) >> $R/out/binaries.txt
cp $R/out/binaries.txt $S/binaries.txt
echo "prep done $(date -u +%T)" >> $S/progress
# host watch: loadavg every 10 s, to a file
( while [ ! -f $S/stop_watch ]; do echo "$(date -u +%T) $(cat /proc/loadavg)" >> $R/out/hostwatch.log; sleep 10; done ) &
echo "bn_census start $(date -u +%T)" >> $S/progress
python3 tools/bn_census.py --arm A=$R/bin/kds_server-A --arm A2=$R/bin/kds_server-A2 --arm B=$R/bin/kds_server-B --arm B2=$R/bin/kds_server-B2 \
  --workdir $R/bn1000 --port 32601 --runs 5 --pin-server 2 --pin-driver 4 --rows 1000 --scan-rows 1000 --ops 3000 --scan-ops 800 --range-ops 240 --blocks 12 \
  --only select-pk,update-range --quiet-load 2 --quiet-wait 240 $GATE --out $R/out/bn1000.json > $R/out/bn1000.log 2>&1
echo "bn_census done rc=$? $(date -u +%T)" >> $S/progress
touch $S/bn_census.done
echo "bm_scan bn start $(date -u +%T)" >> $S/progress
python3 tools/bm_scan.py census --arm A=$R/bin/kds_server-A --arm B2=$R/bin/kds_server-B2 \
  --workdir $R/bn10000 --port 32611 --runs 5 --pin-server 2 --pin-driver 4 --cells bn --scan-ops 192 --point-ops 3000 \
  --quiet-load 2 --quiet-wait 240 $GATE --out $R/out/bmscan-bn.json > $R/out/bmscan-bn.log 2>&1
echo "bm_scan bn done rc=$? $(date -u +%T)" >> $S/progress
touch $S/all.done
touch $S/stop_watch
