#!/bin/bash
# srv.sh start|stop <label> <binary> <port> <cores>
S=/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-fix-ddl-build-fault-net/2fe39e3f-40fc-42ed-910b-fde09fa4cb64/scratchpad
D=$HOME/a1-run/$2
if [ "$1" = start ]; then
  rm -rf $D; mkdir -p $D/wal
  cat > $D/kds.conf <<EOF
data_file = $D/kds.db
wal_dir = $D/wal
port = $4
log_level = warn
log_dir = $D
log_file = kds.log
durability = relaxed
cores = $5
buffer_pool_frames = 131072
$EXTRA
EOF
  setsid nohup $3 --config $D/kds.conf > $D/stdout.log 2>&1 &
  echo $! > $D/pid
  for i in $(seq 1 100); do grep -q "listening on" $D/stdout.log && exit 0; sleep 0.2; done
  echo "server did not start"; cat $D/stdout.log; exit 1
else
  kill $(cat $D/pid); sleep 1; kill -0 $(cat $D/pid) 2>/dev/null && kill -9 $(cat $D/pid)
  rm -rf $D/kds.db $D/wal
  exit 0
fi
