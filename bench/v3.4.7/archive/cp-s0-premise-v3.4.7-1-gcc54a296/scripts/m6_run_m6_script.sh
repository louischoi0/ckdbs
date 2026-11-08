#!/usr/bin/env bash
# CP-S0 M6, the scripted half: both arms of tools/cp_s0_release_order.py against
# a fresh kds_server each, started from the baseline binary by the m1m4
# launcher. Prepared against instructions/v3.2.0/workorder-cp-lock-model-exploration.md
# section 5 (the CP-S0 row, "M6") at cc54a296; nothing here was run while
# writing it (preparation only; bash -n checked).
#
#   ./run_m6_script.sh                      # both arms: group, then strict
#   ARMS=group ./run_m6_script.sh           # one arm
#   DRY_RUN=1 ./run_m6_script.sh            # print the commands, start nothing
#   RUN_ROOT=/var/tmp/cp-s0-run/m6 ./run_m6_script.sh
#
# Launcher interface, read at m1m4/srv.py:444-471 (not the one the task
# assumed, which was `--bin / --text-port / --pin`):
#   srv.py start --binary BIN --workdir DIR --port P [--debug-text-port D]
#                [--cores N] [--taskset 2-5] [--reactor-cpus ...]
#                [--durability strict|group|relaxed] [--buffer-pool-frames N]
#                [--ash on|off] [--label NAME] [--allow-tmpfs] [--timeout S]
#   srv.py stop  --workdir DIR [--timeout S]
#   srv.py status --workdir DIR
# `start` blocks until the `listening on` banner and a TCP connect succeed
# (srv.py:342-360), prints one JSON line, and writes <workdir>/server.json;
# it refuses a workdir on tmpfs unless --allow-tmpfs (srv.py:241-245) and a
# workdir that already holds a server (srv.py:251-254). With --taskset and no
# --reactor-cpus it derives reactor_cpus from the mask (srv.py:270-280).
# `stop` sends SIGTERM and records the shutdown in the sidecar (srv.py:377-).
#
# Why a block device matters for THIS cell: the margin the cell measures is
# the group commit's sync latency (the drain tick, kds.conf.sample:249, plus
# the device's fsync). On tmpfs fsync is near-instant and the margin
# collapses toward Python's socket noise, so the default RUN_ROOT is under
# /var/tmp (ext4 on /dev/nvme0n1p1 on this box, `df -T /var/tmp`), not the
# scratchpad (tmpfs). ALLOW_TMPFS=1 passes --allow-tmpfs and the results file
# must say so.
#
# Both servers are started `--durability group` (the sample's default,
# kds.conf.sample:235) and `--cores 4`; the arm is session A's rung, set by
# the script with SET DURABILITY (protocol.md:127-128) and checked on every
# BEGIN reply. The server is pinned to CPUs 2-5 (reactor_cpus derived), the
# driver to 6-7 - the m1m4 convention (run_m1m4.sh header: on this box SMT
# siblings are (0,4) (1,5) (2,6) (3,7), so the driver shares physical cores
# with reactors 2 and 3; the cell is an ordering, not a throughput number).
#
# Ports (chosen, never 15432; m1m4 uses 15520-15525):
#   group  : KWP 15526  debug-text 15527
#   strict : KWP 15528  debug-text 15529
#
# Time budget (<= 3 minutes): per arm ~5 s start + ~5-15 s for 300 iterations
# (each iteration is two BEGINs, two UPDATEs, a COMMIT whose ack waits one
# drain tick plus an fsync, and a ROLLBACK) + ~5 s stop; two arms well under
# 1 minute on a local NVMe, under 2 on EBS. ITER / WARMUP override.
#
# Outputs under $OUT = m6/out/<stamp>/: m6-<arm>.txt (the script's stdout),
# m6-<arm>.json (--json), m6-<arm>.server.json (the sidecar after stop),
# m6-<arm>.server.out (the server's stdout/stderr), manifest.json, timeline.txt,
# heartbeat (touched every 30 s for the 60 s watchdog; poll $OUT/heartbeat and
# $OUT/phase, never a process pattern).

set -euo pipefail

WT=${WT:-/home/louis/ckdbs/.claude/worktrees/cp-lock-model-exploration}
SP=${SP:-/tmp/claude-1000/-home-louis-ckdbs/1c794fd2-a838-4425-a96a-e3b26a73757b/scratchpad/cp-s0}
M6=${M6:-$SP/m6}
SRV=${SRV:-$SP/m1m4/srv.py}
BIN=${BIN:-$SP/bin/kds_server-base}
SCRIPT=${SCRIPT:-$WT/tools/cp_s0_release_order.py}
RUN_ROOT=${RUN_ROOT:-/var/tmp/cp-s0-run/m6}
ALLOW_TMPFS=${ALLOW_TMPFS:-0}
DRY_RUN=${DRY_RUN:-0}
STAMP=${STAMP:-$(date -u '+%Y%m%dT%H%M%SZ')}
OUT=${OUT:-$M6/out/$STAMP}
ARMS=${ARMS:-"group strict"}
ITER=${ITER:-300}
WARMUP=${WARMUP:-5}
PROTOCOL=${PROTOCOL:-text}
CORES=${CORES:-4}
SERVER_CPUS=${SERVER_CPUS:-2-5}
DRIVER_CPUS=${DRIVER_CPUS:-6-7}
START_TIMEOUT=${START_TIMEOUT:-60}
STOP_TIMEOUT=${STOP_TIMEOUT:-60}

port_kwp()  { case "$1" in group) echo 15526 ;; strict) echo 15528 ;; *) echo 0 ;; esac; }
port_text() { case "$1" in group) echo 15527 ;; strict) echo 15529 ;; *) echo 0 ;; esac; }

utc() { date -u '+%Y-%m-%d %H:%M:%S UTC'; }
say() { echo "[$(utc)] $*" | tee -a "$OUT/timeline.txt"; }
run() {
    if [ "$DRY_RUN" = 1 ]; then echo "DRY: $*"; else "$@"; fi
}

mkdir -p "$OUT"
: > "$OUT/timeline.txt"
echo "preflight" > "$OUT/phase"

# ---- preflight ---------------------------------------------------------------
for f in "$SRV" "$BIN" "$SCRIPT" "$WT/tools/sys_meta.py" "$WT/tools/bench_common.py"; do
    [ -r "$f" ] || { echo "missing: $f" >&2; exit 2; }
done
[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 2; }
TASKSET=""
if command -v taskset >/dev/null; then TASKSET="taskset -c $DRIVER_CPUS"; else say "taskset missing; driver unpinned"; fi

mkdir -p "$RUN_ROOT"
FSTYPE=$(df -T "$RUN_ROOT" 2>/dev/null | awk 'NR==2 {print $2}' || echo unknown)
TMPFS_FLAG=()
if [ "$FSTYPE" = tmpfs ]; then
    if [ "$ALLOW_TMPFS" = 1 ]; then
        TMPFS_FLAG=(--allow-tmpfs)
        say "WARNING: $RUN_ROOT is tmpfs; fsync is near-instant there and the cell's margin collapses (recorded)"
    else
        echo "$RUN_ROOT is on tmpfs; the margin this cell measures is the fsync's. Use an ext4 path" \
             "(RUN_ROOT=/var/tmp/cp-s0-run/m6) or ALLOW_TMPFS=1 to record the violation." >&2
        exit 2
    fi
fi

for arm in $ARMS; do
    for p in "$(port_kwp "$arm")" "$(port_text "$arm")"; do
        if [ "$p" = 0 ]; then echo "unknown arm: $arm" >&2; exit 2; fi
        if (echo > "/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
            echo "port $p already accepts connections (bench/README.md rule 5)" >&2; exit 2
        fi
    done
done

# The watchdog's heartbeat: a file, never a process pattern.
( while true; do utc > "$OUT/heartbeat"; sleep 30; done ) &
HB_PID=$!
STOP_WORKDIR=""
cleanup() {
    kill "$HB_PID" 2>/dev/null || true
    if [ -n "$STOP_WORKDIR" ] && [ -f "$STOP_WORKDIR/server.json" ] && [ "$DRY_RUN" != 1 ]; then
        python3 "$SRV" stop --workdir "$STOP_WORKDIR" --timeout "$STOP_TIMEOUT" >/dev/null 2>&1 || true
    fi
    echo "done" > "$OUT/phase"
}
trap cleanup EXIT

# ---- manifest ------------------------------------------------------------------
DESCRIBE=$(git -C "$WT" describe --tags --always 2>/dev/null || echo unknown)
HEAD_SHA=$(git -C "$WT" rev-parse --short HEAD 2>/dev/null || echo unknown)
BIN_SHA=$(sha256sum "$BIN" | awk '{print $1}')
{
    echo "{"
    echo "  \"cell\": \"CP-S0 M6 scripted half\","
    echo "  \"stamp\": \"$STAMP\","
    echo "  \"started_utc\": \"$(utc)\","
    echo "  \"worktree\": \"$WT\","
    echo "  \"git_describe\": \"$DESCRIBE\","
    echo "  \"head\": \"$HEAD_SHA\","
    echo "  \"binary\": \"$BIN\","
    echo "  \"binary_sha256\": \"$BIN_SHA\","
    echo "  \"script\": \"$SCRIPT\","
    echo "  \"launcher\": \"$SRV\","
    echo "  \"run_root\": \"$RUN_ROOT\","
    echo "  \"run_root_fstype\": \"$FSTYPE\","
    echo "  \"arms\": \"$ARMS\","
    echo "  \"iterations\": $ITER,"
    echo "  \"warmup\": $WARMUP,"
    echo "  \"protocol\": \"$PROTOCOL\","
    echo "  \"cores\": $CORES,"
    echo "  \"server_cpus\": \"$SERVER_CPUS\","
    echo "  \"driver_cpus\": \"$DRIVER_CPUS\","
    echo "  \"server_durability_key\": \"group\","
    echo "  \"loadavg_at_start\": \"$(cut -d' ' -f1-3 /proc/loadavg)\","
    echo "  \"builds_running_at_start\": \"$(pgrep -a -f 'cc1plus|cmake --build|ctest' 2>/dev/null | wc -l)\","
    echo "  \"dry_run\": $DRY_RUN"
    echo "}"
} > "$OUT/manifest.json"
say "manifest: $OUT/manifest.json ($DESCRIBE, binary $BIN_SHA)"

# ---- the arms ---------------------------------------------------------------------
for arm in $ARMS; do
    KWP=$(port_kwp "$arm"); TEXT=$(port_text "$arm")
    WORKDIR="$RUN_ROOT/$STAMP/$arm"
    echo "start-$arm" > "$OUT/phase"
    say "arm=$arm: starting server (KWP $KWP, text $TEXT) in $WORKDIR"
    run python3 "$SRV" start --binary "$BIN" --workdir "$WORKDIR" --port "$KWP" \
        --debug-text-port "$TEXT" --cores "$CORES" --taskset "$SERVER_CPUS" \
        --durability group --label "m6-$arm" --timeout "$START_TIMEOUT" "${TMPFS_FLAG[@]}" \
        | tee -a "$OUT/timeline.txt"
    STOP_WORKDIR="$WORKDIR"

    if [ "$PROTOCOL" = text ]; then PORT=$TEXT; else PORT=$KWP; fi
    echo "run-$arm" > "$OUT/phase"
    say "arm=$arm: running $ITER iterations (+$WARMUP warm-up) over $PROTOCOL on port $PORT"
    set +e
    run $TASKSET python3 "$SCRIPT" --host 127.0.0.1 --port "$PORT" --protocol "$PROTOCOL" \
        --arm "$arm" --iterations "$ITER" --warmup "$WARMUP" --json "$OUT/m6-$arm.json" \
        2>&1 | tee "$OUT/m6-$arm.txt"
    RC=${PIPESTATUS[0]}
    set -e
    say "arm=$arm: script exit $RC"

    echo "stop-$arm" > "$OUT/phase"
    # srv.py stop exits 2 when the `stopped;` banner is missing or SIGKILL
    # was needed (srv.py:426); under `set -e -o pipefail` that would end the
    # run before the next arm, so its status is recorded, not fatal.
    set +e
    run python3 "$SRV" stop --workdir "$WORKDIR" --timeout "$STOP_TIMEOUT" | tee -a "$OUT/timeline.txt"
    STOP_RC=${PIPESTATUS[0]}
    set -e
    say "arm=$arm: stop exit $STOP_RC (0 = SIGTERM and clean banner; 2 = forced or no banner, the arm is suspect)"
    STOP_WORKDIR=""
    if [ "$DRY_RUN" != 1 ]; then
        cp "$WORKDIR/server.json" "$OUT/m6-$arm.server.json" 2>/dev/null || true
        cp "$WORKDIR/server.out" "$OUT/m6-$arm.server.out" 2>/dev/null || true
    fi
done

say "finished; outputs in $OUT"
