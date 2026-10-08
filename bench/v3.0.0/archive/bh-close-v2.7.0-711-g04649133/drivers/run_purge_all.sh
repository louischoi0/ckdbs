#!/bin/bash
# BH close part 2: purge throughput (B only), then DELETE->PURGE latency under readers (B only).
# Server on CPU 2 (cores=1) or 2,3 (cores=2); the tp driver on CPU 4; the latency driver on CPU 5, its
# background readers/writers on 4,6,7.
B=/home/cdkbs/bench-runs/bh-close
rm -f $B/purge-all.done
D="python3 $B/driver/bh_purge_tp.py --bin $B/bin/kds_server-B --port 31721 --workdir $B/w-tp --out $B/purge-tp --max-load 1.6"
CELLS=1:200,1:1000,1:10000,200:30000,1000:30000,10000:30000
taskset -c 4 $D --cores 1 --pin-server 2 --durability relaxed --runs 5 --cells $CELLS
taskset -c 4 $D --cores 2 --pin-server 2,3 --durability relaxed --runs 5 --cells $CELLS
for dur in strict group; do
  taskset -c 4 $D --cores 1 --pin-server 2 --durability $dur --runs 3 --cells 1:1000,1000:10000
  taskset -c 4 $D --cores 2 --pin-server 2,3 --durability $dur --runs 3 --cells 1:1000,1000:10000
done
echo tp-finished
L="python3 $B/driver/bh_purge_latency.py --bin $B/bin/kds_server-B --port 31741 --workdir $B/w-lat --out $B/purge-lat --max-load 1.6 --keys 60000 --samples 20000"
for rep in 0 1 2; do
  for cores in 1 2; do
    if [ $cores = 1 ]; then PIN=2; else PIN=2,3; fi
    $L --cores $cores --pin-server $PIN --readers 0 --writers 0 --label c${cores}-R0W0-r$rep
    $L --cores $cores --pin-server $PIN --readers 3 --writers 0 --label c${cores}-R3W0-r$rep
    $L --cores $cores --pin-server $PIN --readers 2 --writers 1 --label c${cores}-R2W1-r$rep
  done
done
echo lat-finished
S="python3 $B/driver/bh_purge_latency.py --mode snapshot --bin $B/bin/kds_server-B --port 31741 --workdir $B/w-lat --out $B/purge-lat --max-load 1.6 --keys 2000"
$S --cores 1 --pin-server 2 --label snap-c1
$S --cores 2 --pin-server 2,3 --label snap-c2
echo finished > $B/purge-all.done
