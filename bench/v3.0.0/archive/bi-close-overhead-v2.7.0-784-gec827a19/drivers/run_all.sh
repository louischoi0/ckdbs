#!/bin/bash
# BI close, OLTP overhead A/B (workorder-bi-checkpoint-off-the-reactor.md section 5.2 first bullet, BI-Q10 (iii)).
# A = bin/kds_server-A (55ee8370, BI-S1 instrumentation only), B = bin/kds_server-B (ec827a19, BI closed in code).
# Server pinned to CPU 4 (cores = 1) or 4,5 (cores = 2), driver on CPU 7, as BA's close.
# Cells run one after another, never overlapped. Each cell writes <name>.done when it ends.
R=/home/cdkbs/bench-runs/bi-close-overhead-v2.7.0-784-gec827a19
cd $R
rm -f $R/all.done
python3 -I $R/driver/sampler.py $R/host-samples.jsonl &
SAMPLER=$!
echo "started $(date -u '+%F %T') UTC" > $R/cells.log

D="python3 -I $R/driver/bi_overhead_ab.py --bin-a $R/bin/kds_server-A --bin-b $R/bin/kds_server-B --port-a 31811 --port-b 31812 --max-load 2 --max-wait 45"

cell() {  # name cores runs extra...
  local name=$1 c=$2 runs=$3; shift 3
  local pin=4
  if [ $c = 2 ]; then pin=4,5; fi
  echo "cell $name start $(date -u '+%T') load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/cells.log
  taskset -c 7 $D --runs $runs --cores $c --pin-server $pin --workdir $R/w-$name --out $R/out/$name "$@" > $R/out-$name.log 2>&1
  local rc=$?
  echo "cell $name end $(date -u '+%T') rc=$rc load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/cells.log
}

mkdir -p $R/out
cell relaxed-c1 1 16 --rows 200,1000,10000
cell relaxed-c2 2 16 --rows 200,1000,10000
cell group-c1 1 12 --rows 200,1000,10000 --durability group --arms insert,insert-again,iso
cell group-c2 2 12 --rows 200,1000,10000 --durability group --arms insert,insert-again,iso
cell dense-c1 1 10 --rows 1000,10000 --ckpt-ms 1000
cell dense-c2 2 10 --rows 1000,10000 --ckpt-ms 1000

# A/A control: the same binary (A) on both ports
D="python3 -I $R/driver/bi_overhead_ab.py --bin-a $R/bin/kds_server-A --bin-b $R/bin/kds_server-A --port-a 31811 --port-b 31812 --max-load 2 --max-wait 45"
cell aa-c1 1 8 --rows 200,1000,10000
cell aa-c2 2 8 --rows 200,1000,10000

kill $SAMPLER
echo "all done $(date -u '+%F %T') UTC" >> $R/cells.log
touch $R/all.done
