#!/bin/bash
# range100 bisection: A against each BH stage binary (and B again), cores=1 and cores=2, rows 1000 and 10000.
B=/home/cdkbs/bench-runs/bh-close
rm -f $B/bis.done
for s in S1 S2 S3 B; do
  taskset -c 4 python3 $B/driver/bh_overhead_ab.py --rows 1000,10000 --runs 10 --cores 1 --pin-server 2 \
    --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-$s --port-a 31711 --port-b 31712 \
    --workdir $B/w-bis --out $B/bis-c1-$s --max-load 1.6 > $B/bis-c1-$s.log 2>&1
done
for s in S1 B; do
  taskset -c 4 python3 $B/driver/bh_overhead_ab.py --rows 10000 --runs 10 --cores 2 --pin-server 2,3 \
    --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-$s --port-a 31711 --port-b 31712 \
    --workdir $B/w-bis --out $B/bis-c2-$s --max-load 1.6 > $B/bis-c2-$s.log 2>&1
done
echo finished > $B/bis.done
