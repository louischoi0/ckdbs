#!/bin/bash
# re-run of the runs moved to out-discarded/ because a cmake --build or ctest of another session was in the host list
B=/home/cdkbs/bench-runs/bj-close
rm -f $B/rerun2.done
taskset -c 4 python3 $B/driver/bj_close_ab.py --rows 1000 --runs 16 --cores 1 --pin-server 2 \
  --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31911 --port-b 31912 \
  --workdir $B/w-c1 --out $B/out-c1 --max-other-busy 0.15 --max-wait 300 >> $B/run-c1-rerun2.log 2>&1
taskset -c 4 python3 $B/driver/bj_close_ab.py --rows 1000 --runs 16 --cores 2 --pin-server 2,3 \
  --bin-a $B/bin/kds_server-A --bin-b $B/bin/kds_server-B --port-a 31911 --port-b 31912 \
  --workdir $B/w-c2 --out $B/out-c2 --max-other-busy 0.15 --max-wait 300 >> $B/run-c2-rerun2.log 2>&1
echo done > $B/rerun2.done
