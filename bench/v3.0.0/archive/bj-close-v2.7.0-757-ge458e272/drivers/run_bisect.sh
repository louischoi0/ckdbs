#!/bin/bash
# Literal-path stage bisection: cores = 1, 1,000 rows, 12 runs, Part 1 only. Each pair is A-side vs B-side binary.
B=/home/cdkbs/bench-runs/bj-close
rm -f $B/bisect.done
run() {  # name, bin-a, bin-b
  taskset -c 4 python3 $B/driver/bj_stage_ab.py --p1-only --rows 1000 --runs 12 --cores 1 --pin-server 2 \
    --bin-a $B/bin/$2 --bin-b $B/bin/$3 --port-a 31911 --port-b 31912 --tagpre "$1-" \
    --workdir $B/w-bis --out $B/out-bis --max-other-busy 0.15 --max-wait 300 >> $B/run-bis.log 2>&1
}
run s0-s1 kds_server-s0 kds_server-s1
run s0-s2 kds_server-s0 kds_server-s2
run s0-s3 kds_server-s0 kds_server-s3
run A-s4 kds_server-A kds_server-s4
run A-s5 kds_server-A kds_server-s5
run A-s6 kds_server-A kds_server-s6
run s0-A kds_server-s0 kds_server-A
echo done > $B/bisect.done
