#!/bin/bash
# Extra cells, queued behind run_all.sh (waits for all.done): the dense cadence (checkpoint_interval_ms = 1000) at 200 rows,
# and an A/A control at the dense cadence, so the dense cell's longest-statement reading has a same-binary floor.
R=/home/cdkbs/bench-runs/bi-close-overhead-v2.7.0-784-gec827a19
cd $R
while [ ! -f $R/all.done ]; do sleep 5; done
rm -f $R/extra.done
python3 -I $R/driver/sampler.py $R/host-samples-extra.jsonl &
SAMPLER=$!

cell() {  # name cores runs extra...
  local name=$1 c=$2 runs=$3; shift 3
  local pin=4
  if [ $c = 2 ]; then pin=4,5; fi
  echo "cell $name start $(date -u '+%T') load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/cells.log
  taskset -c 7 $D --runs $runs --cores $c --pin-server $pin --workdir $R/w-$name --out $R/out/$name "$@" > $R/out-$name.log 2>&1
  local rc=$?
  echo "cell $name end $(date -u '+%T') rc=$rc load $(cut -d' ' -f1-3 /proc/loadavg)" >> $R/cells.log
}

D="python3 -I $R/driver/bi_overhead_ab.py --bin-a $R/bin/kds_server-A --bin-b $R/bin/kds_server-B --port-a 31811 --port-b 31812 --max-load 2 --max-wait 45"
cell dense200-c1 1 10 --rows 200 --ckpt-ms 1000
cell dense200-c2 2 10 --rows 200 --ckpt-ms 1000
D="python3 -I $R/driver/bi_overhead_ab.py --bin-a $R/bin/kds_server-A --bin-b $R/bin/kds_server-A --port-a 31811 --port-b 31812 --max-load 2 --max-wait 45"
cell aadense-c1 1 10 --rows 1000,10000 --ckpt-ms 1000

kill $SAMPLER
touch $R/extra.done
echo "extra done $(date -u '+%F %T') UTC" >> $R/cells.log
