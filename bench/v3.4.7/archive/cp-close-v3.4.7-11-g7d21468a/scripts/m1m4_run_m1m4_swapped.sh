#!/usr/bin/env bash
# CP-S0 cells M1 and M4 (and the driver's proof arm), prepared against
# `instructions/v3.2.0/workorder-cp-lock-model-exploration.md` section 5, the CP-S0
# row, at cc54a296. Preparation only: nothing here was run while writing it.
#
#   RUN_ROOT=/var/tmp/cp-s0-run ./run_m1m4.sh        # the whole set
#   CELLS=m1 ./run_m1m4.sh                            # one cell
#   DRY_RUN=1 ./run_m1m4.sh                           # print the commands, start nothing
#
# Driver: tools/lock_contention_benchmark.py (unmodified, bench/README.md
# "The drivers themselves are unmodified tools/ scripts"). What it does, read
# at cc54a296:
#   * speaks KWP/1 on the server's default `port` through
#     ckdbs_cli.ServerConnection (driver:82,119; ckdbs_cli.py:160-230) - NOT
#     the newline debug surface; each statement is one write of four frames
#     and one round trip (kwp.py:379-471);
#   * creates `c3_lock (id int64, v int64) BTREE` after a DROP, then inserts
#     rows 1..--rows (driver:213-221); it never drops a relation it did not
#     create;
#   * sets no durability: no SET DURABILITY, no BEGIN (driver has neither), so
#     every statement is autocommit at the server's configured rung
#     (protocol.md:128 - the server's class is the first rung). Both servers
#     below are started with `durability = relaxed`;
#   * --sessions 1 is M1's shape: update-disjoint is
#     `UPDATE c3_lock SET v = k WHERE id = 1 + (slot % rows)` = row 1 for the
#     one session (driver:231-232), a point UPDATE on a row nobody else
#     touches; update-hot at one session is the same statement on the same
#     row (driver:228-229), a second sample; update-disjoint-again is the
#     repeat and the noise floor (driver:41-45, 251, 363-369);
#   * --ab-port opens the second server as a second label; for every arm and
#     every block the order of the two labels alternates (even blocks A then
#     B, odd blocks B then A; driver:308-324), each block being
#     --ops/--blocks statements per session;
#   * --prove prints the "PROOF RUN" banner (driver:330-334) and sets
#     `prove_run: true` in --json (driver:380); it refuses NOTHING in code -
#     the table, the noise-floor lines and the JSON are all still written.
#     Its docstring's "refuses to write a results summary" is the label's
#     meaning, not a code path;
#   * the report prints per arm and label p50us, p99us, qps, err
#     (driver:347-357), then per label "noise floor (disjoint vs its repeat)
#     p50 = X us; hot - disjoint p50 = Y us" (driver:363-369).
#
# Ops sizing (the per-op cost over KWP from Python is taken at 0.1-0.3 ms):
#   proof : 6 arms x 2 servers x PROVE_OPS(400)   =    4,800 stmts ->   ~1-2 s
#   M1    : 6 arms x 2 servers x M1_OPS(20000)    =  240,000 stmts ->  24-72 s per rep,
#           2 reps + 2 server pairs' start/stop                  ->  under 4 min
#   M4    : 6 arms x 8 sessions x M4_OPS(8000)    =  384,000 stmts, 8 at a time ->
#           ~10-60 s depending on the hot row's serialization    ->  under 3 min
# Every number is an env var; a cell cut for the budget is named in the
# results file.
#
# Cores: every server runs `cores = 4` (M4 needs several cores for AT-S5's
# shape - a write runs where its session is; M1 keeps the same server
# configuration so one binary pair is compared under one arrangement, and at
# `cores = 4` the lock table's latches are armed, which is the family's
# real cost - BP-S3 arms them above one core only). Server pinned to CPUs
# 2-5 (`taskset` + `reactor_cpus = 2,3,4,5`, see srv.py), the driver to 6-7.
# On this box (lscpu, 2026-11-08) SMT siblings are (0,4) (1,5) (2,6) (3,7):
# CPUs 2-5 are four distinct physical cores, but the driver's 6-7 are the
# siblings of reactor CPUs 2 and 3. Stated here because a results file must
# say so; SERVER_CPUS / DRIVER_CPUS / CORES override.
#
# Ports: 15520-15525, chosen (bench/README.md rule 5; never 15432).
#   base   KWP 15520  debug-text 15521
#   close KWP 15522  debug-text 15523
#   M4     KWP 15524  debug-text 15525
#
# Outputs: everything under $OUT = m1m4/out/<stamp>/: the driver's stdout
# (<cell>.txt), its --json, each server's sidecar (<cell>-<label>.server.json),
# the views read after each run (<cell>-<label>.views.json/.txt), a
# manifest.json, timeline.txt, and a heartbeat file touched every 30 s for
# the 60 s watchdog (CLAUDE.md "A running measurement is watched every
# minute"; poll $OUT/heartbeat and $OUT/phase, never a process pattern).
# Data files and WAL go under $RUN_ROOT/<stamp>/<cell>/<label>/ - which must
# be a block device (bench/README.md rule 2). The task's default,
# .../scratchpad/cp-s0/run, is on tmpfs on this box (`df -T /tmp` = tmpfs),
# so the preflight refuses it unless ALLOW_TMPFS=1; /var/tmp and /home are
# ext4 on /dev/nvme0n1p1 here.

set -euo pipefail

WT=${WT:-/home/louis/ckdbs/.claude/worktrees/worktree-cp-lock-model-exploration}
SP=${SP:-/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close}
M1M4=${M1M4:-$SP/m1m4}
BIN_BASE=${BIN_BASE:-$SP/bin/kds_server-base}
BIN_CLOSE=${BIN_CLOSE:-$SP/bin/kds_server-close}
RUN_ROOT=${RUN_ROOT:-$SP/run}
ALLOW_TMPFS=${ALLOW_TMPFS:-0}
DRY_RUN=${DRY_RUN:-0}
STAMP=${STAMP:-$(date -u '+%Y%m%dT%H%M%SZ')}
OUT=${OUT:-$M1M4/out/$STAMP}

SERVER_CPUS=${SERVER_CPUS:-2-5}
DRIVER_CPUS=${DRIVER_CPUS:-6-7}
CORES=${CORES:-4}
BUFFER_POOL_FRAMES=${BUFFER_POOL_FRAMES:-65536}
DURABILITY=${DURABILITY:-relaxed}

BLOCKS=${BLOCKS:-4}
ROWS=${ROWS:-256}
# 8, not 1 (review fix): AO-0 item 23's proof is "against the no-lock-family
# binary the two write arms differ only by what first-updater-wins costs",
# and at one session update-hot and update-disjoint are the same statement
# on row 1, so a one-session proof cannot show a driver measuring itself.
# Eight is the driver's own default shape; the cost is ~38k statements.
PROVE_SESSIONS=${PROVE_SESSIONS:-8}
PROVE_OPS=${PROVE_OPS:-400}
M1_OPS=${M1_OPS:-20000}
M1_REPS=${M1_REPS:-2}
M4_SESSIONS=${M4_SESSIONS:-8}
M4_OPS=${M4_OPS:-8000}
CELLS=${CELLS:-prove,m1,m4}

PORT_BASE=${PORT_BASE:-15520}
PORT_BASE_DBG=${PORT_BASE_DBG:-15521}
PORT_CLOSE=${PORT_CLOSE:-15522}
PORT_CLOSE_DBG=${PORT_CLOSE_DBG:-15523}
PORT_M4=${PORT_M4:-15524}
PORT_M4_DBG=${PORT_M4_DBG:-15525}

DRIVER=$WT/tools/lock_contention_benchmark.py
SRV=$M1M4/srv.py
VIEWS=$M1M4/read_views.py

mkdir -p "$OUT" "$RUN_ROOT"
ACTIVE=$OUT/active_servers
: > "$ACTIVE"

utc() { date -u '+%Y-%m-%d %H:%M:%S UTC'; }
say() { local line; line="$(utc)  $*"; echo "$line"; echo "$line" >> "$OUT/timeline.txt"; }
phase() { echo "$(utc) $*" > "$OUT/phase"; say "phase: $*"; }

run() {
    # Prints every command into the timeline; executes it unless DRY_RUN=1.
    say "+ $*"
    if [[ "$DRY_RUN" == 1 ]]; then return 0; fi
    "$@"
}

# ---- servers ---------------------------------------------------------------

start_server() {
    # start_server <cell> <label> <binary> <kwp port> <debug port>
    local cell=$1 label=$2 bin=$3 port=$4 dbg=$5
    local workdir="$RUN_ROOT/$STAMP/$cell/$label"
    local allow=()
    if [[ "$ALLOW_TMPFS" == 1 ]]; then allow=(--allow-tmpfs); fi
    run python3 "$SRV" start --binary "$bin" --workdir "$workdir" --port "$port" \
        --debug-text-port "$dbg" --cores "$CORES" --taskset "$SERVER_CPUS" \
        --durability "$DURABILITY" --buffer-pool-frames "$BUFFER_POOL_FRAMES" --ash on \
        --label "$cell-$label" "${allow[@]}" \
        | tee -a "$OUT/servers.log"
    echo "$cell $label $workdir" >> "$ACTIVE"
}

stop_server() {
    # stop_server <cell> <label>; copies the sidecar beside the driver output.
    local cell=$1 label=$2
    local workdir="$RUN_ROOT/$STAMP/$cell/$label"
    run python3 "$SRV" stop --workdir "$workdir" | tee -a "$OUT/servers.log" || true
    if [[ -f "$workdir/server.json" ]]; then
        cp "$workdir/server.json" "$OUT/$cell-$label.server.json"
    fi
    grep -v -F -- "$cell $label $workdir" "$ACTIVE" > "$ACTIVE.tmp" || true
    mv "$ACTIVE.tmp" "$ACTIVE"
}

read_views() {
    # read_views <cell> <label> <kwp port>
    local cell=$1 label=$2 port=$3
    run taskset -c "$DRIVER_CPUS" python3 "$VIEWS" --port "$port" --label "$cell-$label" \
        --tools "$WT/tools" --out "$OUT/$cell-$label" || say "views of $cell-$label failed"
}

drive() {
    # drive <name> <driver args...>; stdout+stderr to $OUT/<name>.txt
    # A driver that exits non-zero (a refused setup, a dropped connection) is
    # logged and named in $OUT/failed_cells, not fatal (review fix): the
    # cell's views and sidecars are still read, which is what says why.
    local name=$1; shift
    say "+ taskset -c $DRIVER_CPUS python3 -u $DRIVER $*"
    if [[ "$DRY_RUN" == 1 ]]; then return 0; fi
    if ! taskset -c "$DRIVER_CPUS" python3 -u "$DRIVER" "$@" 2>&1 | tee "$OUT/$name.txt"; then
        say "WARNING: the driver for $name exited non-zero; see $OUT/$name.txt"
        echo "$name" >> "$OUT/failed_cells"
    fi
}

check_pair_cores() {
    # check_pair_cores <driver json> (review fix). A one-session pool lands
    # where SO_REUSEPORT puts it, independently per server, and core 0 is not
    # a peer: it runs the 50 ms tick, the checkpoint cadence and the
    # statistics steward. A pair whose two labels sit on different sides of
    # that line carries a bias the block alternation cannot remove; it is
    # logged here so the results file can say so or the rep be re-run.
    local json=$1
    [[ -f "$json" ]] || return 0
    python3 - "$json" <<'EOF' | while IFS= read -r line; do say "$line"; done
import json, sys
d = json.load(open(sys.argv[1]))
cores = {k: v.get("cores", []) for k, v in d.get("pools", {}).items()}
print("cores accepted on: " + "; ".join(
    f"{k}={','.join(v) if v else 'unknown'}" for k, v in cores.items()))
on_core0 = {k: ("0" in v) for k, v in cores.items() if v}
if len(set(on_core0.values())) > 1:
    print("WARNING: the labels are not all on core 0 or all on peer cores; core 0 "
          "carries the ticks - treat this rep as biased and add a rep (M1_REPS)")
EOF
}

cleanup() {
    local rc=$?
    say "cleanup (exit $rc)"
    if [[ -n "${HB_PID:-}" ]]; then kill "$HB_PID" 2>/dev/null || true; fi
    if [[ -s "$ACTIVE" ]]; then
        while read -r cell label workdir; do
            [[ -z "$cell" ]] && continue
            say "stopping a server left running: $cell $label"
            python3 "$SRV" stop --workdir "$workdir" >> "$OUT/servers.log" 2>&1 || true
            [[ -f "$workdir/server.json" ]] && cp "$workdir/server.json" "$OUT/$cell-$label.server.json"
        done < "$ACTIVE"
    fi
    echo "$(utc) done (exit $rc)" > "$OUT/phase"
}
trap cleanup EXIT
# A SIGINT/SIGTERM to the script ends it through the EXIT trap above, so the
# servers it started are stopped rather than left running (review fix).
trap 'exit 130' INT TERM

# ---- preflight -------------------------------------------------------------

phase "preflight"
say "WT=$WT  OUT=$OUT  RUN_ROOT=$RUN_ROOT  DRY_RUN=$DRY_RUN  CELLS=$CELLS"

if [[ "$DRY_RUN" != 1 ]]; then
    for b in "$BIN_BASE" "$BIN_CLOSE"; do
        [[ -x "$b" ]] || { say "missing binary: $b"; exit 2; }
    done
fi
[[ -f "$DRIVER" ]] || { say "missing driver: $DRIVER"; exit 2; }
python3 -c "import sys; sys.path.insert(0, '$WT/tools'); import ckdbs_cli, kwp, sys_meta, bench_common" \
    || { say "the driver's imports fail from $WT/tools"; exit 2; }

FSTYPE=$(df --output=fstype "$RUN_ROOT" | tail -1 | tr -d ' ')
say "RUN_ROOT filesystem: $FSTYPE ($(df -T "$RUN_ROOT" | tail -1))"
if [[ "$FSTYPE" == tmpfs && "$ALLOW_TMPFS" != 1 ]]; then
    say "RUN_ROOT=$RUN_ROOT is tmpfs; bench/README.md rule 2 wants a block device for the data file."
    say "Re-run with RUN_ROOT=/var/tmp/cp-s0-run (ext4 on this box) or ALLOW_TMPFS=1 to record the violation."
    [[ "$DRY_RUN" == 1 ]] || exit 2
fi

NPROC=$(nproc)
if (( NPROC < 8 )); then say "warning: only $NPROC CPUs; SERVER_CPUS=$SERVER_CPUS DRIVER_CPUS=$DRIVER_CPUS may not exist"; fi

for p in "$PORT_BASE" "$PORT_BASE_DBG" "$PORT_CLOSE" "$PORT_CLOSE_DBG" "$PORT_M4" "$PORT_M4_DBG"; do
    if python3 - "$p" <<'EOF'
import socket, sys
p = int(sys.argv[1])
try:
    socket.create_connection(("127.0.0.1", p), timeout=0.3).close()
    sys.exit(0)   # something answers: the port is taken
except OSError:
    sys.exit(1)
EOF
    then say "port $p already accepts connections; choose another (bench/README.md rule 5)"; exit 2; fi
done

{
    echo "{"
    echo "  \"stamp\": \"$STAMP\","
    echo "  \"written_at_utc\": \"$(utc)\","
    echo "  \"worktree\": \"$WT\","
    echo "  \"git_describe\": \"$(git -C "$WT" describe --tags --always --dirty 2>/dev/null)\","
    echo "  \"git_head\": \"$(git -C "$WT" rev-parse HEAD 2>/dev/null)\","
    echo "  \"binary_base\": \"$BIN_BASE\","
    echo "  \"binary_base_sha256\": \"$( [[ -f "$BIN_BASE" ]] && sha256sum "$BIN_BASE" | cut -d' ' -f1 )\","
    echo "  \"binary_close\": \"$BIN_CLOSE\","
    echo "  \"binary_close_sha256\": \"$( [[ -f "$BIN_CLOSE" ]] && sha256sum "$BIN_CLOSE" | cut -d' ' -f1 )\","
    echo "  \"run_root\": \"$RUN_ROOT\","
    echo "  \"run_root_df_T\": \"$(df -T "$RUN_ROOT" | tail -1 | tr -s ' ')\","
    echo "  \"nproc\": $NPROC,"
    echo "  \"smt_siblings\": \"$(for c in $(seq 0 $((NPROC - 1))); do printf 'cpu%s:%s ' "$c" "$(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null)"; done)\","
    echo "  \"server_cpus\": \"$SERVER_CPUS\", \"driver_cpus\": \"$DRIVER_CPUS\", \"cores\": $CORES,"
    echo "  \"durability\": \"$DURABILITY\", \"buffer_pool_frames\": $BUFFER_POOL_FRAMES,"
    echo "  \"blocks\": $BLOCKS, \"rows\": $ROWS,"
    echo "  \"prove\": {\"sessions\": $PROVE_SESSIONS, \"ops\": $PROVE_OPS},"
    echo "  \"m1\": {\"sessions\": 1, \"ops\": $M1_OPS, \"reps\": $M1_REPS},"
    echo "  \"m4\": {\"sessions\": $M4_SESSIONS, \"ops\": $M4_OPS},"
    echo "  \"ports\": {\"base\": $PORT_BASE, \"base_debug\": $PORT_BASE_DBG, \"close\": $PORT_CLOSE, \"close_debug\": $PORT_CLOSE_DBG, \"m4\": $PORT_M4, \"m4_debug\": $PORT_M4_DBG},"
    echo "  \"loadavg_at_start\": \"$(cat /proc/loadavg)\","
    echo "  \"competing_at_start\": \"$(pgrep -a -f 'cc1plus|cmake --build|ctest' | tr '\n' ';' | sed 's/"/\\"/g')\""
    echo "}"
} > "$OUT/manifest.json"
say "manifest: $OUT/manifest.json"

# The watchdog's heartbeat: a file, never a process pattern. It carries the
# current phase (review fix): the script itself is alive while a driver
# hangs, so a watcher reads a phase that has not moved, not a tick alone.
( while true; do echo "$(utc) $(cat "$OUT/phase" 2>/dev/null)" > "$OUT/heartbeat"; sleep 30; done ) &
HB_PID=$!

has_cell() { [[ ",$CELLS," == *",$1,"* ]]; }

# ---- (1) the proof arm -------------------------------------------------------
# AO-0 item 23: against the no-lock-family binary the two write arms may
# differ only by what first-updater-wins costs. At PROVE_SESSIONS=8 (the
# default since the review) the hot arm is contended and the proof is of
# that; PROVE_SESSIONS=1 would make hot and disjoint the same statement on
# row 1 and the arm a check of the rig alone (two servers, the alternation).
# On the close binary the hot arm's refusals (`TxnConflict`, counted under
# `err`) are first-updater-wins itself, expected and part of the proof.

if has_cell prove; then
    phase "prove: start servers"
    start_server prove base "$BIN_BASE" "$PORT_BASE" "$PORT_BASE_DBG"
    start_server prove close "$BIN_CLOSE" "$PORT_CLOSE" "$PORT_CLOSE_DBG"
    phase "prove: driver"
    drive prove --port "$PORT_CLOSE" --label close --ab-port "$PORT_BASE" --ab-label base \
        --sessions "$PROVE_SESSIONS" --rows "$ROWS" --ops "$PROVE_OPS" --blocks "$BLOCKS" \
        --prove --json "$OUT/prove.json"
    phase "prove: views"
    read_views prove base "$PORT_BASE"
    read_views prove close "$PORT_CLOSE"
    phase "prove: stop servers"
    stop_server prove base
    stop_server prove close
fi

# ---- (2) M1: one session, point UPDATE, base vs close interleaved ---------------

if has_cell m1; then
    for rep in $(seq 1 "$M1_REPS"); do
        cell="m1-rep$rep"
        phase "$cell: start servers"
        start_server "$cell" base "$BIN_BASE" "$PORT_BASE" "$PORT_BASE_DBG"
        start_server "$cell" close "$BIN_CLOSE" "$PORT_CLOSE" "$PORT_CLOSE_DBG"
        phase "$cell: driver"
        drive "$cell" --port "$PORT_CLOSE" --label close --ab-port "$PORT_BASE" --ab-label base \
            --sessions 1 --rows "$ROWS" --ops "$M1_OPS" --blocks "$BLOCKS" \
            --json "$OUT/$cell.json"
        check_pair_cores "$OUT/$cell.json"
        phase "$cell: views"
        read_views "$cell" base "$PORT_BASE"
        read_views "$cell" close "$PORT_CLOSE"
        phase "$cell: stop servers"
        stop_server "$cell" base
        stop_server "$cell" close
    done
fi

# ---- (3) M4: base only, 8 sessions, contended vs disjoint ------------------------

if has_cell m4; then
    phase "m4: start servers"
    start_server m4 base "$BIN_BASE" "$PORT_M4" "$PORT_M4_DBG"
    start_server m4 close "$BIN_CLOSE" "$PORT_CLOSE" "$PORT_CLOSE_DBG"
    phase "m4: driver"
    drive m4 --port "$PORT_M4" --label base --ab-port "$PORT_CLOSE" --ab-label close --sessions "$M4_SESSIONS" --rows "$ROWS" \
        --ops "$M4_OPS" --blocks "$BLOCKS" --json "$OUT/m4.json"
    phase "m4: views"
    read_views m4 base "$PORT_M4"
    read_views m4 close "$PORT_CLOSE"
    phase "m4: stop servers"
    stop_server m4 base
    stop_server m4 close
fi

phase "done"
say "outputs under $OUT:"
ls -1 "$OUT" | sed 's/^/  /' | tee -a "$OUT/timeline.txt"
