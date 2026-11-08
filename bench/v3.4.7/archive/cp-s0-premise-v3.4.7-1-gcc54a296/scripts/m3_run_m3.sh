#!/bin/bash
# CP-S0 M3 - the lock family under two scenario drivers on the baseline
# kds_server, `group` durability, 4 cores pinned to CPUs 2-5, the lock
# instruments (`sys.lock_waits`, `sys.wait_stats`, `sys.statement_stats`,
# `sys.meta`, `sys.ash`) read after each cell by read_views.py beside this
# file. Order: `instructions/v3.2.0/workorder-cp-lock-model-exploration.md:594`
# (the CP-S0 row, **M3**) and §3 C2's "S0 measures" (`:403-410`). Nothing
# here is a result; the results file is `bench/v3.4.7/results-cp-s0-premise-<describe>.md`.
#
# Server launcher: ../m1m4/srv.py, whose interface AS WRITTEN at the time of
# this script is
#     srv.py start --binary BIN --workdir DIR --port P [--debug-text-port D]
#                  [--kwp-port K] [--cores N] [--taskset 2-5] [--reactor-cpus ...]
#                  [--durability strict|group|relaxed] [--buffer-pool-frames N]
#                  [--ash on|off] [--log-level L] [--conf-extra "key = value"]...
#                  [--label NAME] [--allow-tmpfs] [--timeout S]
#     srv.py stop --workdir DIR [--timeout S]
#     srv.py status --workdir DIR
# (the orchestrator's note assumed `--bin B --text-port T --kwp-port K
# --pin 2-5`; the real flags are `--binary`, `--debug-text-port`, `--taskset`,
# and `--durability` / `--cores` as assumed). srv.py REFUSES a workdir on
# tmpfs (bench/README.md rule 2); /tmp - and so the session scratchpad - is
# tmpfs on this host (`df -T /tmp`), so the data files go under WORKROOT on
# ext4 (default /var/tmp/cp-s0-m3) and only logs and JSON land here.
# srv.py also refuses a workdir that already holds a data file: every cell
# is a fresh server on a fresh volume, so $WORKROOT/s2 and $WORKROOT/s0 must
# hold no kds.db or server.json before a run (the preflight below checks
# exactly that; an empty WORKROOT passes). Set WORKROOT to a new path to
# keep an earlier run's volumes.
#
# ASH: `ash_enabled = on` is the default (`kds.conf.sample:445`,
# `expeditor.hpp:285`); `ash_interval_ms` is set to 100 through srv.py's
# `--conf-extra` (the key is read at `expeditor.cpp:377-385` and checked
# against [100, 10000], `ash_sampler.hpp:56-57`), so a 60 s run gives ~600
# grid points per core and one sample per active task per point.
#
# Drivers (both speak KWP/1 through `tools/ckdbs_cli.ServerConnection`,
# `scenario2_freight.py:71,270`, `scenario0_stockmarket.py:155,305`; the
# newline debug port is opened too, for read_views.py's `--text-port`
# fallback, but nothing here depends on it):
#
# * Scenario 2, `tools/scenario2_freight.py`: three invocations with one
#   `--suffix`, as `bench/v3.3.0/results-scenario2-freight-v3.3.0-38-ga40882af.md:11`
#   ("`--schema-only`, then `--load-only`, then the run on the same server")
#   and its archive's `run_all.sh:22-30` did. Sizes are that file's Driver
#   row (`:20`): `--organizations 300 --ships 30 --operations 300 --bookers 8
#   --verify 100 --seed 1 --sync`, but **`--cargos` is raised and `--bookings`
#   dropped** (see S2_CARGOS below): the Driver row's `--cargos 4000
#   --bookings 3000` ends the run at 3,000 commits in ~1.5-2 s, and with
#   `--seconds 60` in place of `--bookings` the run ends when the cargo pool
#   runs out - every booking consumes one cargo and the pool is split across
#   bookers (`partition`, `scenario2_freight.py:989-1003`;
#   `run_bookings` breaks on `book_once` returning None, `:964-966`) - which
#   at ~1,500-2,000 bookings/s is ~2-3 s for 4,000 cargos. 24,000 cargos
#   last ~12-16 s of booking and load in ~30 s per load pass at the
#   single-connection, one-commit-per-INSERT rate the v3.3.0 archive shows
#   (`load-cargos` mean 1,209 us at `s2-c1-s`); S2_CARGOS=100000 would fill
#   the 60 s but costs ~2 min per load pass. Note two quirks of the driver's
#   `main()` the three-phase sequence inherits: every invocation runs
#   `create_tables` (an existing name answers EXISTS, not ERR,
#   `command_dispatcher.cpp:5196-5199`), and every invocation that is not
#   `--schema-only` loads the reference data again (`scenario2_freight.py:1634-1651`
#   runs unconditionally before the `--load-only` exit at `:1660-1664`), so
#   the booking run's bookers draw from the rows the third invocation loaded
#   and the relations hold two loads' reference rows. That reload sits
#   between the s2-pre and s2-post readings, so the `sys.wait_stats` delta
#   carries ~30 s of single-connection load; read_views.py's ASH concurrent
#   window (bins with >= 2 client sessions) is the undiluted view of the
#   booking phase, and the results file quotes both. `--contend` (the
#   default, `:1512-1516`) makes every booker draw from every voyage and
#   every customer, so two bookers can meet on one `operations` or
#   `organizations` row - the contention M3 is after. No `--fk` (the order
#   names scenario 2 as `--bookers 8` alone); S2_EXTRA adds flags.
#
# * Scenario 0, `tools/scenario0_stockmarket.py`: one invocation does the
#   load, the run, `--verify` and `--sync` (there is no `--load-only`), so
#   the post-run readings include the load's 8,000 single-connection INSERTs
#   (users, assets, accounts); the pre-run snapshot is taken right after
#   boot, and read_views.py's `--baseline` delta is boot-to-end. `--txn`
#   wraps the four statements in BEGIN/COMMIT (`:1142-1150`; the trader loop
#   `:659-660`, `:687-690`), `--fk` declares `trades.account_id REFERENCES
#   accounts` (`:1154-1162`, `create_tables` `:425-455`) - both off by
#   default. `--traders N` is admitted for any N >= 1 (`:1202-1203`) given
#   at least 2 accounts per trader (`:1249-1250`), and each trader owns a
#   disjoint round-robin partition of accounts (`:86-88`, `:1260`:
#   "two traders never touch one account"). **Therefore scenario 0 cannot
#   show two writers on one parent**: no two sessions ever write one
#   `accounts` row, so the conversion deadlock of `foreign-keys.md` §2c and
#   §2c's parent-UPDATE wait have no contender here. M3 on scenario 0 is
#   **C2's price on the uncontended path** - the FK forward check's `S` on
#   the parent row and its `IS` on the parent relation, taken per trade
#   INSERT and held to the decide (D9(a)) - and zero FK contention by
#   construction (the order's §1.3 `:187-195` and C2 `:403-410`). The
#   results file must say so. A shared-account arm needs a driver flag the
#   driver does not have (C2: "taken only if bench/README.md admits a driver
#   change inside a measurement stage"); this script does not add one.
#   Sizes: `--users 2000 --assets 2000 --traders 8 --seconds 60`, ~4,000
#   accounts, so load (~10 s) + run (60 s) + verify fits in ~1.5 min.
#
# Watchdog: this script appends a line per phase to $OUT/progress.txt and
# rewrites $OUT/heartbeat every 15 s (CLAUDE.md: "polled by a file it
# writes, never by a process pattern"); a reader that sees the heartbeat
# older than a minute has found the run orphaned. Every driver invocation
# runs under `timeout`.
#
# Usage:  bash run_m3.sh            (DRY_RUN=1 bash run_m3.sh prints the commands)
# Knobs (environment): TREE TOOLS BIN SRV OUT WORKROOT S2_PORT S2_TEXT_PORT
#   S0_PORT S0_TEXT_PORT CORES TASKSET DRIVER_CPUS DURABILITY ASH_INTERVAL_MS
#   BUFFER_POOL_FRAMES S2_CARGOS S2_BOOKINGS S2_SECONDS S2_EXTRA S0_USERS
#   S0_ASSETS S0_TRADERS S0_SECONDS S0_EXTRA HOST_SAMPLER SKIP_S2 SKIP_S0
set -uo pipefail
exec 3>&1                                            # the console, for `run`'s command echo

HERE=$(cd "$(dirname "$0")" && pwd)
SCRATCH=$(cd "$HERE/.." && pwd)                      # .../scratchpad/cp-s0
TREE=${TREE:-/home/louis/ckdbs/.claude/worktrees/cp-lock-model-exploration}
TOOLS=${TOOLS:-$TREE/tools}
BIN=${BIN:-$SCRATCH/bin/kds_server-base}             # the baseline copy; never build-release itself
SRV=${SRV:-$SCRATCH/m1m4/srv.py}
READ_VIEWS=${READ_VIEWS:-$HERE/read_views.py}
OUT=${OUT:-$HERE/out}
WORKROOT=${WORKROOT:-/var/tmp/cp-s0-m3}              # ext4: srv.py refuses tmpfs
HOST_SAMPLER=${HOST_SAMPLER:-$SCRATCH/host_sampler.sh}

S2_PORT=${S2_PORT:-25731}; S2_TEXT_PORT=${S2_TEXT_PORT:-25732}
S0_PORT=${S0_PORT:-25741}; S0_TEXT_PORT=${S0_TEXT_PORT:-25742}
CORES=${CORES:-4}
TASKSET=${TASKSET:-2-5}                              # the server's CPUs
DRIVER_CPUS=${DRIVER_CPUS:-0,1,6,7}                  # the Python drivers', off the server's
DURABILITY=${DURABILITY:-group}
ASH_INTERVAL_MS=${ASH_INTERVAL_MS:-100}
BUFFER_POOL_FRAMES=${BUFFER_POOL_FRAMES:-65536}
DRIVER_TIMEOUT=${DRIVER_TIMEOUT:-900}                # seconds per driver invocation

S2_CARGOS=${S2_CARGOS:-24000}
S2_BOOKINGS=${S2_BOOKINGS:-0}                        # 0: time-based, --seconds is the budget
S2_SECONDS=${S2_SECONDS:-60}
S2_SIZES="--organizations 300 --ships 30 --operations 300 --cargos $S2_CARGOS"
S2_COMMON="--verify 100 --seed 1 --sync"
S2_RUN="--bookers 8 --seconds $S2_SECONDS --bookings $S2_BOOKINGS"
S2_EXTRA=${S2_EXTRA:-}                               # e.g. --fk, --isolation repeatable-read
S2_SUFFIX=${S2_SUFFIX:-m3s2}

S0_USERS=${S0_USERS:-2000}; S0_ASSETS=${S0_ASSETS:-2000}
S0_TRADERS=${S0_TRADERS:-8}; S0_SECONDS=${S0_SECONDS:-60}
S0_ARGS="--users $S0_USERS --assets $S0_ASSETS --traders $S0_TRADERS --seconds $S0_SECONDS --fk --txn --verify 200 --seed 1 --sync"
S0_EXTRA=${S0_EXTRA:-}                               # e.g. --no-profit
S0_SUFFIX=${S0_SUFFIX:-m3s0}

DRY_RUN=${DRY_RUN:-0}

mkdir -p "$OUT"
PROGRESS=$OUT/progress.txt
HEARTBEAT=$OUT/heartbeat
TIMELINE=$OUT/timeline.txt

now() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
say() { echo "$(now) $*" | tee -a "$PROGRESS"; }
tl()  { echo "$(now) $*" >> "$TIMELINE"; }
run() {  # echo (to the console, fd 3, and progress.txt) and run; only echo under DRY_RUN
    echo "+ $*" >&3
    echo "+ $*" >> "$PROGRESS"
    [ "$DRY_RUN" = 1 ] && return 0
    "$@"
}

HB_PID=
start_heartbeat() {
    [ "$DRY_RUN" = 1 ] && return
    ( while :; do date -u +%s > "$HEARTBEAT"; sleep 15; done ) &
    HB_PID=$!
}
SAMPLER_PID=
start_sampler() {
    [ "$DRY_RUN" = 1 ] && return
    if [ -x "$HOST_SAMPLER" ]; then
        rm -f "$OUT/host.stop"
        bash "$HOST_SAMPLER" "$OUT/host.log" "$OUT/host.stop" &
        SAMPLER_PID=$!
    fi
}
cleanup() {
    [ -n "$HB_PID" ] && kill "$HB_PID" 2>/dev/null
    if [ -n "$SAMPLER_PID" ]; then touch "$OUT/host.stop"; wait "$SAMPLER_PID" 2>/dev/null; fi
    for d in "$WORKROOT/s2" "$WORKROOT/s0"; do
        if [ "$DRY_RUN" != 1 ] && [ -f "$d/server.json" ] && python3 "$SRV" status --workdir "$d" 2>/dev/null | grep -q '"alive_now": true'; then
            say "cleanup: stopping a server still alive in $d"
            python3 "$SRV" stop --workdir "$d" >> "$OUT/cleanup.log" 2>&1
        fi
    done
}
trap cleanup EXIT

# ---- preflight ---------------------------------------------------------------
say "M3 preflight"
if [ "$DRY_RUN" != 1 ]; then
    [ -x "$BIN" ] || { say "no executable server at BIN=$BIN"; exit 2; }
    [ -f "$SRV" ] || { say "no launcher at SRV=$SRV"; exit 2; }
    [ -f "$READ_VIEWS" ] || { say "no read_views.py at $READ_VIEWS"; exit 2; }
    [ -f "$TOOLS/scenario2_freight.py" ] && [ -f "$TOOLS/scenario0_stockmarket.py" ] || { say "drivers missing under TOOLS=$TOOLS"; exit 2; }
    python3 -c "import sys; sys.path.insert(0, '$TOOLS'); import ckdbs_cli, sys_meta, kwp" || { say "tools/ imports fail"; exit 2; }
    for d in "$WORKROOT/s2" "$WORKROOT/s0"; do
        [ -e "$d/kds.db" ] || [ -e "$d/server.json" ] && { say "$d already holds a server; srv.py refuses a reused workdir - set WORKROOT to a fresh path"; exit 2; }
    done
    for p in $S2_PORT $S2_TEXT_PORT $S0_PORT $S0_TEXT_PORT; do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { say "port $p already accepts connections"; exit 2; }
    done
    {
        echo "started_utc=$(now)"
        echo "tree=$TREE"
        echo "describe=$(git -C "$TREE" describe --tags 2>/dev/null)"
        echo "head=$(git -C "$TREE" rev-parse --short HEAD 2>/dev/null)"
        echo "dirty=$(git -C "$TREE" status --porcelain -- src include tests CMakeLists.txt 2>/dev/null | wc -l) (build inputs)"
        echo "bin=$BIN sha256=$(sha256sum "$BIN" | cut -d' ' -f1)"
        echo "host=$(uname -srm) nproc=$(nproc) loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
        echo "workroot_fs=$(df -T "$WORKROOT" 2>/dev/null | tail -1 || df -T "$(dirname "$WORKROOT")" | tail -1)"
        echo "competing=$(pgrep -a -f 'cc1plus|cmake --build|ctest' | grep -v pgrep | wc -l)"
        echo "cores=$CORES taskset=$TASKSET driver_cpus=$DRIVER_CPUS durability=$DURABILITY ash_interval_ms=$ASH_INTERVAL_MS"
        echo "s2: $S2_SIZES $S2_COMMON $S2_RUN $S2_EXTRA (suffix $S2_SUFFIX)"
        echo "s0: $S0_ARGS $S0_EXTRA (suffix $S0_SUFFIX)"
    } > "$OUT/stamp.txt"
    cat "$OUT/stamp.txt" | tee -a "$PROGRESS"
fi
[ "$DRY_RUN" = 1 ] || mkdir -p "$WORKROOT" 2>/dev/null || true   # a dry run creates nothing on disk but $OUT
start_heartbeat
start_sampler

srv_start() {  # label workdir port textport
    local LABEL=$1 DIR=$2 PORT=$3 TPORT=$4
    run python3 "$SRV" start --binary "$BIN" --workdir "$DIR" --port "$PORT" \
        --debug-text-port "$TPORT" --cores "$CORES" --taskset "$TASKSET" \
        --durability "$DURABILITY" --buffer-pool-frames "$BUFFER_POOL_FRAMES" --ash on \
        --conf-extra "ash_interval_ms = $ASH_INTERVAL_MS" --log-level info --label "$LABEL" \
        > "$OUT/$LABEL.start.json" 2> "$OUT/$LABEL.start.err"
    local RC=$?
    [ "$DRY_RUN" = 1 ] && return 0
    if [ $RC -ne 0 ]; then say "$LABEL: srv.py start failed rc=$RC"; cat "$OUT/$LABEL.start.err" | tee -a "$PROGRESS"; return $RC; fi
    tl "$LABEL server up: $(cat "$OUT/$LABEL.start.json")"
    return 0
}
srv_stop() {  # label workdir
    local LABEL=$1 DIR=$2
    run python3 "$SRV" stop --workdir "$DIR" > "$OUT/$LABEL.stop.json" 2>&1
    local RC=$?
    [ "$DRY_RUN" = 1 ] && return 0
    tl "$LABEL server stop rc=$RC: $(cat "$OUT/$LABEL.stop.json")"
    cp "$DIR/server.json" "$OUT/$LABEL.server.json" 2>/dev/null
    cp "$DIR/kds.conf" "$OUT/$LABEL.kds.conf" 2>/dev/null
    cp "$DIR/server.out" "$OUT/$LABEL.server.out" 2>/dev/null
    cp "$DIR/kdb.log" "$OUT/$LABEL.kdb.log" 2>/dev/null
    return $RC
}
views() {  # label port textport prefix [baseline]
    local LABEL=$1 PORT=$2 TPORT=$3 PREFIX=$4 BASE=${5:-}
    local ARGS=(--port "$PORT" --out "$PREFIX" --label "$LABEL" --tools "$TOOLS")
    [ -n "$BASE" ] && ARGS+=(--baseline "$BASE")
    if ! run python3 "$READ_VIEWS" "${ARGS[@]}" > "$PREFIX.read.log" 2>&1; then
        say "$LABEL: read_views over KWP failed (see $PREFIX.read.log); retrying over the text port $TPORT"
        run python3 "$READ_VIEWS" "${ARGS[@]}" --text-port "$TPORT" >> "$PREFIX.read.log" 2>&1 \
            || say "$LABEL: read_views failed on both surfaces"
    fi
    [ "$DRY_RUN" = 1 ] || tail -n 40 "$PREFIX.read.log" | tee -a "$PROGRESS" >/dev/null
}
drive() {  # label logfile cmd...
    local LABEL=$1 LOG=$2; shift 2
    tl "$LABEL begin loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
    run timeout "$DRIVER_TIMEOUT" taskset -c "$DRIVER_CPUS" "$@" > "$LOG" 2>&1
    local RC=$?
    [ "$DRY_RUN" = 1 ] && return 0
    echo "$RC" > "$LOG.rc"
    tl "$LABEL end rc=$RC loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
    say "$LABEL rc=$RC"
    return $RC
}

# ---- cell 1: scenario 2, 8 contended bookers ----------------------------------
if [ "${SKIP_S2:-0}" != 1 ]; then
    say "== M3 scenario 2: cores $CORES, $DURABILITY, --bookers 8, $S2_SIZES, --seconds $S2_SECONDS, bookings $S2_BOOKINGS"
    if srv_start m3-s2 "$WORKROOT/s2" "$S2_PORT" "$S2_TEXT_PORT"; then
        S2=(python3 "$TOOLS/scenario2_freight.py" --port "$S2_PORT" --suffix "$S2_SUFFIX" $S2_SIZES $S2_COMMON $S2_RUN $S2_EXTRA --server-log "$WORKROOT/s2/kdb.log")
        drive s2-schema "$OUT/s2.schema.log" "${S2[@]}" --schema-only
        drive s2-load   "$OUT/s2.load.log"   "${S2[@]}" --load-only
        views s2-pre  "$S2_PORT" "$S2_TEXT_PORT" "$OUT/s2-pre"
        drive s2-run    "$OUT/s2.run.log"    "${S2[@]}" --json "$OUT/s2.json"
        views s2-post "$S2_PORT" "$S2_TEXT_PORT" "$OUT/s2-post" "$OUT/s2-pre.views.json"
        srv_stop m3-s2 "$WORKROOT/s2"
    fi
fi

# ---- cell 2: scenario 0, 8 traders on disjoint account partitions, --fk --txn ---
if [ "${SKIP_S0:-0}" != 1 ]; then
    say "== M3 scenario 0: cores $CORES, $DURABILITY, $S0_ARGS (disjoint partitions: C2's uncontended price, no two writers on one parent by construction)"
    if srv_start m3-s0 "$WORKROOT/s0" "$S0_PORT" "$S0_TEXT_PORT"; then
        views s0-pre  "$S0_PORT" "$S0_TEXT_PORT" "$OUT/s0-pre"
        drive s0-run "$OUT/s0.run.log" python3 "$TOOLS/scenario0_stockmarket.py" --port "$S0_PORT" --suffix "$S0_SUFFIX" \
            $S0_ARGS $S0_EXTRA --server-log "$WORKROOT/s0/kdb.log" --json "$OUT/s0.json"
        views s0-post "$S0_PORT" "$S0_TEXT_PORT" "$OUT/s0-post" "$OUT/s0-pre.views.json"
        srv_stop m3-s0 "$WORKROOT/s0"
    fi
fi

say "M3 done; outputs under $OUT (s2.json, s0.json, *-pre/*-post.views.json/.txt, *.log, stamp.txt, timeline.txt)"
[ "$DRY_RUN" = 1 ] || echo "finished_utc=$(now)" >> "$OUT/stamp.txt"
exit 0
