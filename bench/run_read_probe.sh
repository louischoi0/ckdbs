#!/bin/sh
# R6-R's R1/R2 runner: the three read arms, interleaved, at every swept row
# count, in one sitting.
#
#   bench/run_read_probe.sh <outdir> [reps] [txns] [rowsizes...]
#
# **Interleaved, not batched** - same reason `run_2pc_cost.sh` gives: a
# drift in the host (frequency, another tenant, page cache) landing on one
# arm alone would read as the read path's own cost.
#
# **One process per arm invocation per row count.** `txn_shipped_read_probe.py`
# makes a fresh server and a fresh data file every time it is called.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: run_read_probe.sh <outdir> [reps] [txns] [rowsizes...]}"
REPS="${2:-5}"
TXNS="${3:-200}"
shift 3 2>/dev/null || shift $#
ROWSIZES="$*"
[ -n "$ROWSIZES" ] || ROWSIZES="200 1000 10000"
PROBE="$ROOT/bench/txn_shipped_read_probe.py"
SERVER="${READ_PROBE_SERVER:-$ROOT/build-release/kds_server}"

mkdir -p "$OUT"
PORT=22960

cell() {
    # cell <label> <arm> <rows> <round>
    PORT=$((PORT + 1))
    echo "  $1 round $4"
    python3 "$PROBE" --arm "$2" --rows "$3" --reps 1 --txns "$TXNS" \
        --port "$PORT" --server "$SERVER" \
        --workdir "$OUT/wd" --json "$OUT/$1-r$4.json" > "$OUT/$1-r$4.log" 2>&1 \
        || echo "    FAILED (see $OUT/$1-r$4.log)"
}

for rows in $ROWSIZES; do
    echo "rows=$rows"
    r=1
    while [ "$r" -le "$REPS" ]; do
        echo "  round $r/$REPS"
        cell "r1-autocommit-$rows" autocommit "$rows" "$r"
        cell "r1-rc-$rows"         rc         "$rows" "$r"
        cell "r2-rr-$rows"         rr         "$rows" "$r"
        r=$((r + 1))
    done
done
echo "done; JSON under $OUT"
