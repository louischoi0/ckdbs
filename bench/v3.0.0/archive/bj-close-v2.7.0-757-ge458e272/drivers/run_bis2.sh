#!/bin/bash
# B (8e37baf4) vs B' (e458e272): what the remedy recovered. cores = 1, 1,000 rows, 12 runs, Part 1 only.
B=/home/cdkbs/bench-runs/bj-close
O=$B/b2
rm -f $O/bis.done
taskset -c 4 python3 $B/driver/bj_stage_ab.py --p1-only --rows 1000 --runs 12 --cores 1 --pin-server 2 \
  --bin-a $B/bin/kds_server-B --bin-b $B/bin/kds_server-B2 --port-a 31911 --port-b 31912 --tagpre "BvB2-" \
  --workdir $O/w-bis --out $O/out-bis --max-other-busy 0.06 --max-wait 600 >> $O/run-bis.log 2>&1
echo done > $O/bis.done
