#!/bin/bash
# Start server L from the copied binary, load every relation, stop it cleanly, keep the file as the pristine copy.
S=/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cc-s0
R=/home/cdkbs/bench-runs/cc-s0
rm -rf $R/load; mkdir -p $R/load $S/out
cat > $R/load/L.conf <<EOF
data_file = $R/load/pristine.db
port = 25600
cores = 1
durability = group
buffer_pool_frames = 65536
log_file = L.log
log_dir = $R/load
log_level = warn
EOF
date -u '+%FT%TZ load start' > $S/load.status
$R/kds_server --config $R/load/L.conf > $R/load/L.stderr 2>&1 &
LP=$!
for i in $(seq 1 100); do (exec 3<>/dev/tcp/127.0.0.1/25600) 2>/dev/null && break; sleep 0.2; done
S2='[{"suffix":"n200","args":["--organizations","20","--ships","5","--operations","20","--cargos","200","--bookings","180","--seconds","120","--seed","1"]},
{"suffix":"n1k","args":["--organizations","50","--ships","10","--operations","50","--cargos","1000","--bookings","900","--seconds","120","--seed","1"]},
{"suffix":"ca","args":["--organizations","300","--ships","30","--operations","300","--cargos","10000","--bookings","9000","--seconds","300","--seed","1"]},
{"suffix":"df","args":["--seconds","25","--seed","1"]}]'
python3 $S/cc_s0_load.py --port 25600 --out $S/out --s2 "$S2" > $S/load.log 2>&1
echo "load rc=$?" >> $S/load.log
kill -TERM $LP
for i in $(seq 1 100); do kill -0 $LP 2>/dev/null || break; sleep 0.3; done
kill -0 $LP 2>/dev/null && kill -KILL $LP
ls -la $R/load >> $S/load.log
date -u '+%FT%TZ load done' >> $S/load.status
echo done > $S/load.done
