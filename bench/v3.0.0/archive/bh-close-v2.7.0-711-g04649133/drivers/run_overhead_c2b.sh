#!/bin/bash
# Resume of cores=2 after run_overhead.sh was stopped: its --max-load 1.0 gate waited 300 s per run at
# cores=2, where the driver's own load average sits at ~1.3. n200 (16 runs) are kept; 1000 and 10000 are redone
# with --max-load 1.6.
B=/home/cdkbs/bench-runs/bh-close
rm -rf $B/w-c2 $B/overhead.done
taskset -c 4 python3 $B/driver/bh_overhead_ab.py --rows 1000,10000 --runs 16 --cores 2 --pin-server 2,3 \
  --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31711 --port-b 31712 \
  --workdir $B/w-c2 --out $B/overhead-c2 --max-load 1.6 > $B/overhead-c2b.log 2>&1
echo finished > $B/overhead.done
