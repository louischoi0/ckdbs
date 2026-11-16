#!/bin/bash
conf=$2
grep -q buffer_pool_frames "$conf" || echo "buffer_pool_frames = 65536" >> "$conf"
exec /tmp/claude-1000/-home-louis-ckdbs/27596f25-ceea-4694-a7ec-1a2566519ac2/scratchpad/g21/bin/kds_server-B "$@"
