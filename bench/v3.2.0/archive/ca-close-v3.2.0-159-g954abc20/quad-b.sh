#!/bin/bash
# quad.sh BINARY ARM OUTDIR REP [CONF_LINE ...]
# One pass of the reduced scenario quad against BINARY with the extra config lines.
# Env: SCEN="1 2 3 4" (default all), TOOLS (default: this worktree's tools/), DATA (default
# /home/cdkbs/bench-runs/bz/data), PORT_BASE (default 15700), SAMPLER=/path/kds_sampler.so
# (profile mode: LD_PRELOAD, SIGUSR1/SIGUSR2 bracket the driver; s4 samples the `on` server only),
# S1_ARGS S2_ARGS S3_ARGS S4_ARGS override the reduced sizes. Output: OUTDIR/ARM-rREP/*.
set -uo pipefail
BIN=$1; ARM=$2; OUT=$3; REP=$4; shift 4
HERE=/home/cdkbs/ckdbs/.claude/worktrees/bz-background-advisory-optimizer/bench/v3.2.0/archive/ca-harness
ROOT=$(cd "$HERE/../../../.." && pwd)
TOOLS=${TOOLS:-$ROOT/tools}
DATA=${DATA:-/home/cdkbs/bench-runs/bz/data}
PB=${PORT_BASE:-15700}
SCEN=${SCEN:-"1 2 3 4"}
S1_ARGS=${S1_ARGS:---years 3 --symbols 6 --exchanges 2 --ops 100 --qps-ops 60 --compare-rounds 2 --conn-ops 60 --write-ops 600 --write-batches 1,100 --connections 1,4}
S2_ARGS=${S2_ARGS:---organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync}
S3_ARGS=${S3_ARGS:---loans 10000 --index-mode all --server-indexes on --seed 1 --assert-index-reads}
S4_ARGS=${S4_ARGS:---days 2 --blocks 4 --session-seconds 12 --overnight-seconds 8}
CELL=$OUT/$ARM-r$REP; mkdir -p "$CELL" "$DATA"
export PYTHONPATH=$TOOLS
PYS1=$HERE/scenario1_backtest_btree_copy.py
EXTRA=("$@")

start_server() { # tag port cores preload [conf lines...]
    local TAG=$1 PORT=$2 CORES=$3 PRE=$4; shift 4
    local D=$DATA/$TAG; rm -rf "$D"; mkdir -p "$D"
    { echo "data_file = $D/kds.db"; echo "cores = $CORES"; echo "port = $PORT"
      echo "durability = group"; echo "log_dir = $D"; echo "log_file = kds.log"
      echo "log_level = warn"; echo "buffer_pool_frames = 65536"
      for l in "$@" "${EXTRA[@]}"; do [ -n "$l" ] && echo "$l"; done; } > "$CELL/$TAG.conf"
    if [ -n "$PRE" ]; then
        LD_PRELOAD=$SAMPLER KDS_SAMPLER_OUT=$CELL/$TAG.dump nohup "$BIN" --config "$CELL/$TAG.conf" > "$D/stdout.log" 2>&1 &
    else
        nohup "$BIN" --config "$CELL/$TAG.conf" > "$D/stdout.log" 2>&1 &
    fi
    echo $! > "$D/pid"
    for i in $(seq 1 150); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
    echo "server $TAG did not open port $PORT" >&2; return 1
}
stop_server() { # tag
    local D=$DATA/$1 P; P=$(cat "$D/pid" 2>/dev/null)
    [ -n "$P" ] && kill -TERM "$P" 2>/dev/null
    for i in $(seq 1 50); do kill -0 "$P" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$P" 2>/dev/null && kill -KILL "$P" 2>/dev/null
    cp "$D/kds.log" "$CELL/$1.serverlog" 2>/dev/null; rm -rf "$D"
}
precheck() { { echo "time: $(date -u +%FT%TZ)"; echo "loadavg: $(cat /proc/loadavg)"
    echo "foreign:"; pgrep -a -f "cc1plus|cmake --build|ctest" | grep -v pgrep | grep -v "quad.sh" | sed 's/^/  /'; } > "$CELL/$1.precheck"; }
sig() { [ -n "${SAMPLER:-}" ] && kill -"$1" "$(cat "$DATA/$2/pid")" 2>/dev/null; sleep 1; }

for S in $SCEN; do
    TAG=s$S; PORT=$((PB + S)); precheck $TAG; T0=$(date +%s.%N)
    PRE=""; [ -n "${SAMPLER:-}" ] && PRE=1
    if [ "$S" = 4 ]; then
        C1="decay_half_life = 5"; C2="cabin_optimizer_snapshot_interval_ms = 500"
        start_server s4-off $((PB+41)) 1 "$PRE" "$C1" "$C2" && start_server s4-on $((PB+42)) 1 "$PRE" "$C1" "$C2" && \
        start_server s4-declared $((PB+43)) 1 "" "$C1" "$C2" || { echo FAIL > "$CELL/$TAG.rc"; continue; }
        sig USR1 s4-on; sig USR1 s4-off
        python3 "$TOOLS/scenario4_cabinopt_days.py" --port-off $((PB+41)) --port-on $((PB+42)) --port-declared $((PB+43)) \
          --pid-off "$(cat $DATA/s4-off/pid)" --pid-on "$(cat $DATA/s4-on/pid)" --pid-declared "$(cat $DATA/s4-declared/pid)" \
          --suffix a $S4_ARGS --json "$CELL/$TAG.json" > "$CELL/$TAG.stdout" 2>&1
        echo $? > "$CELL/$TAG.rc"
        sig USR2 s4-on; sig USR2 s4-off
        for A in off on declared; do stop_server s4-$A; done
    else
        start_server $TAG $PORT 8 "$PRE" || { echo FAIL > "$CELL/$TAG.rc"; continue; }
        sig USR1 $TAG
        case $S in
        1) python3 "$PYS1" --port $PORT --suffix x --seed 1 --bars-clustered btree --verify --sync $S1_ARGS --json "$CELL/$TAG.json" > "$CELL/$TAG.stdout" 2>&1 ;;
        2) python3 "$TOOLS/scenario2_freight.py" --port $PORT --suffix x $S2_ARGS --json "$CELL/$TAG.json" > "$CELL/$TAG.stdout" 2>&1 ;;
        3) python3 "$TOOLS/scenario3_library.py" --port $PORT --suffix x $S3_ARGS --json "$CELL/$TAG.json" > "$CELL/$TAG.stdout" 2>&1 ;;
        esac
        echo $? > "$CELL/$TAG.rc"
        sig USR2 $TAG
        stop_server $TAG
    fi
    echo "$(echo "$(date +%s.%N) - $T0" | bc) s" > "$CELL/$TAG.wall"
done
