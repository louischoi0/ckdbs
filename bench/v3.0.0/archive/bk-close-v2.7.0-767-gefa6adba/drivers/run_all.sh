#!/bin/bash
# BK close: the measurement matrix. Driver pinned to CPU 4; server on CPU 2 (cores = 1) or 2,3 (cores = 2).
# Sequential on purpose: nothing else of this session runs while it does.
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
R=$SP/run
BIN=$R/bin
D="taskset -c 4 python3 $SP/bk_overhead_ab.py --frames 65536 --port-a 32011 --port-b 32012 --pin-driver 4 --max-other-busy 0.06 --max-wait 60"
rm -f $R/all.done
stamp() { date -u +"%F %T UTC $1" >> $R/stages.log; }

stamp "c1 pass 1 start"
$D --rows 200,1000,10000 --runs 16 --cores 1 --pin-server 2 --bin-a $BIN/kds_server-A --bin-b $BIN/kds_server-B \
   --workdir $R/w-c1 --out $R/overhead-c1 > $R/overhead-c1.log 2>&1
stamp "c1 pass 1 end; null pair start"
$D --rows 200,1000,10000 --runs 16 --cores 1 --pin-server 2 --bin-a $BIN/kds_server-A --bin-b $BIN/kds_server-A2 \
   --workdir $R/w-null --out $R/null-c1 > $R/null-c1.log 2>&1
stamp "null pair end; c2 start"
$D --rows 200,1000,10000 --runs 16 --cores 2 --pin-server 2,3 --bin-a $BIN/kds_server-A --bin-b $BIN/kds_server-B \
   --workdir $R/w-c2 --out $R/overhead-c2 > $R/overhead-c2.log 2>&1
stamp "c2 end; c1 repeat (runs 16-31) start"
$D --rows 200,1000,10000 --runs 16 --run-start 16 --cores 1 --pin-server 2 --bin-a $BIN/kds_server-A --bin-b $BIN/kds_server-B \
   --workdir $R/w-c1r --out $R/overhead-c1 > $R/overhead-c1r.log 2>&1
stamp "c1 repeat end; strict start"
$D --rows 200,1000,10000 --runs 6 --ops 600 --cores 1 --durability strict --pin-server 2 --bin-a $BIN/kds_server-A --bin-b $BIN/kds_server-B \
   --workdir $R/w-strict --out $R/strict-c1 > $R/strict-c1.log 2>&1
stamp "strict end"
touch $R/all.done
