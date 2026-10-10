#!/bin/bash
# BA close, Job 1: interleaved A/B overhead. Server pinned to 4 (cores=1) or 4,5 (cores=2), driver on 7.
B=/home/cdkbs/bench-runs/ba-close
rm -f $B/all.done
D="python3 -I $B/driver/ba_overhead_ab.py --rows 200,1000,10000 --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31811 --port-b 31812 --max-load 8 --max-wait 60"
for c in 1 2; do
  if [ $c = 1 ]; then PIN=4; else PIN=4,5; fi
  ps -eo pid,psr,pcpu,etime,args --sort=-pcpu | head -8 > $B/ps-before-c$c.txt
  taskset -c 7 $D --runs 16 --cores $c --pin-server $PIN --workdir $B/w-c$c --out $B/overhead-c$c > $B/overhead-c$c.log 2>&1
done
echo relaxed-done > $B/all.done
for c in 1 2; do
  if [ $c = 1 ]; then PIN=4; else PIN=4,5; fi
  for dur in group strict; do
    taskset -c 7 $D --runs 16 --cores $c --pin-server $PIN --durability $dur --arms insert,insert-again,ping --workdir $B/w-$dur-c$c --out $B/overhead-$dur-c$c > $B/overhead-$dur-c$c.log 2>&1
  done
done
echo finished >> $B/all.done
