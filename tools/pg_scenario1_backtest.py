#!/usr/bin/env python3
"""PostgreSQL baseline for tools/scenario1_backtest.py - same workload, other engine.

The ckdbs tool produces a **queries-per-second matrix**: every read shape the
backtest issues, priced in four cases, on one data set in one process. This
produces the identical matrix against PostgreSQL, with one substitution that
is the whole point of the comparison:

    ckdbs case 3   a **Cabin** on the filter column   CREATE CABIN / DROP CABIN
    PostgreSQL     a **btree index** on that column   CREATE INDEX / DROP INDEX

Both are the engine's own accelerator for a non-primary-key equality. Both
are built at runtime on an already-loaded relation, measured, dropped, and
measured again. That makes the third and fourth columns the same experiment
on both sides, and it is why the interesting number is the **ratio within
each engine** - `cabin/warm` against `index/warm` - rather than the absolute
QPS across them. A ratio is immune to everything the two clients do not
share.

The two tools share `bench_common.py` and, more importantly, this one
**imports the workload itself** from scenario1_backtest.py: the price
generator, the derived features, all eight models, the scoring, the QPS
measurement loop, and the read-shape statements. Nothing is reimplemented,
so nothing can drift. Given the same `--seed`, both engines ingest
byte-identical prices and both backtests make identical decisions - which is
what lets `--verify` mean something across engines and not only within one.

## Fidelity choices, and why each one is not the "natural" PostgreSQL way

A baseline that lets each engine play to its strengths measures two
different workloads and licenses no comparison at all. Every choice below
makes PostgreSQL look *worse* than it would if written idiomatically, and
each is stated here so a quoted number carries it.

1. **The model comparison stays a plain join**, reduced client-side, on
   both sides. `SELECT model_id, sum(pnl_bp) ... GROUP BY model_id` would
   return eight rows instead of thousands and would be enormously faster
   here - and it is now expressible on the ckdbs side too, since
   `docs/spec/parser-v2.md` I14 was resolved by `docs/spec/aggregate.md`. It is
   still not written into that phase, for the original reason: rewriting one
   engine's statement makes the two tools time different questions, and this
   phase's job is to price the join.

   Aggregates are instead measured *as their own phases* (`agg-*`), where
   both sides run the identical statement. That is the comparison the
   rewrite would have destroyed by hiding it inside a phase about something
   else - and it is deliberately the least even table in this tool, because
   PostgreSQL plans a HashAggregate over these and ckdbs makes no plan
   choice at all.

2. **No index on the filter columns by default.** `daily_stats.session_no`
   and `model_results.model_id` are unindexed, which is a seqscan here and a
   `FilterScan` there - the same access path, and the baseline both `cabin`
   and `index` are measured against. The index case adds one at runtime.

3. **`ANALYZE` is run after the load and after every `CREATE INDEX`,
   untimed.** This one is the exception that goes the other way, and it is
   still the fair choice: PostgreSQL's planner is statistics-driven and an
   unanalyzed relation is a misconfigured server, not a slow engine. ckdbs
   collects its own access statistics as it runs. Withholding ANALYZE would
   measure a mistake.

4. **Server-generated ids.** ckdbs invariant 11 forbids a caller-supplied
   primary key, so every relation is `bigint GENERATED ALWAYS AS IDENTITY`
   and INSERT names only the body columns, taking the id back through
   `RETURNING id`. An identity column is a sequence, not an index, which is
   what lets the heap-analogue relations have genuinely zero indexes.

5. **Money is int64 minor units** (`bigint` basis points), never `numeric`.
   ckdbs refuses float/decimal columns at CREATE TABLE; `numeric` would be
   both the correct financial type and a slower one, and would change the
   row size.

6. **Simple query protocol, one statement per round trip, literals inline.**
   No prepared statements and no extended protocol, so PostgreSQL re-parses
   every statement exactly as ckdbs does. `tools/pg_wire.py` says the same.

7. **The client pays the same price on both sides.** `PgClient` decodes
   every returned row and formats it into the one-line, comma-separated
   shape the ckdbs client produces, instead of using pg_wire's fast
   count-only path. That is deliberate: the ckdbs client builds a Python
   string for the whole reply, and letting PostgreSQL skip that would hand
   it a client-side advantage that has nothing to do with either engine.

8. **Cluster settings are PostgreSQL defaults.** `tools/pg_setup.sh` tunes
   nothing, because a baseline tuned by hand is not a baseline. Note the
   two engines' write defaults are therefore not identical - ckdbs defaults
   to `durability = group`, PostgreSQL to `synchronous_commit = on` - which
   is why the write sweep is reported as a *ratio against autocommit* on
   both sides as well as in absolute rows per second.

## Storage organization

Mirrors the ckdbs `clustered_type` choice per relation, which is the part of
that run that is a measurement rather than a workload:

    exchanges, symbols, sessions, daily_bars, models
                              PRIMARY KEY on id  <- ckdbs BTREE: probed by
                              primary key, an index descent on both
    daily_stats, model_results
                              identity, no index <- ckdbs HEAP: appended and
                              then walked, never probed by pk, so no index
                              maintenance per row on either

Usage:
    ./tools/pg_setup.sh init          # once: a scratch cluster on port 15433
    python3 tools/pg_scenario1_backtest.py --json pg.json
    python3 tools/pg_scenario1_backtest.py --years 5 --symbols 4   # quick
    python3 tools/pg_scenario1_backtest.py --qps-ops 500

    # the comparison, end to end:
    python3 tools/scenario1_backtest.py    --port 15599 --json kds.json
    python3 tools/pg_scenario1_backtest.py --port 15433 --json pg.json
    python3 tools/compare_scenario1.py kds.json pg.json

Every run creates its own seven tables suffixed `_<epoch>_<rand>`, matching
the ckdbs side, which needs it because there is no DROP TABLE there. Here
they are dropped at the end unless --keep, since PostgreSQL has one.
"""

import argparse
import random
import sys
import time

from bench_common import Phase, report, write_json
from pg_wire import DEFAULT_HOST, PgConnection, PgError

# The workload itself, imported rather than reimplemented. Everything in
# this list is a thing the two runs must do *identically* or the comparison
# is between two workloads: the same prices, the same features, the same
# eight models making the same decisions, the same statements, and the same
# timing loop around them.
from scenario1_backtest import (
    COUNTRIES, EXCHANGE_CODES, MODELS, SECTORS, SESSIONS_PER_YEAR, Batcher,
    build_shapes, compare_all_sql, compare_one_sql, cross_section_sql,
    derive_stats, generate_series,
    inserted_id, measure_qps, ohlc_from, print_comparison,
    print_concurrency_sweep, print_qps_matrix, print_write_sweep,
    read_back_results, run_backtest, run_per_model_reads, run_replay,
    verify,
)

DEFAULT_PORT = 15433          # tools/pg_setup.sh's scratch cluster
DEFAULT_DATABASE = "bench"

# ---- schema --------------------------------------------------------------
#
# The same seven relations in the same column order, with the closest
# PostgreSQL types. `bigint` for every id and every money-or-basis-point
# value; `text` where ckdbs has `varchar`, because ckdbs has no VARCHAR(n)
# and `text` is the type with no declared limit.
#
# PRIMARY KEY exactly where ckdbs declares BTREE, and nothing at all where
# it declares HEAP. That correspondence is the measurement: an index on
# daily_stats here would be an index ckdbs does not have, and the FilterScan
# columns are left bare on purpose so the `index` case has something to add.

IDENTITY = "bigint GENERATED ALWAYS AS IDENTITY"

SCHEMA = {
    "exchanges": (f"id {IDENTITY} PRIMARY KEY, code text, country text, "
                  f"tz_min int"),
    "symbols": (f"id {IDENTITY} PRIMARY KEY, ticker text, exchange_id bigint, "
                f"sector int, listed_session int"),
    "sessions": (f"id {IDENTITY} PRIMARY KEY, session_no int, year int, "
                 f"month int, dow int"),
    "daily_bars": (f"id {IDENTITY} PRIMARY KEY, symbol_id bigint, "
                   f"session_id bigint, session_no int, open bigint, "
                   f"high bigint, low bigint, close bigint, volume bigint"),
    # No PRIMARY KEY: ckdbs stores this one HEAP, so an index here would be
    # an index the other side does not have.
    "daily_stats": (f"id {IDENTITY}, bar_id bigint, symbol_id bigint, "
                    f"session_no int, ret_bp bigint, mom5_bp bigint, "
                    f"mom20_bp bigint, mom60_bp bigint, mom120_bp bigint, "
                    f"vol10_bp bigint, vol20_bp bigint, vol60_bp bigint"),
    "models": (f"id {IDENTITY} PRIMARY KEY, name text, family int, "
               f"lookback int, top_k int, param_bp bigint"),
    "model_results": (f"id {IDENTITY}, model_id bigint, period_no int, "
                      f"session_no int, pnl_bp bigint, equity_bp bigint, "
                      f"positions int, trades int"),
}

CREATE_ORDER = ("exchanges", "symbols", "sessions", "daily_bars",
                "daily_stats", "models", "model_results")

# The body columns of each relation, in order, for the INSERT column list.
# Explicit because the pk is an identity column and must not be named -
# which is the same rule ckdbs enforces by refusing a full-width value list.
INSERT_COLUMNS = {
    base: ", ".join(part.strip().split(" ", 1)[0]
                    for part in columns.split(",")[1:])
    for base, columns in SCHEMA.items()
}

PG_CONCURRENCY_NOTES = (
    "  PostgreSQL runs a backend process per connection, so a rising",
    "  aggregate here is genuine parallel execution and not only",
    "  round-trip overlap - which is the one line of this comparison",
    "  where the two engines' numbers mean structurally different",
    "  things. Read it against the ckdbs table's flat curve as an",
    "  architectural difference, not a defect in either measurement.",
)

PG_CASE_NOTES = (
    "  cold    every argument seen for the first time: nothing of it is in",
    "          shared_buffers or the OS page cache.",
    "  warm    the same few arguments cycled. warm/cold is what repetition",
    "          alone buys - here, almost entirely the buffer cache.",
    "  index   warm, with a btree index on the filter column, created at",
    "          runtime and ANALYZEd. It is `-` where the filter is the",
    "          primary key, which already has one - the same reason the",
    "          ckdbs side prints `-` there for a Cabin.",
    "  dropped warm again straight after DROP INDEX, which is what makes",
    "          the third column an isolated measurement of the index and",
    "          not of whatever else warmed up while it existed.",
)


def abort(message, detail=None):
    print(f"pg_scenario1 aborted: {message}", file=sys.stderr)
    if detail:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


# ---- the client ----------------------------------------------------------

class PgClient:
    """A PgConnection wearing the ckdbs client's interface.

    `__call__(statement) -> reply string` is the entire contract every
    imported function is written against, and this satisfies it by
    formatting PostgreSQL's replies into the shapes those functions parse:

        SELECT           a header line, then one comma-separated row per
                         match, joined by newlines - what format_reply()
                         produces on the ckdbs side
        INSERT ... RETURNING id
                         `INSERTED id=<n>`, which inserted_id() reads
        anything else    `OK <tag>`
        an error         `ERR <message>`

    Rows are decoded and joined rather than counted through pg_wire's fast
    path. That is fidelity choice 7 in the module docstring: the ckdbs
    client builds a Python string for the whole reply, and a PostgreSQL
    client that skipped it would be measured with an advantage belonging to
    neither engine.

    The header line is synthetic - pg_wire's fetch() does not keep column
    names, and nothing downstream reads them, because select_rows() drops
    the first line. It is emitted so the reply has the shape the parser
    expects, not so it can be read.
    """

    def __init__(self, host, port, user, database, timeout):
        try:
            self._conn = PgConnection(host=host, port=port, user=user,
                                      database=database, timeout=timeout)
        except (OSError, PgError) as e:
            abort(f"could not connect to {host}:{port}/{database}: {e}",
                  "start the scratch cluster with: ./tools/pg_setup.sh init")
        self.errors = 0
        self.first_error = None

    def __call__(self, statement):
        try:
            rows, error = self._conn.fetch(statement)
        except PgError as e:
            error = str(e)
            rows = []
        if error is not None:
            self.errors += 1
            reply = f"ERR {error}"
            if self.first_error is None:
                self.first_error = f"{statement}  ->  {reply}"
            return reply

        head = statement.lstrip()[:6].upper()
        if head.startswith("INSERT"):
            # RETURNING id gives exactly one column of one row.
            if rows and rows[0] and rows[0][0] is not None:
                return f"INSERTED id={rows[0][0].decode()}"
            return "OK INSERT"
        if not rows:
            # A SELECT that matched nothing still has to look like a SELECT
            # reply, or select_rows() would read the tag as a header and
            # report -1 rows. An empty header line is the honest shape.
            return "cols" if head.startswith("SELECT") else "OK"
        return "cols\n" + "\n".join(
            ",".join("" if value is None else value.decode(errors="replace")
                     for value in row)
            for row in rows)

    def timed(self, statement, phase):
        t0 = time.perf_counter()
        reply = self(statement)
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def close(self):
        self._conn.close()


# ---- schema construction -------------------------------------------------

def create_tables(client, suffix):
    for base in CREATE_ORDER:
        reply = client(f"CREATE TABLE {base}_{suffix} ({SCHEMA[base]})")
        if reply.startswith("ERR"):
            abort(f"could not create {base}_{suffix}", reply)


def drop_tables(client, suffix):
    # Reverse creation order so nothing is dropped out from under a
    # dependency, though none is declared here - the ckdbs side's foreign
    # keys have no counterpart in this run, because its --fk measures the
    # cost of the check and adding one here would measure a different one.
    for base in reversed(CREATE_ORDER):
        client(f"DROP TABLE IF EXISTS {base}_{suffix}")
    client(f"DROP TABLE IF EXISTS write_probe_{suffix}")


def analyze_all(client, suffix):
    """Statistics for every relation, untimed. Fidelity choice 3."""
    for base in CREATE_ORDER:
        client(f"ANALYZE {base}_{suffix}")


# ---- load ----------------------------------------------------------------
#
# The same three loaders as the ckdbs side, rewritten only where the wire
# differs: an explicit column list (the pk is an identity column and may not
# be named) and `RETURNING id` in place of the `INSERTED id=` reply. The row
# *values* come from the imported generators, so both engines ingest the
# same bytes from the same seed.

def insert_sql(base, suffix, values):
    return (f"INSERT INTO {base}_{suffix} ({INSERT_COLUMNS[base]}) "
            f"VALUES ({values}) RETURNING id")


def result_insert_sql(suffix, values):
    """The result-row INSERT for run_backtest, in PostgreSQL's form.

    The ckdbs side sends a positional value list with the Keystone pk
    omitted; an identity column here refuses that outright, so the body
    columns are named. No RETURNING: the backtest never reads the id back,
    and asking for one would make this the only statement in the run whose
    reply the other engine does not also produce.
    """
    return (f"INSERT INTO model_results_{suffix} "
            f"({INSERT_COLUMNS['model_results']}) VALUES ({values})")


def load_lookups(client, suffix, symbol_count, exchange_count, rng, phases):
    phase = Phase("load-exchanges", "venues")
    exchange_ids = []
    for i in range(exchange_count):
        code = EXCHANGE_CODES[i % len(EXCHANGE_CODES)] + (
            f"{i // len(EXCHANGE_CODES)}" if i >= len(EXCHANGE_CODES) else "")
        reply = client.timed(insert_sql(
            "exchanges", suffix,
            f"'{code}', '{COUNTRIES[i % len(COUNTRIES)]}', "
            f"{rng.randint(-12, 12) * 60}"), phase)
        got = inserted_id(reply)
        if got:
            exchange_ids.append(got)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    if not exchange_ids:
        abort("the load created no exchanges", client.first_error)

    phase = Phase("load-symbols", "instruments")
    symbols = []
    for i in range(symbol_count):
        ticker = f"SYM{i:04d}"
        exchange_id = exchange_ids[i % len(exchange_ids)]
        reply = client.timed(insert_sql(
            "symbols", suffix,
            f"'{ticker}', {exchange_id}, {i % SECTORS}, 0"), phase)
        got = inserted_id(reply)
        if got:
            symbols.append((got, ticker, exchange_id))
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)
    return symbols


def load_sessions(client, suffix, count, start_year, batch, phases):
    phase = Phase("load-sessions", f"{count:,} business days")
    batcher = Batcher(client, batch)
    ids = []
    for n in range(count):
        year = start_year + n // SESSIONS_PER_YEAR
        month = 1 + (n % SESSIONS_PER_YEAR) * 12 // SESSIONS_PER_YEAR
        batcher.step()
        reply = client.timed(insert_sql(
            "sessions", suffix, f"{n}, {year}, {month}, {n % 5}"), phase)
        batcher.maybe_commit()
        got = inserted_id(reply)
        if got:
            ids.append(got)
    batcher.finish()
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)
    return ids


def load_history(client, suffix, symbols, session_ids, rng, batch, phases):
    bars_phase = Phase("load-bars", "OHLCV, one row per (symbol, session)")
    stats_phase = Phase("load-stats", "derived features, one row per bar")
    batcher = Batcher(client, batch)

    for index, (symbol_id, _ticker, _exchange_id) in enumerate(symbols, 1):
        closes = generate_series(len(session_ids), rng)
        stats = derive_stats(closes)

        bar_ids = []
        for n, session_id in enumerate(session_ids):
            open_, high, low = ohlc_from(closes[n], stats[n]["ret_bp"], rng)
            batcher.step()
            reply = client.timed(insert_sql(
                "daily_bars", suffix,
                f"{symbol_id}, {session_id}, {n}, {open_}, {high}, {low}, "
                f"{closes[n]}, {rng.randint(1_000, 50_000_000)}"), bars_phase)
            batcher.maybe_commit()
            bar_ids.append(inserted_id(reply))

        for n, bar_id in enumerate(bar_ids):
            if bar_id is None:
                continue
            s = stats[n]
            batcher.step()
            client.timed(insert_sql(
                "daily_stats", suffix,
                f"{bar_id}, {symbol_id}, {n}, {s['ret_bp']}, {s['mom5_bp']}, "
                f"{s['mom20_bp']}, {s['mom60_bp']}, {s['mom120_bp']}, "
                f"{s['vol10_bp']}, {s['vol20_bp']}, {s['vol60_bp']}"),
                stats_phase)
            batcher.maybe_commit()

        print(f"  symbol {index}/{len(symbols)}: {len(bar_ids):,} bars",
              flush=True)

    batcher.finish()
    bars_phase.elapsed = sum(bars_phase.latencies)
    stats_phase.elapsed = sum(stats_phase.latencies)
    phases.extend((bars_phase, stats_phase))


def load_models(client, suffix, top_k, phases):
    phase = Phase("load-models", f"{len(MODELS)} strategies")
    rows = []
    for name, family, lookback, param_bp in MODELS:
        reply = client.timed(insert_sql(
            "models", suffix,
            f"'{name}', {family}, {lookback}, {top_k}, {param_bp}"), phase)
        got = inserted_id(reply)
        if got is None:
            abort(f"could not create model {name}", client.first_error)
        rows.append((got, name, family, lookback, param_bp))
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)
    return rows


# ---- the individual read shapes ------------------------------------------
#
# The same statements the ckdbs side times, so its `report()` table and this
# one line up phase for phase. They are written out here rather than
# imported because run_read_phases() on that side is one function that both
# builds and sends; splitting it purely to share it would complicate the
# tool that matters for the benefit of the tool that mirrors it.

def run_read_phases(client, suffix, symbols, session_count, bar_count, ops,
                    rng, phases):
    symbol_ids = [symbol_id for symbol_id, _, _ in symbols]

    phase = Phase("read-bar-lookup", "pk equality on daily_bars")
    for _ in range(ops):
        client.timed(f"SELECT * FROM daily_bars_{suffix} "
                     f"WHERE id = {rng.randint(1, max(1, bar_count))}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("read-bar-range", "pk BETWEEN on daily_bars, 200 wide")
    for _ in range(ops):
        low = rng.randint(1, max(1, bar_count - 200))
        client.timed(f"SELECT * FROM daily_bars_{suffix} "
                     f"WHERE id BETWEEN {low} AND {low + 200}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("read-symbol-history", "seqscan, one symbol's whole run")
    for _ in range(max(1, ops // 20)):
        client.timed(f"SELECT * FROM daily_stats_{suffix} "
                     f"WHERE symbol_id = {rng.choice(symbol_ids)}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("read-day-slice", "seqscan, one session's features")
    for _ in range(ops):
        client.timed(f"SELECT * FROM daily_stats_{suffix} "
                     f"WHERE session_no = {rng.randrange(session_count)}",
                     phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("read-join-point", "bar -> symbol -> exchange, by pk")
    for _ in range(ops):
        client.timed(
            f"SELECT b.close, s.ticker, e.code "
            f"FROM daily_bars_{suffix} AS b "
            f"JOIN symbols_{suffix} AS s ON b.symbol_id = s.id "
            f"JOIN exchanges_{suffix} AS e ON s.exchange_id = e.id "
            f"WHERE b.id = {rng.randint(1, max(1, bar_count))}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("read-join-exists", "semi-join: symbols with a bar")
    for _ in range(max(1, ops // 20)):
        client.timed(
            f"SELECT s.ticker FROM symbols_{suffix} AS s "
            f"WHERE EXISTS (SELECT b.id FROM daily_bars_{suffix} AS b "
            f"WHERE b.symbol_id = s.id)", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)


# ---- the aggregate shapes ------------------------------------------------
#
# The same five statements the ckdbs side times, phase for phase, so
# compare_scenario1.py can put them beside each other - it matches phases by
# name and silently drops any that exists on only one side.
#
# This is the sharpest comparison in the scenario and the least even, which
# is why it is worth running. PostgreSQL plans these: it chooses a
# HashAggregate or a GroupAggregate, may read the grouping column from an
# index, and may parallelise the scan under them. ckdbs makes no plan choice
# at all - it walks the relation and folds outside the executor, which is
# what keeps the compiled chain identical to the same statement without a
# GROUP BY (docs/spec/aggregate.md AG1).
#
# The high-cardinality shape is where that asymmetry was expected to show
# most. **It is where PostgreSQL loses** (bench/results-scenario1-vs-pg.md):
# from 1 group to 7,560 over the same 60,480 rows, this side's p50 grows
# +454% and ckdbs's +46%, so a 2.7x deficit on the global form becomes a
# 1.37x win. Worth knowing before reading the low-cardinality rows as a
# verdict on either aggregate implementation - most of that gap is the scan
# underneath.
#
# The two engines emit groups in different orders - ckdbs in first-seen
# order, PostgreSQL in whatever its aggregate produced - and neither was
# asked to sort. These phases measure latency, so that does not enter.

def run_aggregate_phases(client, suffix, session_count, ops, rng, phases):
    scans = max(1, ops // 20)

    phase = Phase("agg-global", "whole-relation fold, no GROUP BY")
    for _ in range(scans):
        client.timed(f"SELECT COUNT(*), MIN(close), MAX(close), SUM(volume) "
                     f"FROM daily_bars_{suffix}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("agg-by-symbol", "GROUP BY symbol_id - 8 groups")
    for _ in range(scans):
        client.timed(f"SELECT symbol_id, COUNT(*), MIN(close), MAX(close) "
                     f"FROM daily_bars_{suffix} GROUP BY symbol_id", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("agg-by-session", "GROUP BY session_no - one per day")
    for _ in range(scans):
        client.timed(f"SELECT session_no, COUNT(*), SUM(volume) "
                     f"FROM daily_bars_{suffix} GROUP BY session_no", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("agg-day-slice", "one session's cross section, grouped")
    for _ in range(ops):
        client.timed(f"SELECT symbol_id, COUNT(*), SUM(ret_bp) "
                     f"FROM daily_stats_{suffix} "
                     f"WHERE session_no = {rng.randrange(session_count)} "
                     f"GROUP BY symbol_id", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("agg-distinct", "COUNT(DISTINCT symbol_id) over the run")
    for _ in range(scans):
        client.timed(f"SELECT COUNT(DISTINCT symbol_id) "
                     f"FROM daily_bars_{suffix}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)


# ---- the QPS sweep -------------------------------------------------------
#
# Structurally identical to the ckdbs sweep, which is the point: the shapes
# and the cold/warm cases come from build_shapes() and measure_qps() on that
# side, imported, so the statements and the timing loop are literally the
# same code. Only the accelerator differs - a btree index where the other
# has a Cabin.

def sweep_shape(client, suffix, shape, ops, warm_keys, rng, index_name):
    row = {"shape": shape.name, "detail": shape.detail}

    seen = set()
    cold = []
    attempts = 0
    while len(cold) < ops and attempts < ops * 20:
        key = shape.keys(rng)
        attempts += 1
        if key in seen:
            continue
        seen.add(key)
        cold.append(shape.build(key))
    row["cold"] = measure_qps(client, cold)
    row["cold_ops"] = len(cold)

    keys = [shape.keys(rng) for _ in range(max(1, warm_keys))]
    warm = [shape.build(keys[i % len(keys)]) for i in range(ops)]
    row["warm"] = measure_qps(client, warm)
    row["warm_ops"] = len(warm)

    if shape.cabin_on is None:
        row["cabin"] = None
        row["dropped"] = None
        row["cabin_note"] = "pk - already indexed by the primary key"
        return row

    relation, column = shape.cabin_on
    reply = client(f"CREATE INDEX {index_name} ON {relation}_{suffix} "
                   f"({column})")
    if reply.startswith("ERR"):
        row["cabin"] = None
        row["dropped"] = None
        row["cabin_note"] = reply[:60]
        return row

    # Statistics before measuring: an index the planner does not know the
    # selectivity of is an index the planner may decline to use, and that
    # would be a measurement of a misconfigured server. Untimed, and the
    # ckdbs side's counterpart is its one warming pass that observes the
    # Cabin's values.
    client(f"ANALYZE {relation}_{suffix}")
    row["cabin"] = measure_qps(client, warm)

    drop = client(f"DROP INDEX {index_name}")
    if not drop.startswith("ERR"):
        client(f"ANALYZE {relation}_{suffix}")
        row["dropped"] = measure_qps(client, warm)
    else:
        row["dropped"] = None
    row["cabin_note"] = f"{relation}.{column}"
    return row


def run_qps_sweep(client, suffix, symbols, session_count, bar_count,
                  model_ids, ops, warm_keys, rng):
    shapes = build_shapes(suffix, symbols, session_count, bar_count, model_ids)
    rows = []
    for number, shape in enumerate(shapes):
        print(f"  sweeping {shape.name}...", flush=True)
        rows.append(sweep_shape(client, suffix, shape, ops, warm_keys, rng,
                                f"ix_sweep_{suffix}_{number}"))
    return rows


# ---- write QPS -----------------------------------------------------------

def run_write_sweep(client, suffix, batches, ops):
    table = f"write_probe_{suffix}"
    reply = client(f"CREATE TABLE {table} (id {IDENTITY}, a bigint, "
                   f"b bigint, c bigint, d bigint)")
    if reply.startswith("ERR"):
        print(f"  write sweep skipped: {reply}")
        return []

    results = []
    for size in batches:
        batcher = Batcher(client, size)
        started = time.perf_counter()
        errors = 0
        for i in range(ops):
            batcher.step()
            got = client(f"INSERT INTO {table} (a, b, c, d) VALUES "
                         f"({i}, {i * 2}, {i * 3}, {i * 5})")
            batcher.maybe_commit()
            if got.startswith("ERR"):
                errors += 1
        batcher.finish()
        elapsed = time.perf_counter() - started
        results.append({
            "batch": size,
            "ops": ops,
            "qps": ops / elapsed if elapsed > 0 else 0.0,
            "errors": errors,
        })
        print(f"  batch {size if size else 1:>5}: "
              f"{results[-1]['qps']:>10,.0f} rows/s", flush=True)
    return results


# ---- concurrent QPS ------------------------------------------------------

def run_concurrency_sweep(host, port, user, database, timeout, suffix,
                          session_count, rebalance, counts, ops):
    """The same experiment as the ckdbs side, with real parallelism behind
    it: PostgreSQL runs a backend process per connection, so a rising
    aggregate here is genuine concurrent execution and not only round-trip
    overlap. That difference is the reading, not a defect in the comparison.
    """
    import threading

    sessions = list(range(0, session_count, rebalance)) or [0]
    results = []

    for count in counts:
        per_thread = max(1, ops // count)
        errors = [0] * count
        connections = []
        try:
            for _ in range(count):
                connections.append(PgConnection(host=host, port=port,
                                                user=user, database=database,
                                                timeout=timeout))
        except (OSError, PgError) as e:
            print(f"  concurrency {count}: could not open connections: {e}")
            for c in connections:
                c.close()
            break

        def worker(index):
            conn = connections[index]
            for i in range(per_thread):
                session_no = sessions[(index * per_thread + i) % len(sessions)]
                try:
                    rows, error = conn.fetch(
                        cross_section_sql(suffix, session_no))
                except PgError:
                    error = "protocol"
                if error is not None:
                    errors[index] += 1

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(count)]
        started = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        elapsed = time.perf_counter() - started
        for c in connections:
            c.close()

        total = per_thread * count
        results.append({
            "connections": count,
            "ops": total,
            "qps": total / elapsed if elapsed > 0 else 0.0,
            "errors": sum(errors),
        })
        print(f"  {count:>3} connection(s): {results[-1]['qps']:>10,.0f} q/s",
              flush=True)
    return results


# ---- EXPLAIN -------------------------------------------------------------

def print_plans(client, suffix, models, session_count, bar_count):
    """The counterpart of the ckdbs side's ANALYZE table.

    Only the top node and the estimated rows, because the whole point is the
    one distinction the ckdbs table also draws: did this shape descend an
    index, or walk the relation? A full EXPLAIN dump would answer a question
    nobody asked of the other engine.
    """
    print()
    print("plans (EXPLAIN)")
    header = f"{'statement':<22}{'top node':<44}{'est rows':>10}"
    print(header)
    print("-" * len(header))
    statements = [
        ("backtest-read", cross_section_sql(suffix, session_count // 2)),
        ("compare-all", compare_all_sql(suffix)),
        ("compare-one", compare_one_sql(suffix, models[0][0])),
        ("read-bar-lookup", f"SELECT * FROM daily_bars_{suffix} "
                            f"WHERE id = {max(1, bar_count // 2)}"),
        ("read-bar-range", f"SELECT * FROM daily_bars_{suffix} "
                           f"WHERE id BETWEEN 1 AND 200"),
        ("read-day-slice", f"SELECT * FROM daily_stats_{suffix} "
                           f"WHERE session_no = {session_count // 2}"),
    ]
    for name, sql in statements:
        reply = client("EXPLAIN " + sql)
        if reply.startswith("ERR"):
            print(f"{name:<22}{'?':<44}{0:>10}")
            continue
        lines = [line for line in reply.split("\n")[1:] if line.strip()]
        top = lines[0].strip() if lines else "?"
        rows = 0
        if "rows=" in top:
            try:
                rows = int(top.split("rows=")[1].split()[0].rstrip(")"))
            except (ValueError, IndexError):
                rows = 0
        print(f"{name:<22}{top[:43]:<44}{rows:>10,}")
    print()
    print("  a `Seq Scan` where the ckdbs table says `FilterScan` is the")
    print("  same access path on both engines, which is what makes the")
    print("  index and cabin columns comparable.")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"default: {DEFAULT_PORT} (pg_setup.sh's "
                             f"scratch cluster)")
    parser.add_argument("--user", default=None,
                        help="default: the invoking user, which is what "
                             "pg_setup.sh grants trust auth to")
    parser.add_argument("--database", default=DEFAULT_DATABASE,
                        help=f"default: {DEFAULT_DATABASE}")
    parser.add_argument("--suffix", default=None,
                        help="table-name suffix; default <epoch>_<rand>, "
                             "matching the ckdbs side")

    parser.add_argument("--years", type=int, default=30,
                        help=f"years of daily history (default: 30), at "
                             f"{SESSIONS_PER_YEAR} business days each")
    parser.add_argument("--symbols", type=int, default=8,
                        help="instruments (default: 8)")
    parser.add_argument("--exchanges", type=int, default=2,
                        help="venues (default: 2)")
    parser.add_argument("--start-year", type=int, default=1995,
                        help="first year of the calendar (default: 1995)")

    parser.add_argument("--rebalance", type=int, default=21,
                        help="sessions between rebalances (default: 21)")
    parser.add_argument("--top-k", type=int, default=3,
                        help="positions each ranking model holds (default: 3)")

    parser.add_argument("--batch", type=int, default=200,
                        help="rows per BEGIN/COMMIT during the load "
                             "(default: 200); 0 is autocommit per row")
    parser.add_argument("--no-load-txn", dest="batch", action="store_const",
                        const=0, help="the same as --batch 0")

    parser.add_argument("--ops", type=int, default=200,
                        help="operations per individual read phase "
                             "(default: 200)")
    parser.add_argument("--replay", type=int, default=1,
                        help="extra passes of the cross-section join "
                             "(default: 1)")
    parser.add_argument("--compare-rounds", type=int, default=4,
                        help="passes of the per-model comparison read "
                             "(default: 4)")

    parser.add_argument("--sweep", dest="sweep", action="store_true",
                        default=True,
                        help="the QPS matrix: every read shape in each of "
                             "four cases, with a btree index in the third "
                             "(default: on)")
    parser.add_argument("--no-sweep", dest="sweep", action="store_false",
                        help="skip the QPS matrix")
    parser.add_argument("--aggregates", dest="aggregates",
                        action="store_true", default=True,
                        help="time the GROUP BY rollups beside the read "
                             "shapes (default: on), matching the ckdbs "
                             "side phase for phase")
    parser.add_argument("--no-aggregates", dest="aggregates",
                        action="store_false",
                        help="skip the aggregate phases; pass it on both "
                             "sides or the comparison drops them anyway")
    parser.add_argument("--qps-ops", type=int, default=100,
                        help="statements per cell of the QPS matrix "
                             "(default: 100). Keep it equal to the ckdbs "
                             "run's value or the two matrices are not "
                             "measuring the same thing")
    parser.add_argument("--warm-keys", type=int, default=8,
                        help="distinct arguments cycled in the warm, index "
                             "and dropped cases (default: 8)")
    parser.add_argument("--write-sweep", dest="write_sweep",
                        action="store_true", default=True,
                        help="INSERT QPS against transaction batch size "
                             "(default: on)")
    parser.add_argument("--no-write-sweep", dest="write_sweep",
                        action="store_false", help="skip it")
    parser.add_argument("--write-batches", default="1,10,100,1000",
                        help="comma-separated batch sizes (default: "
                             "1,10,100,1000)")
    parser.add_argument("--write-ops", type=int, default=2000,
                        help="rows inserted per batch size (default: 2000)")
    parser.add_argument("--connections", default="1,2,4,8",
                        help="comma-separated connection counts for the "
                             "concurrency sweep (default: 1,2,4,8)")
    parser.add_argument("--conn-ops", type=int, default=200,
                        help="join statements spread across the connections "
                             "at each count (default: 200)")

    parser.add_argument("--show-models", action="store_true",
                        help="print the full eight-model comparison table")
    parser.add_argument("--explain", action="store_true",
                        help="print EXPLAIN's top node for each read shape - "
                             "the counterpart of the ckdbs side's --analyze")
    parser.add_argument("--verify", dest="verify", action="store_true",
                        default=True,
                        help="check every model's P&L read back through the "
                             "comparison join against the driver's running "
                             "total (default: on)")
    parser.add_argument("--no-verify", dest="verify", action="store_false",
                        help="skip the round-trip check")
    parser.add_argument("--keep", action="store_true",
                        help="leave the run's tables behind. They are "
                             "dropped by default, which the ckdbs side "
                             "cannot do - there is no DROP TABLE there")

    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed (default: 1). Must match the ckdbs "
                             "run's seed: the prices are generated from it, "
                             "and two runs on different data are not a "
                             "comparison")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="socket timeout in seconds (default: 300)")
    parser.add_argument("--json", metavar="PATH",
                        help="write results as JSON - the file "
                             "tools/compare_scenario1.py reads")
    args = parser.parse_args()

    if args.years < 1:
        abort("--years must be at least 1")
    if args.symbols < 1:
        abort("--symbols must be at least 1")
    if args.rebalance < 1:
        abort("--rebalance must be at least 1")
    if args.top_k < 1:
        abort("--top-k must be at least 1")
    if args.compare_rounds < 1:
        abort("--compare-rounds must be at least 1")

    def int_list(text, flag):
        values = []
        for part in text.split(","):
            part = part.strip()
            if not part:
                continue
            if not part.isdigit() or int(part) < 1:
                abort(f"{flag}: '{part}' is not a positive integer")
            values.append(int(part))
        return values

    write_batches = int_list(args.write_batches, "--write-batches")
    connection_counts = int_list(args.connections, "--connections")

    session_count = args.years * SESSIONS_PER_YEAR
    bar_total = session_count * args.symbols
    suffix = (args.suffix
              or f"{time.time_ns() // 1_000_000_000}_{random.randrange(1 << 16)}")
    rng = random.Random(args.seed)

    client = PgClient(args.host, args.port, args.user, args.database,
                      args.timeout)

    print(f"loading: {args.years} years x {SESSIONS_PER_YEAR} sessions x "
          f"{args.symbols} symbols = {bar_total:,} bars, "
          f"{bar_total:,} feature rows  (tables suffixed _{suffix})"
          + (f"  [{args.batch}-row transactions]" if args.batch else
             "  [one transaction per row]"),
          flush=True)

    create_tables(client, suffix)

    load_phases = []
    t_load = time.perf_counter()

    symbols = load_lookups(client, suffix, args.symbols, args.exchanges, rng,
                           load_phases)
    if not symbols:
        abort("the load created no symbols", client.first_error)

    session_ids = load_sessions(client, suffix, session_count,
                                args.start_year, args.batch, load_phases)
    if not session_ids:
        abort("the load created no sessions", client.first_error)
    print(f"  sessions {len(session_ids):>10,} rows", flush=True)

    load_history(client, suffix, symbols, session_ids, rng, args.batch,
                 load_phases)
    models = load_models(client, suffix, args.top_k, load_phases)
    print(f"  models   {len(models):>10,} rows   "
          f"(load took {time.perf_counter() - t_load:.1f}s)", flush=True)

    analyze_all(client, suffix)

    # ---- the backtest ----------------------------------------------------
    run_phases = []
    print(f"backtesting: {len(models)} models over "
          f"{len(session_ids) // args.rebalance:,} rebalance periods "
          f"({args.rebalance} sessions apart)", flush=True)
    t_run = time.perf_counter()
    state, periods = run_backtest(client, suffix, models, len(session_ids),
                                  args.rebalance, args.top_k, run_phases,
                                  insert_builder=result_insert_sql)
    print(f"  {periods:,} periods scored in "
          f"{time.perf_counter() - t_run:.1f}s", flush=True)

    run_replay(client, suffix, len(session_ids), args.rebalance, args.replay,
               run_phases)
    by_model = read_back_results(client, suffix, run_phases)
    run_per_model_reads(client, suffix, models, args.compare_rounds,
                        run_phases)

    read_phases = []
    run_read_phases(client, suffix, symbols, len(session_ids), bar_total,
                    args.ops, rng, read_phases)
    if args.aggregates:
        run_aggregate_phases(client, suffix, len(session_ids), args.ops, rng,
                             read_phases)

    # ---- the sweeps ------------------------------------------------------
    model_ids = [model_id for model_id, *_rest in models]
    sweep_rows = []
    if args.sweep:
        print("QPS sweep:", flush=True)
        sweep_rows = run_qps_sweep(client, suffix, symbols, len(session_ids),
                                   bar_total, model_ids, args.qps_ops,
                                   args.warm_keys, rng)

    write_results = []
    if args.write_sweep and write_batches:
        print("write sweep:", flush=True)
        write_results = run_write_sweep(client, suffix, write_batches,
                                        args.write_ops)

    concurrency_results = []
    if connection_counts:
        print("concurrency sweep:", flush=True)
        concurrency_results = run_concurrency_sweep(
            args.host, args.port, args.user, args.database, args.timeout,
            suffix, len(session_ids), args.rebalance, connection_counts,
            args.conn_ops)

    problems = verify(state, by_model) if args.verify else []

    # ---- report ----------------------------------------------------------
    phases = load_phases + run_phases + read_phases
    periods_per_year = SESSIONS_PER_YEAR / args.rebalance
    meta = {
        "engine": "postgresql",
        "scenario": "backtest",
        "columns": sum(len(SCHEMA[base].split(",")) for base in SCHEMA),
        "rows": bar_total * 2 + len(session_ids) + len(symbols) + len(MODELS),
        "host": args.host,
        "port": args.port,
        "table": f"7 relations, suffix _{suffix}",
        "clustered": "btree",
        "years": args.years,
        "symbols": len(symbols),
        "sessions": len(session_ids),
        "bars": bar_total,
        "rebalance": args.rebalance,
        "periods": periods,
        "top_k": args.top_k,
        "replay": args.replay,
        "compare_rounds": args.compare_rounds,
        "batch": args.batch,
        "seed": args.seed,
        "accelerator": "index",
    }

    report(phases, meta, footer=(
        f"ingest: {bar_total:,} bars + {bar_total:,} feature rows, "
        f"{'batched ' + str(args.batch) + ' per transaction' if args.batch else 'one transaction per row'}",
        "backtest-read is the same 3-relation join the ckdbs run issues, "
        "written identically and in the same order",
        "daily_stats and model_results carry no index, matching the ckdbs "
        "run's HEAP relations - which is what the sweep's index column adds",
        "no aggregates: the model comparison is reduced client-side, because "
        "the other engine's grammar has none",
        "every row is decoded and formatted client-side, as the ckdbs client "
        "does, so neither side is handed a client-cost advantage",
    ))

    if sweep_rows:
        print_qps_matrix(sweep_rows, args.qps_ops, args.warm_keys,
                         accel="index", notes=PG_CASE_NOTES)
    print_write_sweep(write_results)
    print_concurrency_sweep(concurrency_results,
                            notes=PG_CONCURRENCY_NOTES)

    if args.explain:
        print_plans(client, suffix, models, len(session_ids), bar_total)

    ranking = print_comparison(by_model, periods_per_year, args.show_models)

    if args.verify:
        print()
        if problems:
            print(f"verify: FAILED - {len(problems)} model(s) disagree "
                  f"between the join and the driver's running total")
            for line in problems:
                print(f"    {line}")
        else:
            print(f"verify: OK - all {len(state)} models' P&L read back "
                  f"through the comparison join matches what was accumulated")

    if client.errors:
        print()
        print(f"  {client.errors} error replies over the whole run; "
              f"first: {client.first_error}")

    if args.json:
        def case_json(cell):
            if not cell:
                return None
            qps, mean_us, p50_us, errors = cell
            return {"qps": round(qps, 1), "mean_us": round(mean_us, 1),
                    "p50_us": round(p50_us, 1), "errors": errors}

        meta["ranking"] = ranking
        meta["verify_problems"] = problems
        meta["qps_sweep"] = [
            {"shape": row["shape"], "detail": row["detail"],
             "cabin_on": row.get("cabin_note"),
             "cold_ops": row["cold_ops"], "warm_ops": row["warm_ops"],
             **{case: case_json(row.get(case))
                for case in ("cold", "warm", "cabin", "dropped")}}
            for row in sweep_rows]
        for entry in write_results + concurrency_results:
            entry["qps"] = round(entry["qps"], 1)
        meta["write_sweep"] = write_results
        meta["concurrency_sweep"] = concurrency_results
        write_json(args.json, meta, phases)

    if not args.keep:
        drop_tables(client, suffix)
        print(f"  dropped the run's tables (--keep to leave them)")

    client.close()
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
