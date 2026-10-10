#!/bin/bash
# Quiet-window repeat of the cores = 1 synced arms (group, strict), single-row INSERT + replicate.
B=/home/cdkbs/bench-runs/ba-close
rm -f $B/sync.done
D="python3 -I $B/driver/ba_overhead_ctrl.py --rows 200,1000,10000 --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31811 --port-b 31812 --max-load 2 --max-wait 120"
for dur in group strict; do
  taskset -c 7 $D --runs 16 --cores 1 --pin-server 4 --durability $dur --arms insert,insert-again,iso --workdir $B/wqs-$dur --out $B/overheadq-$dur-c1 > $B/overheadq-$dur-c1.log 2>&1
done
echo finished > $B/sync.done
