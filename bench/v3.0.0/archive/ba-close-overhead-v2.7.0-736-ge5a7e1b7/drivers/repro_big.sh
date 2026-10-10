#!/bin/bash
B=/home/cdkbs/bench-runs/ba-close
for i in $(seq 1 120); do
  l=$(cut -d' ' -f1 /proc/loadavg); awk "BEGIN{exit !($l<2.0)}" && break; sleep 10
done
echo "start load $(cut -d' ' -f1 /proc/loadavg) $(date -u +%H:%M:%S)" > $B/repro.log
taskset -c 7 python3 -I $B/driver/ba_overhead_ctrl.py --rows 50000,100000 --runs 2 --ops 600 --blocks 12 --cores 2 --pin-server 4,5 --arms range100,select-pk,iso --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31811 --port-b 31812 --workdir $B/wrepro --out $B/repro-c2 --max-load 2 --max-wait 120 >> $B/repro.log 2>&1
echo "exit $? $(date -u +%H:%M:%S)" >> $B/repro.log
touch $B/repro.done
