#!/bin/bash
# Move runs the discard rules flag into b2/discarded/passN and re-run them, up to 5 passes.
B=/home/cdkbs/bench-runs/bj-close
O=$B/b2
rm -f $O/redo.done
for pass in 1 2 3 4 5; do
  mkdir -p $O/discarded/pass$pass
  n=0
  for c in 1 2; do
    for t in $(python3 $B/driver/screen.py --tags $O/out-c$c); do
      mv $O/out-c$c/$t.json $O/out-c$c/$t.raw.json.gz $O/discarded/pass$pass/ && n=$((n+1))
    done
  done
  echo "pass $pass moved $n" >> $O/redo.log
  [ $n = 0 ] && break
  for spec in "1 1000 2" "2 1000 2,3" "1 10000 2"; do
    set -- $spec
    taskset -c 4 python3 $B/driver/bj_close_ab.py --rows $2 --runs 16 --cores $1 --pin-server $3 \
      --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B2 --port-a 31911 --port-b 31912 \
      --workdir $O/w-c$1 --out $O/out-c$1 --max-other-busy 0.06 --max-wait 600 >> $O/run-c$1-n$2-redo.log 2>&1
  done
done
echo done > $O/redo.done
