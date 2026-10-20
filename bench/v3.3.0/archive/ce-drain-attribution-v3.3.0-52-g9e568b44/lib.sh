#!/bin/bash
RUN=/home/cdkbs/bench-runs/ce-drain-attribution-v3.3.0-52-g9e568b44
TOOLS=$RUN/tools
status() { echo "$(date -u +%FT%TZ) $*" > "$RUN/status"; }
tl() { echo "$*" >> "$RUN/timeline"; }
free_kb() { df --output=avail / | tail -1; }
busy_pct() {
    local i1 t1 i2 t2 u n s i w q sq st
    read -r _ u n s i w q sq st _ < /proc/stat
    i1=$((i + w)); t1=$((u + n + s + i + w + q + sq + st))
    sleep 2
    read -r _ u n s i w q sq st _ < /proc/stat
    i2=$((i + w)); t2=$((u + n + s + i + w + q + sq + st))
    echo $(( 100 * ( (t2 - t1) - (i2 - i1) ) / (t2 - t1) ))
}
foreign_procs() { pgrep -a -f "cc1plus|cmake --build|ctest|kds_tests" | grep -v "pgrep" || true; }
quiet_gate() {  # 4 consecutive quiet checks (~9 s), capped at 240 s
    local TAG=$1 waited=0 b procs q=0
    while :; do
        procs=$(foreign_procs); b=$(busy_pct)
        if [ -z "$procs" ] && [ "$b" -le 8 ]; then
            q=$((q + 1)); [ "$q" -ge 4 ] && break
            status "$TAG quiet check $q/4: busy=${b}%"; waited=$((waited + 2)); sleep 0.3; continue
        fi
        q=0; status "$TAG waiting for a quiet host: busy=${b}% procs=$(echo "$procs" | grep -c .)"
        waited=$((waited + 5)); [ "$waited" -ge 240 ] && { touch "$RUN/flag/$TAG.gate-timeout"; break; }
        sleep 3
    done
    echo "$waited" > "$RUN/logs/$TAG.gatewait"
}
start_server() {  # tag port bin pin
    local TAG=$1 PORT=$2 BIN=$3 PIN=$4 DIR=$RUN/data/$1 i
    rm -rf "$DIR"; mkdir -p "$DIR"
    { echo "data_file = $DIR/kds.db"; echo "cores = 1"; echo "port = $PORT"; echo "durability = group"
      echo "log_dir = $DIR"; echo "log_file = kds.log"; echo "log_level = warn"; echo "buffer_pool_frames = 65536"; } > "$RUN/conf/$TAG.conf"
    nohup taskset -c "$PIN" "$BIN" --config "$RUN/conf/$TAG.conf" > "$DIR/stdout.log" 2>&1 &
    echo $! > "$DIR/pid"
    for i in $(seq 1 150); do
        (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
        sleep 0.2
    done
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
