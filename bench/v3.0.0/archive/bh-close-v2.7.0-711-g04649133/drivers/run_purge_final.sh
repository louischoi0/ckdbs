#!/bin/bash
# The full purge-throughput matrix again, as one consistent pass on a quiet host (the first pass ran at loadavg 1.4-1.6).
B=/home/cdkbs/bench-runs/bh-close
rm -f $B/purge-final.done
D="python3 $B/driver/bh_purge_tp.py --bin $B/bin/kds_server-B --port 31721 --workdir $B/w-tp --out $B/purge-tp-final"
CELLS=1:200,1:1000,1:10000,200:30000,1000:30000,10000:30000
taskset -c 4 $D --max-load 1.0 --cores 1 --pin-server 2 --durability relaxed --runs 5 --cells $CELLS
taskset -c 4 $D --max-load 1.6 --cores 2 --pin-server 2,3 --durability relaxed --runs 5 --cells $CELLS
for dur in strict group; do
  taskset -c 4 $D --max-load 1.0 --cores 1 --pin-server 2 --durability $dur --runs 3 --cells 1:1000,1000:10000
  taskset -c 4 $D --max-load 1.6 --cores 2 --pin-server 2,3 --durability $dur --runs 3 --cells 1:1000,1000:10000
done
echo finished > $B/purge-final.done
