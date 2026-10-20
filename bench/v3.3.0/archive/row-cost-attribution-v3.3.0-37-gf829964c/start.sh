#!/bin/bash
# Start KDS (cores=1, group) pinned to CPU 2 from the hash-checked copy; record the pid.
# usage: start.sh [LD_PRELOAD-sampler-or-empty] [binary]
R=/home/cdkbs/bench-runs/row-cost
BIN=${2:-/home/cdkbs/bench-runs/v3.3.0-33-g9682b725/kds_server}
if [ -z "$2" ]; then
  echo "7532bd14afb2acd4f9e48cac00732f899cb8e59b26fa1d9b982047fc89272f2a  $BIN" | sha256sum -c - || exit 1
fi
rm -rf $R/data; mkdir -p $R/data
cat > $R/kds.conf <<C
data_file = $R/data/kds.db
cores = 1
port = 15690
durability = group
log_dir = $R/data
log_file = kds.log
log_level = warn
buffer_pool_frames = 65536
C
if [ -n "$1" ]; then export LD_PRELOAD=$1 KDS_SAMPLER_OUT=$R/sample.dump; fi
nohup taskset -c 2 $BIN --config $R/kds.conf > $R/stdout.log 2>&1 &
echo $! > $R/kds.pid
