#!/bin/bash
B=/home/cdkbs/bench-runs/bh-close2
rm -f $B/all.done
for c in 1 2; do
  if [ $c = 1 ]; then PIN=2; else PIN=2,3; fi
  taskset -c 4 python3 $B/driver/bh_overhead_ab.py --rows 200,1000,10000 --runs 16 --cores $c --pin-server $PIN \
    --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B2 --port-a 31811 --port-b 31812 \
    --workdir $B/w-c$c --out $B/overhead-c$c --max-load 1.6 > $B/overhead-c$c.log 2>&1
done
echo overhead-done > $B/all.done
D="python3 $B/driver/bh_purge_tp.py --bin $B/bin/kds_server-B2 --port 31821 --workdir $B/w-tp --out $B/purge-tp --max-load 1.6"
CELLS=1:200,1:1000,1:10000,200:30000,1000:30000,10000:30000
taskset -c 4 $D --cores 1 --pin-server 2 --durability relaxed --runs 5 --cells $CELLS > $B/purge-c1.log 2>&1
taskset -c 4 $D --cores 2 --pin-server 2,3 --durability relaxed --runs 5 --cells $CELLS > $B/purge-c2.log 2>&1
taskset -c 4 $D --cores 1 --pin-server 2 --durability strict --runs 3 --cells 1:1000,1000:10000 > $B/purge-strict.log 2>&1
echo finished >> $B/all.done
