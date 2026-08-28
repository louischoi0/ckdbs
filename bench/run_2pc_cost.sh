#!/bin/sh
# RP8's B1-B3 runner: the arms, interleaved, in one sitting.
#
#   bench/run_2pc_cost.sh <outdir> [reps] [txns]
#
# **Interleaved, not batched.** Running every `local` rep and then every
# `xowner` rep would let a drift in the host - a frequency change, another
# tenant, a page cache that warmed - land entirely on one arm and read as
# the protocol's cost. The order's §7 requires it and M1's finding is why:
# absolute numbers on this host are sitting-bound.
#
# **One process per arm invocation**, so no arm inherits another's page
# cache, trx-id lease state or WAL segment; the probe makes a fresh data
# file each time it is called.
#
# B3's before-arm is `--pre`, a *same-sitting* build of the pre-R6 tag -
# never a subtraction from a stored file (§7's second standing constraint).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: run_2pc_cost.sh <outdir> [reps] [txns]}"
REPS="${2:-5}"
TXNS="${3:-200}"
PROBE="$ROOT/bench/txn_2pc_cost_probe.py"
NOW="$ROOT/build-release/kds_server"
PRE="${PRE_SERVER:-}"

mkdir -p "$OUT"
PORT=22900

cell() {
    # cell <label> <server> <arm> <participants> <round>
    PORT=$((PORT + 1))
    echo "  $1 round $5"
    python3 "$PROBE" --arm "$3" --participants "$4" --reps 1 --txns "$TXNS" \
        --port "$PORT" --server "$2" \
        --workdir "$OUT/wd" --json "$OUT/$1-r$5.json" > "$OUT/$1-r$5.log" 2>&1 \
        || echo "    FAILED (see $OUT/$1-r$5.log)"
}

r=1
while [ "$r" -le "$REPS" ]; do
    echo "round $r/$REPS"
    # B1: one two-owner transaction against the same work as two separate
    # one-owner ones, plus the one-owner unit D7's ~2x is stated against.
    cell b1-local    "$NOW" local  1 "$r"
    cell b1-xowner2  "$NOW" xowner 2 "$r"
    cell b1-split2   "$NOW" split  2 "$r"
    # B2: width. D7 predicts depth 2 and width up to the device's limit,
    # with bench/v2.1.0 §3a's four-stream overlap as the curve it declines on.
    cell b2-xowner1  "$NOW" xowner 1 "$r"
    cell b2-xowner3  "$NOW" xowner 3 "$r"
    cell b2-xowner4  "$NOW" xowner 4 "$r"
    cell b2-xowner5  "$NOW" xowner 5 "$r"
    # B3: the fast path against the pre-R6 tag, both arms this sitting.
    if [ -n "$PRE" ]; then
        cell b3-local-pre "$PRE" local 1 "$r"
    fi
    r=$((r + 1))
done
echo "done; JSON under $OUT"
