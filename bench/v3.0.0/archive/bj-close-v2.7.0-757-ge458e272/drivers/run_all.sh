#!/bin/bash
# BJ close: the whole matrix. Resumable: a (cores, rows, run) whose JSON exists is skipped.
B=/home/cdkbs/bench-runs/bj-close
rm -f $B/all.done
for c in 1 2; do
  if [ $c = 1 ]; then PIN=2; else PIN=2,3; fi
  taskset -c 4 python3 $B/driver/bj_close_ab.py --rows 200,1000,10000 --runs 16 --cores $c --pin-server $PIN \
    --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31911 --port-b 31912 \
    --workdir $B/w-c$c --out $B/out-c$c --max-other-busy 0.15 --max-wait 300 >> $B/run-c$c.log 2>&1
done
echo done > $B/all.done
