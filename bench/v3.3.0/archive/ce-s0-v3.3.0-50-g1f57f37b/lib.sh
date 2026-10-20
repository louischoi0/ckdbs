#!/bin/bash
# Shared helpers for the CE-S0 run (v3.3.0-50-g1f57f37b, engine binary = v3.3.0-33-g9682b725's). Source, do not run.
RUN=/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b
TOOLS=$RUN/tools
BIN=$RUN/kds_server
mkdir -p "$RUN/logs" "$RUN/json" "$RUN/data" "$RUN/conf" "$RUN/done" "$RUN/flag" "$RUN/contaminated"

status() { echo "$(date -u +%FT%TZ) $*" > "$RUN/status"; }
tl() { echo "$*" >> "$RUN/timeline"; }

busy_pct() {  # % of all CPUs busy over 3 s, integer
    local a b i1 t1 i2 t2
    read -r _ u n s i w q sq st _ < /proc/stat
    i1=$((i + w)); t1=$((u + n + s + i + w + q + sq + st))
    sleep 3
    read -r _ u n s i w q sq st _ < /proc/stat
    i2=$((i + w)); t2=$((u + n + s + i + w + q + sq + st))
    echo $(( 100 * ( (t2 - t1) - (i2 - i1) ) / (t2 - t1) ))
}

foreign_procs() { pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v "pgrep" || true; }

quiet_gate() {  # waits (up to 3600 s) for 6 consecutive quiet checks (~30 s): no build/test process and <= 8 % busy
    local TAG=$1 waited=0 b procs q=0
    while :; do
        procs=$(foreign_procs)
        b=$(busy_pct)
        if [ -z "$procs" ] && [ "$b" -le 8 ]; then
            q=$((q + 1)); [ "$q" -ge 6 ] && break
            status "$TAG quiet check $q/6: busy=${b}%"
            waited=$((waited + 3)); sleep 2; waited=$((waited + 2)); continue
        fi
        q=0
        status "$TAG waiting for a quiet host: busy=${b}% procs=$(echo "$procs" | grep -c .)"
        waited=$((waited + 3))
        if [ "$waited" -ge 3600 ]; then touch "$RUN/flag/$TAG.gate-timeout"; break; fi
        sleep 7; waited=$((waited + 7))
    done
    echo "$waited" > "$RUN/logs/$TAG.gatewait"
}

precheck() {
    quiet_gate "$1"
    {
        echo "time: $(date -u +%FT%TZ)"
        echo "loadavg: $(cat /proc/loadavg)"
        echo "gate wait s: $(cat $RUN/logs/$1.gatewait)"
        echo "pgrep cc1plus|cmake --build|ctest|kds_tests:"
        foreign_procs | sed 's/^/  /'
        [ -z "$(foreign_procs)" ] && echo "  none"
    } > "$RUN/logs/$1.precheck" 2>&1
}

# start_server tag port cores durability pin-cpu-or-empty [extra conf lines...]
start_server() {
    local TAG=$1 PORT=$2 CORES=$3 DUR=$4 PIN=$5; shift 5
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
        echo "buffer_pool_frames = 65536"
        for l in "$@"; do [ -n "$l" ] && echo "$l"; done
    } > "$RUN/conf/$TAG.conf"
    if [ -n "$PIN" ]; then
        nohup taskset -c "$PIN" "$BIN" --config "$RUN/conf/$TAG.conf" > "$DIR/stdout.log" 2>&1 &
    else
        nohup "$BIN" --config "$RUN/conf/$TAG.conf" > "$DIR/stdout.log" 2>&1 &
    fi
    echo $! > "$DIR/pid"
    local i
    for i in $(seq 1 150); do
        (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
        sleep 0.2
    done
    echo "server $TAG did not open port $PORT" >&2
    return 1
}

stop_server() {
    local DIR=$RUN/data/$1 PID i
    PID=$(cat "$DIR/pid" 2>/dev/null)
    [ -n "$PID" ] && kill -TERM "$PID" 2>/dev/null
    for i in $(seq 1 50); do kill -0 "$PID" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$PID" 2>/dev/null && kill -KILL "$PID" 2>/dev/null
    cp "$DIR/kds.log" "$RUN/logs/$1.serverlog" 2>/dev/null
    rm -rf "$DIR"
}

kill_stale_servers() {  # by pid file, never by pattern
    local f p
    for f in "$RUN"/data/*/pid; do
        [ -e "$f" ] || continue
        p=$(cat "$f")
        kill -TERM "$p" 2>/dev/null
        sleep 1
        kill -KILL "$p" 2>/dev/null
    done
    rm -rf "$RUN"/data/*
}
