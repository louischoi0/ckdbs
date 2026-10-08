#!/bin/bash
# BH close part 1: interleaved A/B, cores=1 then cores=2. Driver pinned to CPU 4.
B=/home/cdkbs/bench-runs/bh-close
rm -f $B/overhead.done
taskset -c 4 python3 $B/driver/bh_overhead_ab.py --rows 200,1000,10000 --runs 16 --cores 1 --pin-server 2 \
  --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31711 --port-b 31712 \
  --workdir $B/w-c1 --out $B/overhead-c1 > $B/overhead-c1.log 2>&1
taskset -c 4 python3 $B/driver/bh_overhead_ab.py --rows 200,1000,10000 --runs 16 --cores 2 --pin-server 2,3 \
  --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31711 --port-b 31712 \
  --workdir $B/w-c2 --out $B/overhead-c2 > $B/overhead-c2.log 2>&1
echo finished > $B/overhead.done
