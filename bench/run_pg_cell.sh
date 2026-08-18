#!/usr/bin/env bash
# One PostgreSQL cell of the scenario2 comparison - the twin of
# bench/run_s2_cell.sh, so the two sides of an interleaved run are set up the
# same way. Fresh database per cell, not fresh relations: the ckdbs side gets
# a fresh data file, and dropping relations inside one cluster is not the
# same thing, because the cluster keeps the bloat.
#
#   run_pg_cell.sh <label> [-- twin flags...]
#
# Env: PGENV   command wrapper that puts a PostgreSQL server on PATH, for a
#              host where the distro package is not installed (see
#              bench/docs/README.md). Empty when psql is already on PATH.
#      DRV     the tree the twin is run from (default: this repository)
#      ROOT    where output lands (default $HOME/bench-s2-pg)
set -euo pipefail

label=$1; shift
[[ "${1:-}" == "--" ]] && shift

DRV=${DRV:-$(git rev-parse --show-toplevel)}
# Wrapper that puts a PostgreSQL server on PATH. Empty when the distro
# package is installed; see bench/docs/README.md for the rootless case.
PGENV=${PGENV:-}
ROOT=${ROOT:-$HOME/bench-s2-pg}
OUTDIR=${OUTDIR:-$ROOT/out}
# A label like "pg-1" is not a legal unquoted identifier; the hyphen ends the
# statement and CREATE DATABASE fails on the rest.
DB=${DB:-bench_$(printf '%s' "$label" | tr -c 'A-Za-z0-9' '_')}
mkdir -p "$OUTDIR"

"$DRV/bench/wait_quiet.sh"

echo "== $label (postgresql) ==" | tee "$OUTDIR/$label.txt"
{ echo "server: $($PGENV psql -h 127.0.0.1 -p 15433 -d postgres -Atc 'select version()')";
  echo "database: $DB";
  echo "uptime before: $(uptime)"; } | tee -a "$OUTDIR/$label.txt"

$PGENV psql -h 127.0.0.1 -p 15433 -d postgres -qc "DROP DATABASE IF EXISTS $DB" >/dev/null
$PGENV psql -h 127.0.0.1 -p 15433 -d postgres -qc "CREATE DATABASE $DB" >/dev/null

( while true; do cut -d' ' -f1-3 /proc/loadavg; sleep 5; done ) > "$ROOT/$label.load" &
sampler=$!

set +e
$PGENV python3 "$DRV/tools/pg_scenario2_freight.py" --port 15433 --database "$DB" \
    --json "$OUTDIR/$label.json" "$@" >> "$OUTDIR/$label.txt" 2>&1
rc=$?
set -e
kill $sampler 2>/dev/null || true

size=$($PGENV psql -h 127.0.0.1 -p 15433 -d postgres -Atc \
        "select pg_database_size('$DB')")
{ echo "driver exit: $rc";
  echo "database bytes: $size";
  echo "load samples max: $(sort -g "$ROOT/$label.load" | tail -1)";
  echo "uptime after: $(uptime)"; } | tee -a "$OUTDIR/$label.txt"
exit $rc
