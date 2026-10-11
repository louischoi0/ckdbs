#!/bin/bash
# re-run of the c2 runs discarded for other-CPU busy > 30 % (they were moved to out-discarded/)
B=/home/cdkbs/bench-runs/bj-close
for r in 200 1000; do
  taskset -c 4 python3 $B/driver/bj_close_ab.py --rows $r --runs 16 --cores 2 --pin-server 2,3 \
    --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31911 --port-b 31912 \
    --workdir $B/w-c2 --out $B/out-c2 --max-other-busy 0.15 --max-wait 300 >> $B/run-c2-rerun.log 2>&1
done
echo done > $B/rerun.done
