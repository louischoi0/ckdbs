#!/bin/sh
# EE-S0's cells, from a hashed copy of the release binary (bench/README rule 3),
# each cell's host load recorded before it (rule 4).
R=/home/ukjin/ee-s0-run/final
W=/home/ukjin/workspaces/ckdbs/.claude/worktrees/ee-decide-slot
B=$R/kds_server
cd $W || exit 1
host() {
    echo "cell $1 $(date -u +%H:%M:%S) load $(cat /proc/loadavg) builds=$(pgrep -f 'cc1plus|cmake --build|ctest' | wc -l)" >> $R/host.txt
    echo "$1 start $(date -u +%H:%M:%S)" >> $R/progress
}
host wake
taskset -c 8-15 python3 tools/ee_s0_premise.py --binary $B --workdir $R/w-wake --port 15961 --cell wake --rounds 60 --cpus 2,3 --json $R/wake.json > $R/wake.out 2>&1 || echo "wake FAILED" >> $R/progress
host wake-drain0
taskset -c 8-15 python3 tools/ee_s0_premise.py --binary $B --workdir $R/w-wake0 --port 15962 --cell wake --rounds 30 --cpus 2,3 --drain-us 0 --json $R/wake-drain0.json > $R/wake0.out 2>&1 || echo "wake0 FAILED" >> $R/progress
for t in 0 2; do
    host hot$t
    taskset -c 8-15 python3 tools/ee_s0_premise.py --binary $B --workdir $R/w-hot$t --port 1596$((3 + t)) --cell hot --cores 4 --cpus 0,1,2,3 --sessions 8 --seconds 15 --think-ms $t --json $R/hot$t.json > $R/hot$t.out 2>&1 || echo "hot$t FAILED" >> $R/progress
done
host fence8
D=$R/w-fence8
rm -rf $D; mkdir -p $D
printf "data_file = $D/ee.db\nport = 15966\ncores = 8\ndurability = relaxed\nbuffer_pool_frames = 65536\nlog_file = ee.log\nlog_dir = $D\nlog_level = warn\nreactor_cpus = 0,1,2,3,4,5,6,7\n" > $D/ee.conf
taskset -c 0-7 $B --config $D/ee.conf > $D/stderr 2>&1 &
PID=$!
sleep 2
taskset -c 8-15 python3 tools/cp_s0_fence.py --port 15966 --writers 8 --ops 2000 --blocks 8 --label kwp-w8-c8 --server-pid $PID --cpu-schedstat --json $R/fence8.json > $R/fence8.out 2>&1 || echo "fence8 FAILED" >> $R/progress
kill $PID
wait $PID 2>/dev/null
host end
echo "DONE $(date -u +%H:%M:%S)" >> $R/progress
