#!/bin/bash
# s3 sampled with the sampler armed 30 s after the driver starts (load+create-index ~29 s), so the dump holds the read phases.
OUT=$1; CELL=$OUT/prof-c13-s3reads-r1; mkdir -p "$CELL"
BIN=/home/cdkbs/bench-runs/bz/kds_server.c13; SO=/home/cdkbs/bench-runs/bz/bin/kds_sampler.so
W=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer
D=/home/cdkbs/bench-runs/bz/data/s3r; rm -rf $D; mkdir -p $D; PORT=15703
{ echo "data_file = $D/kds.db"; echo "cores = 8"; echo "port = $PORT"; echo "durability = group"
  echo "log_dir = $D"; echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > $CELL/s3.conf
LD_PRELOAD=$SO KDS_SAMPLER_HZ=500 KDS_SAMPLER_OUT=$CELL/s3.dump nohup $BIN --config $CELL/s3.conf > $D/stdout.log 2>&1 &
P=$!
for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.2; done
export PYTHONPATH=$W/tools
echo "$(date -u +%FT%TZ) driver start" > $OUT/status
python3 $W/tools/scenario3_library.py --port $PORT --suffix x --loans 10000 --index-mode all --server-indexes on --seed 1 --assert-index-reads --ops 3000 --json $CELL/s3.json > $CELL/s3.stdout 2>&1 &
DP=$!
sleep 30; kill -USR1 $P; echo "$(date -u +%FT%TZ) armed" >> $OUT/status
wait $DP; echo $? > $CELL/s3.rc
kill -USR2 $P; sleep 1
kill -TERM $P; sleep 2; kill -0 $P 2>/dev/null && kill -KILL $P
rm -rf $D; echo "$(date -u +%FT%TZ) DONE" >> $OUT/status; touch $OUT/done3
