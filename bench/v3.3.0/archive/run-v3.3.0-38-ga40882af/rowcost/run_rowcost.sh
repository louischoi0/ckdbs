#!/bin/bash
# KDS-only row-cost sweep: server pinned to CPU 2, client to CPU 6 (the attribution file's pinning); detached, resumable.
# Both a "new" client (this worktree's tools/) and an "old" client (a40882af^'s tools/) run, alternately, on fresh servers.
R=/home/cdkbs/bench-runs/v3.3.0-38-ga40882af
RC=$R/rowcost
source $R/lib.sh
echo $$ > $RC/master.pid
runone() {   # variant tools
    local V=$1 T=$2 PORT=15690
    [ -e $RC/done.$V ] && return
    precheck rowcost-$V
    local DIR=$RC/data-$V
    rm -rf $DIR; mkdir -p $DIR
    { echo "data_file = $DIR/kds.db"; echo "cores = 1"; echo "port = $PORT"; echo "durability = group"; echo "log_dir = $DIR"
      echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > $RC/$V.conf
    nohup taskset -c 2 $R/kds_server --config $RC/$V.conf > $DIR/stdout.log 2>&1 &
    local PID=$!
    echo $PID > $DIR/pid
    for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.2; done
    export PYTHONPATH=$T
    taskset -c 6 python3 $RC/kdsrow.py load --kds-port $PORT > $RC/$V.load 2>&1
    taskset -c 6 python3 $RC/kdsrow.py run --kds-port $PORT --kds-pid $PID --out $RC/cells-$V.jsonl --progress $RC/progress --reps 3 --target-s 3 > $RC/$V.run 2>&1
    kill -TERM $PID 2>/dev/null; sleep 2; kill -KILL $PID 2>/dev/null
    rm -rf $DIR
    touch $RC/done.$V
}
# the old client first would bias nothing: the server is fresh each time; run new, old
runone new $TOOLS
runone old $R/oldtools/tools
touch $RC/all.done
