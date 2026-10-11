#!/bin/bash
# Re-measure of the literal path, A (e5a7e1b7) vs B' (e458e272). Resumable.
B=/home/cdkbs/bench-runs/bj-close
O=$B/b2
mkdir -p $O
rm -f $O/all.done
cell() {  # cores rows pin
  taskset -c 4 python3 $B/driver/bj_close_ab.py --rows $2 --runs 16 --cores $1 --pin-server $3 \
    --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B2 --port-a 31911 --port-b 31912 \
    --workdir $O/w-c$1 --out $O/out-c$1 --max-other-busy 0.15 --max-wait 300 >> $O/run-c$1-n$2.log 2>&1
}
cell 1 1000 2
cell 2 1000 2,3
cell 1 10000 2
echo done > $O/all.done
