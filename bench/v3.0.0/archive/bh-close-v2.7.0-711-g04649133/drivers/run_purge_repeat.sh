#!/bin/bash
# A second, later pass of the one-key purge cell (host-regime check). B only.
B=/home/cdkbs/bench-runs/bh-close
rm -f $B/purge-repeat.done
D="python3 $B/driver/bh_purge_tp.py --bin $B/bin/kds_server-B --port 31721 --workdir $B/w-tp --out $B/purge-tp-repeat --max-load 1.6"
taskset -c 4 $D --cores 1 --pin-server 2 --durability relaxed --runs 10 --cells 1:1000,1000:30000
taskset -c 4 $D --cores 2 --pin-server 2,3 --durability relaxed --runs 10 --cells 1:1000,1000:30000
echo finished > $B/purge-repeat.done
