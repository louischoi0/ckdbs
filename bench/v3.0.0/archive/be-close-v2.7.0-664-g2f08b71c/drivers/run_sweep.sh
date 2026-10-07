#!/bin/bash
R=/home/cdkbs/bench-runs/be-close
L=$R/sweep.log
: > $L
for BUD in 16384 65536 131072; do
  for ARM in B A; do
    PAST=0
    if [ "$ARM" = "A" ] && [ "$BUD" = "131072" ]; then PAST=4000; fi
    for REP in 1 2; do
      if [ "$ARM" = "A" ] && [ "$REP" = "2" ] && [ "$BUD" != "16384" ]; then continue; fi
      echo "== $ARM budget=$BUD rep=$REP past=$PAST load $(cut -d' ' -f1-3 /proc/loadavg)" >> $L
      $R/bin/kds_pool_sweep_bench-$ARM --budget $BUD --file $R/sweep/$ARM.db --past $PAST >> $L 2>&1
      rm -f $R/sweep/$ARM.db
    done
  done
done
echo done >> $L
