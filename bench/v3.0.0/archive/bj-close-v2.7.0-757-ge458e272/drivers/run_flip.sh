#!/bin/bash
# Flipped pair: slot A = B' (e458e272), slot B = B (8e37baf4), cores 1, 1,000 rows, 16 runs, full driver.
# Part 2 and 3 then run on B (8e37baf4) in the same hour as the B' runs of b2/out-c1, and Part 1 is a second B vs B' pair.
B=/home/cdkbs/bench-runs/bj-close
O=$B/b2
rm -f $O/flip.done
taskset -c 4 python3 $B/driver/bj_close_ab.py --rows 1000 --runs 16 --cores 1 --pin-server 2 \
  --bin-a $B/bin/kds_server-B2 --bin-b $B/bin/kds_server-B --port-a 31911 --port-b 31912 \
  --workdir $O/w-flip --out $O/out-flip --max-other-busy 0.06 --max-wait 600 >> $O/run-flip.log 2>&1
echo done > $O/flip.done
