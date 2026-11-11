#!/bin/bash
# CP-S0 cell M2 - candidate C1's price: a SELECT walk over ~100-1,000 leaf
# pages, the baseline kds_server against a build whose ReadBorrow::Position
# declines slices (patch-no-slice.diff), interleaved A/B, the server's on-CPU
# time per statement from /proc/<pid>/task/*/schedstat, reduced to ns per
# scanned leaf page by m2_summary.py.
#
# The driver is tools/bm_scan.py, UNMODIFIED (bench/README.md: a driver
# change inside a measurement stage measures the driver), in its `census`
# mode with two `--arm`s: a fresh server per arm and run, cores = 1,
# durability = relaxed, buffer_pool_frames = 65536 (tools/bn_census.py:143-148
# at cc54a296 writes `port = P`, the KWP/1 default port - expeditor.cpp:228;
# `kwp_port`, the v0 load endpoint at expeditor.cpp:679, is not needed and not
# written), both servers pinned to one CPU, the driver to another, the same
# statements on both servers per block with the server order rotated per
# block and per run (bn_census.rotated), and SHOW META read around every
# block.
#
# Cells (tools/bm_scan.py:159-190 at cc54a296), chosen for their leaf-page
# count - ANALYZE's `pages=` at BM's close, bench/v3.0.0/archive/
# bm-close-v2.7.0-946-g6b771ec1/census.json.gz, which the layout confirms
# (page body 8192 - 32 header - 16 heap header - 4 next-page id = 8140 bytes;
# a row is 20 header + payload + 5 slot: heap_page.hpp:75,111,160,174):
#   rows-100k  s100k (id, v, w int64), 100,000 rows: SELECT * FROM s100k WHERE w = ?
#              payload 24 -> 49 B/row -> 166 rows per leaf -> 603 leaf pages
#   wide-a     wd (13 columns, one spilled varchar), 10,000 rows: SELECT * FROM wd WHERE a = ?
#              payload 218 -> 243 B/row -> 33 rows per leaf -> 304 leaf pages
# Both inside the order's 100-1,000; `bn` (10,000 narrow rows) is 61 pages and
# is left out. Each cell's point arm (SELECT * FROM <rel> WHERE id = ?) is the
# control: the point statement declares the whole id space only, which both
# builds answer identically (read_borrow.hpp:116-118), so its delta is the
# noise floor of the run.
#
# Budget (<= 5 min): per run ~22 s (two server starts, s100k + wd + the
# 2,200-commit flood per arm ~9 s, wait_idle 2 x 2 s, idle read 2 x 1 s,
# point:s100k 12,000 x ~57 us, rows-100k 768 x ~3.3 ms, point:wd 12,000 x
# ~78 us, wide-a 3,072 x ~0.45 ms, stops); 6 runs ~2.5 min, plus a ~15 s
# pre-flight sanity. A hard `timeout` guards the census; bm_scan writes its
# JSON after every run, so a cut run still leaves the completed ones.
#
# Every knob is an environment variable; the defaults are the planned cell.
set -uo pipefail

SCRATCH=/tmp/claude-1000/-home-louis-ckdbs/5bdaae98-e052-4814-8a2c-eb67c5cf878d/scratchpad/cp-close
TREE=/home/louis/ckdbs/.claude/worktrees/worktree-cp-lock-model-exploration
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE="${M2_BASE:-$SCRATCH/bin/kds_server-base}"
NOSLICE="${M2_NOSLICE:-$SCRATCH/bin/kds_server-close}"
OUT="${M2_OUT:-$SCRATCH/run/m2}"
# The data file goes on a block device, never tmpfs (bench/README.md rule 2).
# On this box the scratchpad (/tmp) is tmpfs and / is ext4 on /dev/nvme0n1p1,
# so the servers' workdir defaults to /var/tmp; the outputs stay in OUT.
WORK="${M2_WORKDIR:-/var/tmp/cp-s0-m2/work}"
PORT="${M2_PORT:-32611}"          # arms take PORT, PORT+1[, PORT+2] (bn_census.parse_arms); the sanity PORT+10
# M2_REPLICATE=1 adds a third arm, `base2`, the SAME baseline binary under a
# second label: the A-A' floor the BM method read its deltas against. At BM's
# close two byte-identical arms differed by 4.3 % [2.9, 15.8] on rows-100k
# (m2_summary.py on bench/v3.0.0/archive/bm-close-.../census.json.gz), so
# without it the point control and the block spread are the only references.
# It costs ~50 % more per run; RUNS then defaults to 5.
REPLICATE="${M2_REPLICATE:-0}"
if [ "$REPLICATE" = 1 ]; then RUNS="${M2_RUNS:-5}"; else RUNS="${M2_RUNS:-6}"; fi
BLOCKS="${M2_BLOCKS:-12}"
SCAN_OPS="${M2_SCAN_OPS:-768}"    # rows-100k runs SCAN_OPS/4 = 192 scans per path per run (16 per block); wide-a 768 (64 per block)
POINT_OPS="${M2_POINT_OPS:-3000}" # bm_scan's default
CELLS="${M2_CELLS:-rows-100k,wide-a}"
PIN_SERVER="${M2_PIN_SERVER:-2}"  # SMT siblings on this box: (0,4) (1,5) (2,6) (3,7); 6 and 7 stay idle
PIN_DRIVER="${M2_PIN_DRIVER:-3}"
QUIET_LOAD="${M2_QUIET_LOAD:-2}"  # bm_scan's per-run load gate, bounded by QUIET_WAIT
QUIET_WAIT="${M2_QUIET_WAIT:-60}"
HOST_WAIT="${M2_HOST_WAIT:-600}"  # wait this long for a compiler/test-free host before starting
CENSUS_TIMEOUT="${M2_CENSUS_TIMEOUT:-270}"
SANITY_TIMEOUT="${M2_SANITY_TIMEOUT:-150}"  # two arms in turn, each: start, load, wait_idle (<= 30 s), 40 scans, stop (<= 30 s)
SANITY_SCANS="${M2_SANITY_SCANS:-40}"

export PYTHONDONTWRITEBYTECODE=1   # no __pycache__ written into the tree's tools/

die() { echo "run_m2: $*" >&2; exit 2; }

mkdir -p "$OUT" || die "cannot create $OUT"
mkdir -p "$WORK" || die "cannot create $WORK"
RUNLOG="$OUT/run.txt"
{
  echo "== CP-S0 M2 run"
  echo "started_utc $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo "tree $TREE"
  echo "describe $(git -C "$TREE" describe --tags 2>/dev/null) head $(git -C "$TREE" rev-parse --short HEAD 2>/dev/null)"
  echo "driver $TREE/tools/bm_scan.py (unmodified)"
  echo "base $BASE"; echo "noslice $NOSLICE"
  echo "workdir $WORK"; echo "out $OUT"
  echo "port $PORT (arms from $PORT; sanity $((PORT+10)))"
  echo "replicate $REPLICATE"
  echo "runs $RUNS blocks $BLOCKS scan_ops $SCAN_OPS point_ops $POINT_OPS cells $CELLS"
  echo "pin_server $PIN_SERVER pin_driver $PIN_DRIVER quiet_load $QUIET_LOAD quiet_wait $QUIET_WAIT"
} > "$RUNLOG"

# ---- preconditions ---------------------------------------------------------
for b in "$BASE" "$NOSLICE"; do
  [ -x "$b" ] || die "binary missing or not executable: $b"
done
for t in python3 taskset ss sha256sum; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
[ -f "$TREE/tools/bm_scan.py" ] || die "driver missing: $TREE/tools/bm_scan.py"
# The driver must be the committed one (bench/README.md: unmodified tools/).
if ! git -C "$TREE" diff --quiet -- tools/bm_scan.py tools/bn_census.py tools/kwp.py tools/sys_meta.py tools/bb_overhead_benchmark.py; then
  echo "WARNING: a driver file is modified in the worktree; the run measures a changed driver" | tee -a "$RUNLOG" >&2
  [ "${M2_ALLOW_DIRTY_DRIVER:-0}" = 1 ] || die "refusing: set M2_ALLOW_DIRTY_DRIVER=1 to run anyway"
fi

# Rule 2: the data file's device, named in the record; tmpfs refused.
WORK_FS=$(df -T "$WORK" | awk 'NR==2{print $1" "$2}')
echo "workdir_fs $WORK_FS" >> "$RUNLOG"
case "$WORK_FS" in
  *tmpfs*) [ "${M2_ALLOW_TMPFS:-0}" = 1 ] || die "workdir $WORK is on tmpfs ($WORK_FS); set M2_WORKDIR to a block device or M2_ALLOW_TMPFS=1" ;;
esac

# The arms, in --arm order (bn_census.parse_arms gives arm i the port PORT+i).
ARMS=(--arm "base=$BASE" --arm "close=$NOSLICE")
NARMS=2
if [ "$REPLICATE" = 1 ]; then ARMS+=(--arm "base2=$BASE"); NARMS=3; fi

# Rule 5: the ports are chosen and must be free.
for p in $(seq "$PORT" $((PORT+NARMS-1))) $((PORT+10)); do
  if ss -ltn 2>/dev/null | awk '{print $4}' | grep -q ":$p\$"; then die "port $p is in use"; fi
done

# Rule 3: the binaries are measured from copies; hash what will be started.
{
  echo "== binaries (sha256, size, mtime)"
  for b in "$BASE" "$NOSLICE"; do
    echo "$(sha256sum "$b" | cut -d' ' -f1)  $(stat -c '%s %y' "$b")  $b"
  done
  echo "== no-slice patch"
  [ -f "$SCRATCH/patch-no-slice.diff" ] && sha256sum "$SCRATCH/patch-no-slice.diff"
  echo "== drivers"
  sha256sum "$TREE/tools/bm_scan.py" "$TREE/tools/bn_census.py" "$TREE/tools/kwp.py" "$TREE/tools/sys_meta.py" "$HERE/m2_sanity.py" "$HERE/m2_summary.py"
} > "$OUT/binaries.txt"

# Rule 4: the host, before anything starts.
{
  echo "== date"; date -u '+%Y-%m-%d %H:%M:%S UTC'
  echo "== uname"; uname -a
  echo "== cpus"; nproc; grep -m1 'model name' /proc/cpuinfo
  echo "== siblings"; for c in /sys/devices/system/cpu/cpu[0-9]*; do echo "$(basename "$c"): $(cat "$c/topology/thread_siblings_list")"; done
  echo "== mem"; free -m
  echo "== loadavg"; cat /proc/loadavg
  echo "== df -T workdir"; df -T "$WORK"
  echo "== df -T out"; df -T "$OUT"
  echo "== competing (pgrep cc1plus|cmake --build|ctest|kds_tests)"; pgrep -a -f 'cc1plus|cmake --build|ctest|kds_tests' | grep -v run_m2 || echo none
  echo "== listeners"; ss -ltn
} > "$OUT/host.txt"

# Wait for a host with no build or test suite running (bounded).
t0=$(date +%s)
while pgrep -f 'cc1plus|cmake --build|ctest|kds_tests' | grep -qv "^$$\$"; do
  if [ $(( $(date +%s) - t0 )) -ge "$HOST_WAIT" ]; then
    echo "host still busy after ${HOST_WAIT}s; running anyway (recorded per cell by the driver)" | tee -a "$RUNLOG" >&2
    break
  fi
  sleep 5
done
echo "host_wait_s $(( $(date +%s) - t0 ))" >> "$RUNLOG"

# A 2 s host sampler for the run's length (bench/README.md rule 4).
STOP="$OUT/.sampler-stop"; rm -f "$STOP"
if [ -x "$SCRATCH/host_sampler.sh" ]; then
  "$SCRATCH/host_sampler.sh" "$OUT/host.log" "$STOP" &
  SAMPLER=$!
else
  SAMPLER=""
fi
finish_sampler() { touch "$STOP"; [ -n "$SAMPLER" ] && wait "$SAMPLER" 2>/dev/null; }
trap finish_sampler EXIT

# ---- 1. pre-flight sanity (kFenceWalks-free, same rows, same pages) ---------
# No sys.meta key counts a granted TryAcquire (contention.hpp's Tally has no
# lock-ask count; fence_walks counts FenceCoversKey walks a slice IS never
# triggers, lock_table.cpp:340-356; lock_partition waits are the contended
# branch only and the latches are null at cores = 1, lock_table.hpp:511-512;
# sys.lock_waits holds parked waits only; app.lock_conflict counts refusals).
# The sanity therefore reads them all around a burst of the rows-100k scan on
# each binary and expects zero deltas on both, and checks that both answer
# the same rows and the same `pages=`. A difference in rows or pages is a
# different workload and stops the run.
echo "sanity_start $(date -u '+%H:%M:%S')" >> "$RUNLOG"
timeout -k 20 "$SANITY_TIMEOUT" python3 "$HERE/m2_sanity.py" \
    --base "$BASE" --noslice "$NOSLICE" --workdir "$WORK/sanity" --port $((PORT+10)) \
    --pin-server "$PIN_SERVER" --pin-driver "$PIN_DRIVER" --scans "$SANITY_SCANS" \
    --out "$OUT/sanity.json" 2>&1 | tee "$OUT/sanity.log"
SANITY_RC=${PIPESTATUS[0]}
echo "sanity_rc $SANITY_RC" >> "$RUNLOG"
if [ "$SANITY_RC" -eq 2 ]; then
  die "sanity: the two builds do not answer the same workload (see $OUT/sanity.log)"
elif [ "$SANITY_RC" -ne 0 ]; then
  echo "sanity exited $SANITY_RC (timeout or server failure); continuing with the census, recorded" | tee -a "$RUNLOG" >&2
fi

# ---- 2. the census: tools/bm_scan.py, unmodified ---------------------------
echo "census_start $(date -u '+%H:%M:%S')" >> "$RUNLOG"
CENSUS_CMD=(python3 "$TREE/tools/bm_scan.py" census
    "${ARMS[@]}"
    --workdir "$WORK" --port "$PORT"
    --pin-server "$PIN_SERVER" --pin-driver "$PIN_DRIVER"
    --runs "$RUNS" --blocks "$BLOCKS" --scan-ops "$SCAN_OPS" --point-ops "$POINT_OPS"
    --cells "$CELLS"
    --quiet-load "$QUIET_LOAD" --quiet-wait "$QUIET_WAIT"
    --out "$OUT/census.json")
printf 'census_cmd'; printf ' %q' "${CENSUS_CMD[@]}"; printf '\n'
{ printf 'census_cmd'; printf ' %q' "${CENSUS_CMD[@]}"; printf '\n'; } >> "$RUNLOG"
timeout --signal=INT -k 30 "$CENSUS_TIMEOUT" "${CENSUS_CMD[@]}" 2>&1 | tee "$OUT/census.log"
CENSUS_RC=${PIPESTATUS[0]}
echo "census_rc $CENSUS_RC" >> "$RUNLOG"
echo "census_end $(date -u '+%H:%M:%S')" >> "$RUNLOG"
if [ "$CENSUS_RC" -eq 124 ]; then
  echo "census cut by the ${CENSUS_TIMEOUT}s timeout; the runs written so far are in $OUT/census.json" | tee -a "$RUNLOG" >&2
fi

# Leftover servers (a cut run's) are stopped: those started from our two
# binaries only (`taskset` execs the binary, so its cmdline is `<bin> --config`).
pkill -TERM -f "^$BASE --config " 2>/dev/null || true
pkill -TERM -f "^$NOSLICE --config " 2>/dev/null || true

# ---- 3. the record: server files kept, data files dropped ------------------
mkdir -p "$OUT/server-files"
find "$WORK" -maxdepth 2 \( -name '*.conf' -o -name '*.stderr' -o -name '*.log' -o -name '*.json' \) \
     -exec cp -p {} "$OUT/server-files/" \; 2>/dev/null
if [ "${M2_KEEP_DATA:-0}" != 1 ]; then
  find "$WORK" -maxdepth 2 -name '*.db' -exec rm -f {} \; 2>/dev/null
  find "$WORK" -maxdepth 2 -type d -name '*.db.wal' -exec rm -rf {} + 2>/dev/null
fi

# ---- 4. the summary: ns per leaf page, the point control, the spread --------
if [ -s "$OUT/census.json" ]; then
  REPL_FLAG=()
  [ "$REPLICATE" = 1 ] && REPL_FLAG=(--replicate base2)
  python3 "$HERE/m2_summary.py" "$OUT/census.json" --base base --noslice close \
      "${REPL_FLAG[@]}" --cells "$CELLS" --out "$OUT/summary.md" 2>&1 | tee "$OUT/summary.log"
  echo "summary_rc ${PIPESTATUS[0]}" >> "$RUNLOG"
  echo; echo "---- $OUT/summary.md"; cat "$OUT/summary.md"
else
  echo "no census.json written; nothing to summarize" | tee -a "$RUNLOG" >&2
fi
cat /proc/loadavg >> "$RUNLOG"
echo "ended_utc $(date -u '+%Y-%m-%d %H:%M:%S UTC')" >> "$RUNLOG"
echo "run record: $RUNLOG"
[ "$CENSUS_RC" -eq 0 ]
