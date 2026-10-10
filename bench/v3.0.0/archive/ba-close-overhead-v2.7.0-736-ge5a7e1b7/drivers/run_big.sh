#!/bin/bash
# Walks longer than one 64-page slice: 50,000 and 100,000 rows. Scan arm only, 600 statements per arm per server.
B=/home/cdkbs/bench-runs/ba-close
rm -f $B/big.done
taskset -c 7 python3 -I $B/driver/ba_overhead_ctrl.py --rows 50000,100000 --runs 12 --ops 600 --blocks 12 --cores 1 --pin-server 4 --arms range100,select-pk,iso --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31811 --port-b 31812 --workdir $B/wbig --out $B/overheadbig-c1 --max-load 2 --max-wait 120 > $B/overheadbig-c1.log 2>&1
taskset -c 7 python3 -I $B/driver/ba_overhead_ctrl.py --rows 50000,100000 --runs 12 --ops 600 --blocks 12 --cores 2 --pin-server 4,5 --arms range100,select-pk,iso --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31811 --port-b 31812 --workdir $B/wbig2 --out $B/overheadbig-c2 --max-load 2 --max-wait 120 > $B/overheadbig-c2.log 2>&1
echo finished > $B/big.done
