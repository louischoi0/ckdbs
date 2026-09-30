#!/bin/bash
# Shared helpers for the 9a0525d scenario rebaseline. Source, do not run.
RUN=/home/cdkbs/bench-runs/rebaseline-9a0525d
TOOLS=/home/cdkbs/ckdbs/.claude/worktrees/bench-rerun-scenarios/tools
BIN=$RUN/kds_server
mkdir -p "$RUN/logs" "$RUN/json" "$RUN/data" "$RUN/conf"

precheck() {  # tag
    {
        echo "time: $(date -u +%FT%TZ)"
        echo "loadavg: $(cat /proc/loadavg)"
        echo "pgrep cc1plus|cmake --build|ctest:"
        pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep | grep -v rebaseline || echo "  none"
    } > "$RUN/logs/$1.precheck" 2>&1
}

# start_server tag port cores durability [extra conf lines...]
start_server() {
    local TAG=$1 PORT=$2 CORES=$3 DUR=$4; shift 4
    local DIR=$RUN/data/$TAG
    rm -rf "$DIR"; mkdir -p "$DIR"
    {
        echo "data_file = $DIR/kds.db"
        echo "cores = $CORES"
        echo "port = $PORT"
        echo "durability = $DUR"
        echo "log_dir = $DIR"
        echo "log_file = kds.log"
        echo "log_level = warn"
        for l in "$@"; do echo "$l"; done
    } > "$RUN/conf/$TAG.conf"
    nohup "$BIN" --config "$RUN/conf/$TAG.conf" > "$DIR/stdout.log" 2>&1 &
    echo $! > "$DIR/pid"
    for i in $(seq 1 150); do
        (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
        sleep 0.2
    done
    echo "server $TAG did not open port $PORT" >&2
    return 1
}

stop_server() {  # tag
    local DIR=$RUN/data/$1
    local PID=$(cat "$DIR/pid" 2>/dev/null)
    [ -n "$PID" ] && kill -TERM "$PID" 2>/dev/null
    for i in $(seq 1 50); do kill -0 "$PID" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$PID" 2>/dev/null && kill -KILL "$PID" 2>/dev/null
    cp "$DIR/kds.log" "$RUN/logs/$1.serverlog" 2>/dev/null
    rm -rf "$DIR/kds.db"* "$DIR"/wal* 2>/dev/null
    rm -rf "$DIR"
}
