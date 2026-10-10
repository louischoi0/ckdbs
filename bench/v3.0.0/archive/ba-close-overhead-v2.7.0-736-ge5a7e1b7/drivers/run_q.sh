#!/bin/bash
# Quiet-window repeat of the relaxed arms, with two more controls (SHOW NAMESPACES, SET ISOLATION LEVEL).
B=/home/cdkbs/bench-runs/ba-close
rm -f $B/q.done
D="python3 -I $B/driver/ba_overhead_ctrl.py --rows 200,1000,10000 --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31811 --port-b 31812 --max-load 2 --max-wait 120"
for c in 1 2; do
  if [ $c = 1 ]; then PIN=4; else PIN=4,5; fi
  taskset -c 7 $D --runs 16 --cores $c --pin-server $PIN --workdir $B/wq-c$c --out $B/overheadq-c$c > $B/overheadq-c$c.log 2>&1
done
echo finished > $B/q.done
