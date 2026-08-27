#!/usr/bin/env python3
"""QPS across read shapes and cases, measured on 30 years of daily bars.

**What this tool produces is a queries-per-second matrix**: every read shape
this workload issues, priced in each of four cases - cold, warm, with a
Cabin, and after dropping it - on one data set, in one process. The write
side gets the same treatment against transaction batch size, and the join
gets one more sweep against connection count.

The backtest above it is the *vehicle*, not the deliverable. It exists
because a QPS number is only worth reading if the data underneath it has a
shape worth querying: 30 years of history is what makes a full-relation
FilterScan expensive, eight models rebalancing against it is what fills a
result relation worth joining, and a walk forward is what leaves both of
them warm in a way a synthetic loop does not. The model ranking is printed
in one line unless --show-models asks for the table.

Where tools/scenario0_stockmarket.py measures a **write** workload - trades
and balance updates under a concurrent reporter - this one measures the
other half of a financial system, and the half that is almost all reads:

    1. a bulk ingest of historical market data (the INSERT half)
    2. a walk-forward backtest that reads it back through 3-relation joins
    3. eight models scored against each other from a join of their own
       results relation (the SELECT/JOIN half)
    4. the QPS sweeps over all of it

The relations, seven of them - **five carrying the price history** and two
carrying the backtest:

    exchanges       venues; one row per market
    symbols         instruments, each on one exchange
    sessions        the trading calendar: one row per business day, 30 years
    daily_bars      OHLCV, one row per (symbol, session) - the bulk relation
    daily_stats     the derived feature row for each bar: returns, momentum
                    at four lookbacks, volatility at three
    ---
    models          the eight strategies, declared once
    model_results   one row per (model, rebalance period), written by the
                    backtest as it walks forward

`daily_stats` exists as its own relation rather than as more columns on
`daily_bars` for the reason this tool exists: it makes the backtest's read a
real **join chain** - walk the feature rows for one session, probe the bar
each one describes, probe that bar's symbol - instead of a single-relation
scan. It is also how a feature store is actually shaped: the bar is the
record of what happened, the feature row is one model generation's view of
it, and the two are rewritten on completely different schedules.

**Why the features are precomputed and stored.** The engine has no
arithmetic in a select list: `close - prev_close` is not expressible, and a
20-session rolling standard deviation is not a fold over a group but a
window over an ordered run, which nothing here has. So the features are
computed by this driver at generation time and stored, exactly as a real
pipeline would materialize them, and every model comparison at the end is
**calculated client-side** from rows the join returned.

Aggregates themselves *are* expressible now (`docs/spec/aggregate.md`
resolved `docs/spec/parser-v2.md` I14): `COUNT`, `SUM`, `MIN`, `MAX` and
`GROUP BY`. They are measured as their own phases (`agg-*`) rather than
folded into the phases above, because rewriting the model comparison to
`GROUP BY model_id` would make this tool and its PostgreSQL twin time
different questions - the point of that phase is the join's cost. What is
still absent, and what keeps the client-side reduction honest, is the
arithmetic: no `SUM(a - b)`, no expression anywhere in a select list.

The eight models, four per family, all long-only and rebalanced every
`--rebalance` sessions:

    mom-5     momentum   long the top --top-k symbols by 5-session return
    mom-20    momentum   ... by 20-session return
    mom-60    momentum   ... by 60-session return
    mom-120   momentum   ... by 120-session return
    vol-low   volatility long the --top-k *lowest* 20-session volatility
    vol-high  volatility long the --top-k *highest* 20-session volatility
    vol-tgt   volatility inverse-volatility weights over 60-session vol,
                         taken only where 20-session momentum is positive
    vol-break volatility long each symbol whose last return exceeded twice
                         its 10-session volatility

They are deliberately not eight tunings of one idea: two families that
disagree about what a cross-section is for, so the comparison at the end has
something to say. Every weight is in basis points and every P&L is int64
basis points of the notional - **money is never a float**, here or in the
schema, because `float` and `decimal` columns are refused at CREATE TABLE
(a relation's row size is a schema constant and their on-disk width is an
open decision, docs/spec/client-manual.md section 3).

The measured phases, in order:

    load-*            the ingest; `load-bars` and `load-stats` are the two
                      that scale with --years x --symbols
    backtest-read     one 3-relation join per rebalance - FilterScan on
                      daily_stats, Probe into daily_bars, Probe into symbols
    result-insert     eight rows appended per rebalance, one per model
    backtest-replay   the same cross-sections re-read (--replay): the
                      research loop, and the only phase where a Cabin on
                      session_no can hit, since the walk forward reads each
                      session exactly once
    compare-all       every result row joined to its model, in one statement
    compare-one       one model's results joined to its model - the shape a
                      dashboard actually issues - over --compare-rounds
                      passes
    read-*            the individual access shapes, priced on their own so
                      the join numbers above are readable

Then the three sweeps, which are the output that matters:

    QPS matrix        seven read shapes x four cases. `cold` uses an
                      argument no statement has used before; `warm` cycles
                      --warm-keys of them; `cabin` is warm with a Cabin
                      declared on the filter column and observed; `dropped`
                      is warm again immediately after DROP CABIN. A shape
                      filtering on the primary key has no cabin case, since
                      a cabin on the pk column is refused.
    INSERT QPS        rows per second at each --write-batches transaction
                      size, on a relation of the sweep's own so the read
                      data set does not move under the report.
    join QPS          the cross-section join at each --connections count,
                      aggregate and per connection.

The cases are switched *between* measurements over identical bytes, which is
what makes the columns comparable to each other. Two whole runs compared
against each other differ by their page cache, their file layout, and
whatever else the machine was doing; these differ by one DDL statement.

`--analyze` prints the step chain and the examined-row count the engine
reports for each read phase. Worth turning on the first time: a `Probe` step
against a **heap** relation has no pk index to descend and silently becomes
a full chain scan, at which point a 3-step join and a 3-relation cross
product produce the same plausible-looking table. `--bars-clustered heap` is
offered precisely so that difference can be measured rather than assumed.

Two engine features can be switched on to change how the reads are served,
and neither may change an answer:

    --cabin   a Cabin (docs/spec/cabin.md) on `daily_stats.session_no` and
              `model_results.model_id` - the two columns every FilterScan in
              this scenario filters on. An observed value's rows are served
              without walking the relation. Note the cross-section case is
              at the edge of its budget on purpose: 30 years is ~7,560
              distinct `session_no` values against a default
              `cabin_max_values` of 4,096, so a long run observes a prefix
              and the rest keep walking - a cap **refuses to observe** and
              never truncates a set, which is what keeps the answers equal.
              A Cabin observes a value on its first equality read and can
              only serve the *second*, so the repeat phases (--replay,
              --compare-rounds) are where it shows; the walk forward on its
              own reads each session once and is all misses by construction.
    --fk      the four references this data has always had, declared:
              daily_bars.symbol_id -> symbols, daily_bars.session_id ->
              sessions, daily_stats.bar_id -> daily_bars, and
              model_results.model_id -> models. Every ingested row then
              probes its parents before it is written, which is the cost
              being measured; nothing here deletes, so the reverse check
              never fires. Requires --bars-clustered btree, since a foreign
              key references the parent's primary key and a heap parent has
              no index to check against.

`--verify` closes the loop: every model's P&L is accumulated in memory as
the backtest runs *and* re-read at the end through the comparison join, and
the two must agree exactly. It verifies the round trip - that the database
returned what was written - not the arithmetic, which has no second
implementation to be checked against.

Usage:
    # start a server first - Release build, scratch data file:
    #   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    #   cmake --build build-release -j
    #   ./build-release/kds_server /tmp/backtest.db --port 15599 \
    #       --log-dir /tmp --log-file bt.log --log-level debug
    python3 tools/scenario1_backtest.py --port 15599   # 30 years, all sweeps
    python3 tools/scenario1_backtest.py --years 5 --symbols 4   # a quick one
    python3 tools/scenario1_backtest.py --qps-ops 500           # tighter QPS
    python3 tools/scenario1_backtest.py --analyze --json qps.json
    python3 tools/scenario1_backtest.py --bars-clustered heap   # what a
                                                                # missing pk
                                                                # index costs
    # the write and concurrency sweeps on their own, over a small data set:
    python3 tools/scenario1_backtest.py --years 2 --no-sweep \
        --write-batches 1,50,500 --connections 1,4,16

Note --sweep and --cabin are mutually exclusive: the sweep creates and drops
its own Cabins to measure the `cabin` and `dropped` columns, and a column
declared CABIN at CREATE TABLE already carries one. Use --cabin --no-sweep
for a whole run with Cabins on from the first statement, which is a
different measurement - it prices the observation cost across the load and
the backtest, where the sweep prices the steady state.

Every run creates its own seven tables, suffixed `_<epoch>_<rand>`, because
there is no DROP TABLE: a shared data file would otherwise accumulate
relations and the load phase would time a growing catalog. Use a scratch
data file and delete it between runs.
"""

import argparse
import math
import random
import re
import statistics
import sys
import time

from bench_common import Phase, report, write_json
from benchmark import read_durability, server_side_us
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply

# ---- schema --------------------------------------------------------------
#
# Column 0 of every relation is the Keystone primary key: system-generated,
# not supplied on INSERT (invariant 11). It is written out in each column
# list anyway because CREATE TABLE declares it; only INSERT omits it.
#
# Storage is chosen per relation and the choices are part of the
# measurement:
#
#   BTREE  everything a join probes by primary key. `daily_bars` is probed
#          once per feature row by the backtest's join, `symbols` once per
#          bar, `models` once per result row - and on a heap relation a
#          `Probe` step has no index to descend, so each of those becomes a
#          full chain scan and the join becomes quadratic. `sessions` is
#          BTREE for the same reason under --fk.
#   HEAP   the two relations that are appended and then walked, never probed
#          by pk: `daily_stats` (walked by session_no, one cross-section at
#          a time) and `model_results` (walked by model_id). A tail append
#          is exactly right for both, and a tree would buy a descent and a
#          split per row for a descent nothing performs.
#
# 49 columns per run. The catalog's relations chain now, so the ceiling is
# thousands of columns for the whole instance rather than the ~68 it was
# (docs/rules/keystoneid-k0-findings.md) - but nothing reclaims a catalog row, so
# it is a ceiling on columns ever created, not on live ones.

SCHEMA = {
    "exchanges": (
        "id int64, code varchar, country varchar, tz_min int32", "BTREE"),
    "symbols": (
        "id int64, ticker varchar, exchange_id int64, sector int32, "
        "listed_session int32", "BTREE"),
    "sessions": (
        "id int64, session_no int32, year int32, month int32, dow int32",
        "BTREE"),
    # The bulk relation, and the one --bars-clustered moves.
    "daily_bars": (
        "id int64, symbol_id int64, session_id int64, session_no int32, "
        "open int64, high int64, low int64, close int64, volume int64",
        "BTREE"),
    # The feature row. Four momentum lookbacks and three volatility windows
    # are stored as separate columns rather than one generic (window, value)
    # pair per row, because the models read a whole cross-section in one
    # statement and a long-form table would multiply that read by seven.
    "daily_stats": (
        "id int64, bar_id int64, symbol_id int64, session_no int32, "
        "ret_bp int64, mom5_bp int64, mom20_bp int64, mom60_bp int64, "
        "mom120_bp int64, vol10_bp int64, vol20_bp int64, vol60_bp int64",
        "HEAP"),
    "models": (
        "id int64, name varchar, family int32, lookback int32, top_k int32, "
        "param_bp int64", "BTREE"),
    "model_results": (
        "id int64, model_id int64, period_no int32, session_no int32, "
        "pnl_bp int64, equity_bp int64, positions int32, trades int32",
        "HEAP"),
}

# Creation order matters under --fk: a parent has to exist before a child
# can reference it, and there is no ALTER TABLE to add the constraint
# afterwards.
CREATE_ORDER = ("exchanges", "symbols", "sessions", "daily_bars",
                "daily_stats", "models", "model_results")

# ---- the Cabin (docs/spec/cabin.md) --------------------------------------
#
# Two cabins, on the two columns this scenario filters by equality:
#
#   daily_stats.session_no    the backtest's cross-section read, once per
#                             rebalance
#   model_results.model_id    the per-model comparison read, once per model
#
# Their value counts sit on either side of the default budget on purpose.
# `model_id` has exactly eight distinct values and is observed whole after
# the first pass. `session_no` has one per trading day - ~7,560 over 30
# years against a `cabin_max_values` default of 4,096 - so a full run
# observes a prefix and the rest of the sessions keep walking the relation.
# That is the cap doing what it is specified to do: **refuse to observe a
# value, never truncate a value's entry set**, because a truncated set
# marked authoritative is a wrong answer where an unobserved value is only a
# slower one.
CABIN_COLUMNS = (("daily_stats", "session_no"), ("model_results", "model_id"))

# ---- the foreign keys (docs/spec/foreign-keys.md) ------------------------
#
# (child, column, parent). Every one of these is a reference this data
# already had - the driver only ever writes ids it created - so declaring
# them must not change a single row. What moves is the write cost: each
# ingested bar probes two parents and each feature row probes one, before
# the row is written.
#
# A foreign key references the parent's Keystone primary key and never a
# business key (F1), which is why none of these names a parent column: there
# is only one possible answer. All four parents are BTREE because a heap
# parent is refused at declaration - it has no pk index, so every check
# would scan it.
FOREIGN_KEYS = (
    ("daily_bars", "symbol_id", "symbols"),
    ("daily_bars", "session_id", "sessions"),
    ("daily_stats", "bar_id", "daily_bars"),
    ("model_results", "model_id", "models"),
)

# ---- the eight models ----------------------------------------------------
#
# Two families that disagree about what a cross-section is for. The momentum
# family ranks by past return over four lookbacks; the volatility family
# ranks by dispersion, in three different directions plus one breakout rule.
#
# `param_bp` is the family's second parameter, stored so the models relation
# describes the run rather than merely naming it: for the momentum family it
# is the minimum momentum a symbol must show to be taken at all, and for
# vol-break it is the multiple of volatility a move must exceed.

FAMILY_MOMENTUM, FAMILY_VOLATILITY = 0, 1
FAMILY_NAME = {FAMILY_MOMENTUM: "momentum", FAMILY_VOLATILITY: "volatility"}

MODELS = (
    # name, family, lookback, param_bp
    ("mom-5", FAMILY_MOMENTUM, 5, 0),
    ("mom-20", FAMILY_MOMENTUM, 20, 0),
    ("mom-60", FAMILY_MOMENTUM, 60, 0),
    ("mom-120", FAMILY_MOMENTUM, 120, 0),
    ("vol-low", FAMILY_VOLATILITY, 20, 0),
    ("vol-high", FAMILY_VOLATILITY, 20, 0),
    ("vol-tgt", FAMILY_VOLATILITY, 60, 0),
    ("vol-break", FAMILY_VOLATILITY, 10, 20000),  # 2.0x, in basis points
)

# One whole notional, in basis points. Every model is fully invested or
# holding cash; a weight of 10000 is 100% of the book.
FULL_WEIGHT_BP = 10_000

SECTORS = 8
COUNTRIES = ("KR", "US", "JP", "GB", "DE", "SG", "HK", "AU")
EXCHANGE_CODES = ("XKRX", "XNYS", "XTKS", "XLON", "XETR", "XSES", "XHKG",
                  "XASX")

# 252 business days a year is the convention every performance number in
# finance is annualized against; using it here keeps `--years` meaning what
# a reader expects rather than 365 rows of which two fifths are weekends.
SESSIONS_PER_YEAR = 252

INSERTED_ID = re.compile(r"\bid=(\d+)")


def abort(message, reply=None):
    print(f"scenario1 aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def connect(host, port, timeout):
    try:
        return ServerConnection(host, port, timeout=timeout)
    except OSError as e:
        abort(f"could not connect to {host}:{port}: {e}\n"
              f"  start one with: ./build-release/kds_server /tmp/backtest.db "
              f"--port {port}")


# ---- --echo: every statement, as it is sent ------------------------------
#
# Off by default and not free: a write per statement, on a tool whose load
# phase is a hundred thousand of them. A run with it on is a run to read,
# not a run to quote.
ECHO = False
ECHO_REPLY_MAX = 96


def set_echo(enabled):
    global ECHO
    ECHO = bool(enabled)


class Client:
    """One connection plus the one-command-one-reply callable everything
    below is written against.

    Deliberately a local copy of the same 40 lines scenario0_stockmarket.py
    carries, rather than an import of it: the two scenarios share
    bench_common and nothing else, so neither can break the other by
    changing how it drives a socket.
    """

    def __init__(self, host, port, timeout):
        self._conn = connect(host, port, timeout)
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            shown = (reply if len(reply) <= ECHO_REPLY_MAX
                     else reply[:ECHO_REPLY_MAX] + "...")
            print(f"{command}  ->  {shown}", file=sys.stderr, flush=True)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def timed(self, command, phase):
        """Sends `command`, charging its round trip to `phase`."""
        t0 = time.perf_counter()
        reply = self(command)
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def close(self):
        self._conn.close()


# ---- reply parsing -------------------------------------------------------
#
# A SELECT reply is one wire line: a header of comma-separated column names,
# then one comma-separated row per match, joined with the literal `\n`
# escape that format_reply() has already turned into real newlines.

def select_rows(reply):
    """The data rows of a SELECT reply, as lists of field strings.

    An `ERR` reply yields nothing rather than raising: a phase counts its
    own errors, and a caller that gets an empty cross-section behaves the
    same whether the statement failed or genuinely matched no rows. Which of
    the two it was is in the phase's error count, printed beside it.
    """
    if reply.startswith("ERR"):
        return []
    lines = [line for line in reply.split("\n") if line != ""]
    return [line.split(",") for line in lines[1:]]


def inserted_id(reply):
    got = INSERTED_ID.search(reply)
    return int(got.group(1)) if got else None


# ---- schema construction -------------------------------------------------

def schema_for(base, cabin, fk, suffix, bars_clustered):
    """The column list and storage clause for `base`, with the cabin policy,
    the foreign keys and --bars-clustered applied.

    Textual substitution rather than a second SCHEMA table: the point is that
    every variant of this run is the *same* schema apart from one clause
    each, and two tables would let them drift apart silently.

    Both clauses are collected per column and appended in **one** pass, in
    the order `<col> <type> REFERENCES <parent> CABIN`. That order is fixed
    by the engine and not a preference - two optional clauses accepted in
    either order is a grammar with no statable shape - and building them in
    two independent passes is how this got it wrong: a column carrying both
    (`model_results.model_id`, under `--cabin --fk`) came out as
    `CABIN REFERENCES` and was refused at the REFERENCES.
    """
    columns, clustered = SCHEMA[base]

    if base == "daily_bars":
        clustered = bars_clustered.upper()

    # column name -> the clauses to append, already in engine order.
    suffixes = {}
    if fk:
        for child, column, parent in FOREIGN_KEYS:
            if child == base:
                suffixes.setdefault(column, []).append(
                    f"REFERENCES {parent}_{suffix}")
    if cabin:
        for relation, column in CABIN_COLUMNS:
            if relation == base:
                suffixes.setdefault(column, []).append("CABIN")

    if not suffixes:
        return columns, clustered

    # Rebuilt from the split rather than by string replacement: a column
    # name is a prefix of no other name here today, and rebuilding means it
    # never has to be.
    rebuilt = []
    for declaration in columns.split(", "):
        name = declaration.split(" ", 1)[0]
        extra = suffixes.get(name)
        rebuilt.append(f"{declaration} {' '.join(extra)}" if extra
                       else declaration)
    return ", ".join(rebuilt), clustered


def create_tables(client, suffix, cabin, fk, bars_clustered):
    for base in CREATE_ORDER:
        columns, clustered = schema_for(base, cabin, fk, suffix,
                                        bars_clustered)
        reply = client(f"CREATE TABLE {base}_{suffix} ({columns}) {clustered}")
        if not reply.startswith("ERR"):
            continue

        # A server built before either feature parses the column list up to
        # the clause it does not know and then refuses the rest. Saying so
        # beats a syntax error pointing into the middle of a column.
        if fk and "REFERENCES" in reply.upper():
            abort(f"--fk: this server does not understand REFERENCES.\n  It "
                  f"needs a build with docs/spec/foreign-keys.md in it "
                  f"(FK-M1); re-run without --fk, or rebuild the server.",
                  reply)
        if fk and "heap relation" in reply:
            abort(f"--fk: {base} references a heap parent, which is refused - "
                  f"a foreign key references the parent's primary key and a "
                  f"heap relation has no pk index, so every check would scan "
                  f"it.\n  Re-run with --bars-clustered btree, or without "
                  f"--fk.", reply)
        if cabin and "CABIN" in reply.upper():
            abort(f"--cabin: this server does not understand the column cabin "
                  f"policy.\n  It needs a build with docs/spec/cabin.md in "
                  f"it; re-run without --cabin, or rebuild the server.", reply)
        if "no room" in reply or "reserved catalog page range" in reply:
            abort(f"could not create {base}_{suffix}: the catalog is out of "
                  f"column space.\n  This scenario spends 49 columns per run "
                  f"and nothing reclaims them, because there is no DROP "
                  f"TABLE.\n  Restart the server on a fresh data file.", reply)
        abort(f"could not create {base}_{suffix}", reply)


# ---- market data generation ----------------------------------------------
#
# A geometric random walk per symbol, in integer minor units, with a
# per-symbol daily volatility so the cross-section has something for the
# volatility family to rank. Deterministic in --seed: two runs with the same
# seed generate byte-identical prices, which is what makes a --cabin run and
# a plain one comparable at all.

def generate_series(sessions, rng):
    """One symbol's closing prices, in minor units, one per session."""
    price = float(rng.randint(5_000, 500_000))
    # Between 0.6% and 3.5% daily: the spread is the point, since a
    # cross-section where every symbol has the same volatility gives the
    # volatility family nothing to choose between.
    daily_vol = rng.uniform(0.006, 0.035)
    # A small per-symbol drift, so momentum has a signal to find and not
    # only noise to overfit.
    drift = rng.gauss(0.0002, 0.0004)
    closes = []
    for _ in range(sessions):
        price *= math.exp(drift + rng.gauss(0.0, daily_vol))
        # A floor rather than a wrap: a symbol that walks to zero would give
        # every later return a division by nothing, and delisting is not
        # something this schema models.
        closes.append(max(100, int(price)))
    return closes


def basis_points(now, before):
    """(now / before - 1) in basis points, as an integer.

    Integer basis points everywhere, never a float and never a decimal:
    `float` and `decimal` columns are refused at CREATE TABLE, so a return
    that cannot be stored is a return this driver must not compute either.
    """
    if before <= 0:
        return 0
    return int(round((now - before) * 10_000 / before))


def derive_stats(closes):
    """The feature rows for one symbol's price series.

    Returns a list of dicts, one per session, carrying the four momentum
    lookbacks and three volatility windows the models read. A window that
    has not filled yet contributes 0 rather than being skipped: every bar
    gets a feature row, so the two relations stay in one-to-one
    correspondence and the join's row count is a fact about the data rather
    than about the warm-up.
    """
    returns = [0]
    for i in range(1, len(closes)):
        returns.append(basis_points(closes[i], closes[i - 1]))

    def momentum(i, window):
        return basis_points(closes[i], closes[i - window]) if i >= window else 0

    def volatility(i, window):
        if i < window:
            return 0
        # Population standard deviation of the window's returns, in basis
        # points. pstdev rather than stdev because the window is the whole
        # population being described, not a sample of a larger one.
        return int(round(statistics.pstdev(returns[i - window + 1:i + 1])))

    rows = []
    for i in range(len(closes)):
        rows.append({
            "ret_bp": returns[i],
            "mom5_bp": momentum(i, 5),
            "mom20_bp": momentum(i, 20),
            "mom60_bp": momentum(i, 60),
            "mom120_bp": momentum(i, 120),
            "vol10_bp": volatility(i, 10),
            "vol20_bp": volatility(i, 20),
            "vol60_bp": volatility(i, 60),
        })
    return rows


def ohlc_from(close, ret_bp, rng):
    """An open/high/low around a close, so the bar relation carries a real
    row shape rather than one number repeated four times. Nothing reads
    these - they are here because a bar that is only a close is not a bar,
    and because the row width is part of what the ingest measures."""
    span = max(1, abs(close * max(50, abs(ret_bp)) // 10_000))
    open_ = max(1, close - int(round(close * ret_bp / 10_000)))
    high = max(open_, close) + rng.randint(0, span)
    low = max(1, min(open_, close) - rng.randint(0, span))
    return open_, high, low


# ---- load ----------------------------------------------------------------

class Batcher:
    """Wraps a run of inserts in BEGIN/COMMIT every `size` statements.

    Ingest is the phase this exists for. Without it every INSERT is its own
    transaction and pays its own durability point (`docs/spec/txn.md`); with it a
    batch pays one. The batch is committed and reopened rather than held
    open for the whole load, because an explicit transaction that spans the
    entire ingest holds one write transaction open across a hundred thousand
    round trips - and a failure anywhere in it unwinds all of them.

    A no-op when `size` is 0, which is what --no-load-txn asks for.
    """

    def __init__(self, client, size):
        self._client = client
        self._size = size
        self._open = False
        self._count = 0

    def step(self):
        """Called once per row, before the insert is sent."""
        if self._size <= 0:
            return
        if not self._open:
            self._client("BEGIN")
            self._open = True
            self._count = 0
        self._count += 1

    def maybe_commit(self):
        """Called once per row, after the insert. Commits on a full batch."""
        if self._size <= 0 or not self._open:
            return
        if self._count >= self._size:
            self._client("COMMIT")
            self._open = False

    def finish(self):
        if self._open:
            self._client("COMMIT")
            self._open = False


def load_lookups(client, suffix, symbol_count, exchange_count, rng, phases):
    """Exchanges and symbols. Returns [(symbol_id, ticker, exchange_id)].

    Ids are read back from each reply rather than assumed to be 1..n: they
    are ascending but **not gapless**, since a failed insert burns one, and a
    run that hits an error must not go on addressing rows by ordinal.
    """
    phase = Phase("load-exchanges", "venues")
    exchange_ids = []
    for i in range(exchange_count):
        code = EXCHANGE_CODES[i % len(EXCHANGE_CODES)] + (
            f"{i // len(EXCHANGE_CODES)}" if i >= len(EXCHANGE_CODES) else "")
        reply = client.timed(
            f"INSERT INTO exchanges_{suffix} VALUES "
            f"('{code}', '{COUNTRIES[i % len(COUNTRIES)]}', "
            f"{rng.randint(-12, 12) * 60})", phase)
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
        reply = client.timed(
            f"INSERT INTO symbols_{suffix} VALUES "
            f"('{ticker}', {exchange_id}, {i % SECTORS}, 0)", phase)
        got = inserted_id(reply)
        if got:
            symbols.append((got, ticker, exchange_id))
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)
    return symbols


def load_sessions(client, suffix, count, start_year, batch, phases):
    """The trading calendar: one row per business day. Returns the session
    ids in session order, which is what every bar links to."""
    phase = Phase("load-sessions", f"{count:,} business days")
    batcher = Batcher(client, batch)
    ids = []
    for n in range(count):
        year = start_year + n // SESSIONS_PER_YEAR
        # A synthetic month from the position in the year, and a weekday
        # from the position in the week. Neither is a real calendar, and
        # neither is read by a model - they are here so the relation carries
        # the columns a calendar carries.
        month = 1 + (n % SESSIONS_PER_YEAR) * 12 // SESSIONS_PER_YEAR
        batcher.step()
        reply = client.timed(
            f"INSERT INTO sessions_{suffix} VALUES "
            f"({n}, {year}, {month}, {n % 5})", phase)
        batcher.maybe_commit()
        got = inserted_id(reply)
        if got:
            ids.append(got)
    batcher.finish()
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)
    return ids


def load_history(client, suffix, symbols, session_ids, rng, batch, phases):
    """The two bulk relations, one symbol's whole history at a time.

    Bars first, then that symbol's feature rows: a feature row carries the
    id of the bar it describes, so the bar's INSERT reply has to come back
    before the feature row can be written. Per symbol rather than per
    session so the two relations are appended in long runs - a chain grows
    at the tail, and interleaving would gain nothing but a second pass over
    the same ids.

    Returns {symbol_id: [close, ...]} - the prices as generated, kept only
    so --verify has something the database was never asked about.
    """
    bars_phase = Phase("load-bars", "OHLCV, one row per (symbol, session)")
    stats_phase = Phase("load-stats", "derived features, one row per bar")
    batcher = Batcher(client, batch)
    series = {}

    for index, (symbol_id, _ticker, _exchange_id) in enumerate(symbols, 1):
        closes = generate_series(len(session_ids), rng)
        stats = derive_stats(closes)
        series[symbol_id] = closes

        bar_ids = []
        for n, session_id in enumerate(session_ids):
            open_, high, low = ohlc_from(closes[n], stats[n]["ret_bp"], rng)
            batcher.step()
            reply = client.timed(
                f"INSERT INTO daily_bars_{suffix} VALUES "
                f"({symbol_id}, {session_id}, {n}, {open_}, {high}, {low}, "
                f"{closes[n]}, {rng.randint(1_000, 50_000_000)})", bars_phase)
            batcher.maybe_commit()
            bar_ids.append(inserted_id(reply))

        for n, bar_id in enumerate(bar_ids):
            if bar_id is None:
                # The bar this feature row would describe was not written.
                # Writing the feature row anyway would give it a dangling
                # bar_id, which under --fk is a violation and without --fk is
                # a row the join silently drops.
                continue
            s = stats[n]
            batcher.step()
            client.timed(
                f"INSERT INTO daily_stats_{suffix} VALUES "
                f"({bar_id}, {symbol_id}, {n}, {s['ret_bp']}, {s['mom5_bp']}, "
                f"{s['mom20_bp']}, {s['mom60_bp']}, {s['mom120_bp']}, "
                f"{s['vol10_bp']}, {s['vol20_bp']}, {s['vol60_bp']})",
                stats_phase)
            batcher.maybe_commit()

        print(f"  symbol {index}/{len(symbols)}: {len(bar_ids):,} bars",
              flush=True)

    batcher.finish()
    bars_phase.elapsed = sum(bars_phase.latencies)
    stats_phase.elapsed = sum(stats_phase.latencies)
    phases.extend((bars_phase, stats_phase))
    return series


def load_models(client, suffix, top_k, phases):
    """The eight strategy rows. Returns [(model_id, name, family, lookback,
    param_bp)] in declaration order."""
    phase = Phase("load-models", f"{len(MODELS)} strategies")
    rows = []
    for name, family, lookback, param_bp in MODELS:
        reply = client.timed(
            f"INSERT INTO models_{suffix} VALUES "
            f"('{name}', {family}, {lookback}, {top_k}, {param_bp})", phase)
        got = inserted_id(reply)
        if got is None:
            abort(f"could not create model {name}", client.first_error)
        rows.append((got, name, family, lookback, param_bp))
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)
    return rows


# ---- the models ----------------------------------------------------------
#
# Each takes one cross-section - the rows the backtest's join returned for
# one session - and answers {symbol_id: weight_bp}, weights summing to at
# most FULL_WEIGHT_BP. A model that likes nothing answers {}, which is cash
# and is a perfectly good answer; a backtest that forces a position every
# period is measuring a different strategy from the one it describes.
#
# All eight are long-only and equal-weight apart from vol-tgt, whose whole
# idea is that they should not be.


def _equal_weights(chosen):
    if not chosen:
        return {}
    each = FULL_WEIGHT_BP // len(chosen)
    return {symbol_id: each for symbol_id in chosen}


# A model's `lookback` is stored in the `models` relation, so the column it
# reads has to be derived from that number rather than written into the
# branch - otherwise editing MODELS changes what the catalog says a model is
# and leaves what it does untouched, which is the one inconsistency this
# scenario cannot detect for itself. Both maps are the windows daily_stats
# actually carries; asking for another is a KeyError at the first
# cross-section rather than a silently wrong backtest.
MOMENTUM_COLUMN = {5: "mom5_bp", 20: "mom20_bp", 60: "mom60_bp",
                   120: "mom120_bp"}
VOLATILITY_COLUMN = {10: "vol10_bp", 20: "vol20_bp", 60: "vol60_bp"}


def _momentum_positions(cross_section, lookback, top_k):
    key = MOMENTUM_COLUMN[lookback]
    # Only symbols whose momentum is actually positive: the top k of a
    # cross-section that is uniformly falling is still a long book, and
    # ranking without a sign test is the classic way a momentum backtest
    # reports a loss it never had to take.
    ranked = sorted((row for row in cross_section if row[key] > 0),
                    key=lambda row: row[key], reverse=True)
    return _equal_weights([row["symbol_id"] for row in ranked[:top_k]])


def _volatility_positions(name, cross_section, lookback, param_bp, top_k):
    key = VOLATILITY_COLUMN[lookback]

    if name in ("vol-low", "vol-high"):
        # The same ranking read from both ends. A zero is a window that has
        # not filled yet, not a riskless symbol, so it is excluded rather
        # than handed the top of the low-volatility book.
        ranked = sorted((row for row in cross_section if row[key] > 0),
                        key=lambda row: row[key], reverse=(name == "vol-high"))
        return _equal_weights([row["symbol_id"] for row in ranked[:top_k]])

    if name == "vol-tgt":
        # Inverse-volatility weights over the symbols with positive
        # 20-session momentum: size by risk, direct by trend. The one model
        # here whose weights are not equal, which is why it is worth having.
        eligible = [row for row in cross_section
                    if row[key] > 0 and row["mom20_bp"] > 0]
        if not eligible:
            return {}
        inverse = {row["symbol_id"]: 1.0 / row[key] for row in eligible}
        total = sum(inverse.values())
        weights = {symbol_id: int(FULL_WEIGHT_BP * share / total)
                   for symbol_id, share in inverse.items()}
        return {k: v for k, v in weights.items() if v > 0}

    if name == "vol-break":
        # A breakout: today's move exceeded `param_bp` times the recent
        # volatility, and it was up.
        chosen = [row["symbol_id"] for row in cross_section
                  if row[key] > 0 and row["ret_bp"] > 0
                  and row["ret_bp"] * 10_000 > param_bp * row[key]]
        return _equal_weights(chosen[:top_k])

    raise ValueError(f"unknown volatility model {name}")


def positions_for(name, family, lookback, param_bp, top_k, cross_section):
    if family == FAMILY_MOMENTUM:
        return _momentum_positions(cross_section, lookback, top_k)
    return _volatility_positions(name, cross_section, lookback, param_bp,
                                 top_k)


# ---- the backtest --------------------------------------------------------

def cross_section_sql(suffix, session_no):
    """The read the whole scenario is built around: one session's feature
    rows, each joined to the bar it describes and to that bar's symbol.

    Written order is execution order - the statement *is* the chain, never
    silently reordered (`docs/spec/parser-v2.md`) - so this reads exactly as it
    runs: walk `daily_stats` filtered on `session_no` (a FilterScan, since
    the column is not the pk and carries no index), then for each surviving
    row descend `daily_bars` by primary key, then `symbols` by primary key.

    `ticker` is projected and never used by a model. It is here because a
    dashboard issuing this query wants it, and because dropping the third
    relation would quietly turn a 3-step chain into a 2-step one.
    """
    return (
        f"SELECT t.symbol_id, t.ret_bp, t.mom5_bp, t.mom20_bp, t.mom60_bp, "
        f"t.mom120_bp, t.vol10_bp, t.vol20_bp, t.vol60_bp, b.close, s.ticker "
        f"FROM daily_stats_{suffix} AS t "
        f"JOIN daily_bars_{suffix} AS b ON t.bar_id = b.id "
        f"JOIN symbols_{suffix} AS s ON t.symbol_id = s.id "
        f"WHERE t.session_no = {session_no}")


CROSS_SECTION_FIELDS = ("symbol_id", "ret_bp", "mom5_bp", "mom20_bp",
                        "mom60_bp", "mom120_bp", "vol10_bp", "vol20_bp",
                        "vol60_bp", "close")


def parse_cross_section(reply):
    """The join's rows as dicts. `ticker` is dropped here rather than not
    projected: see cross_section_sql()."""
    rows = []
    for fields in select_rows(reply):
        if len(fields) < len(CROSS_SECTION_FIELDS):
            continue
        try:
            rows.append({name: int(fields[i])
                         for i, name in enumerate(CROSS_SECTION_FIELDS)})
        except ValueError:
            # A row that does not parse is a row this driver cannot score.
            # Skipped rather than fatal: the phase's error count and the
            # --verify pass are what report it.
            continue
    return rows


def default_result_insert(suffix, values):
    """The result-row INSERT, in ckdbs's positional form: no column list,
    and the Keystone pk deliberately not supplied (invariant 11).

    Pulled out as a builder so the PostgreSQL baseline can pass its own -
    an identity column there refuses a positional value list, so it needs
    the body columns named. The *values* are built by run_backtest either
    way, which is what keeps the two engines writing the same numbers.
    """
    return f"INSERT INTO model_results_{suffix} VALUES ({values})"


def run_backtest(client, suffix, models, session_count, rebalance, top_k,
                 phases, insert_builder=default_result_insert):
    """Walks forward through the sessions, rebalancing every `rebalance` of
    them, and returns per-model state.

    The loop at each rebalance session:

      1. read the cross-section (one 3-relation join)
      2. mark to market: last period's positions earned the move in each
         symbol's close between the previous read and this one
      3. append one `model_results` row per model for the period that just
         ended
      4. each model picks its new positions from the cross-section it just
         read

    Marking to market from *DB-read* closes rather than from the generated
    series is the point: every number the comparison reports came back
    through the join being measured. Step 3 is skipped on the first
    rebalance, when there is no completed period yet.
    """
    read_phase = Phase("backtest-read",
                       "3-relation join, one cross-section per rebalance")
    insert_phase = Phase("result-insert",
                         f"{len(models)} rows per rebalance, one per model")

    state = {}
    for model_id, name, family, lookback, param_bp in models:
        state[model_id] = {
            "model_id": model_id,
            "name": name,
            "family": family,
            "lookback": lookback,
            "param_bp": param_bp,
            "positions": {},      # symbol_id -> weight_bp
            "equity_bp": 0,       # cumulative P&L in bp of notional
            "periods": [],        # per-period pnl_bp, for the comparison
            "trades": 0,
        }

    previous_closes = {}
    period_no = 0
    rebalance_sessions = list(range(0, session_count, rebalance))

    for step, session_no in enumerate(rebalance_sessions):
        reply = client.timed(cross_section_sql(suffix, session_no), read_phase)
        cross_section = parse_cross_section(reply)
        closes = {row["symbol_id"]: row["close"] for row in cross_section}

        if step > 0:
            for model in state.values():
                pnl_bp = 0
                for symbol_id, weight_bp in model["positions"].items():
                    before = previous_closes.get(symbol_id)
                    now = closes.get(symbol_id)
                    if before is None or now is None:
                        # A symbol that did not come back in this session's
                        # cross-section contributes nothing. It cannot happen
                        # on generated data where every symbol trades every
                        # session, and it is handled anyway because the
                        # alternative is a KeyError in the middle of a run.
                        continue
                    pnl_bp += weight_bp * basis_points(now, before) // 10_000
                model["equity_bp"] += pnl_bp
                model["periods"].append(pnl_bp)

                client.timed(
                    insert_builder(
                        suffix,
                        f"{model['model_id']}, {period_no}, {session_no}, "
                        f"{pnl_bp}, {model['equity_bp']}, "
                        f"{len(model['positions'])}, {model['trades']}"),
                    insert_phase)
            period_no += 1

        for model in state.values():
            chosen = positions_for(model["name"], model["family"],
                                   model["lookback"], model["param_bp"],
                                   top_k, cross_section)
            # A trade is a position that was not held before or is no longer
            # held: the symmetric difference, which is what a turnover
            # number actually counts.
            model["trades"] = len(set(chosen) ^ set(model["positions"]))
            model["positions"] = chosen

        previous_closes = closes

    read_phase.elapsed = sum(read_phase.latencies)
    insert_phase.elapsed = sum(insert_phase.latencies)
    phases.extend((read_phase, insert_phase))
    return state, period_no


# ---- the comparison ------------------------------------------------------

def compare_all_sql(suffix):
    """Every result row joined to the model that produced it, in one
    statement: a Scan of `model_results` and a pk Probe into `models` per
    row. This is the statement a "compare the models" report issues, and the
    one whose result the client then reduces itself.

    Deliberately not rewritten to `SUM(...) GROUP BY model_id`, which the
    grammar now takes (`docs/spec/aggregate.md`): this phase prices the
    *join*, and its PostgreSQL twin has to issue the same statement for the
    two numbers to mean anything. The fold is measured on its own, in the
    `agg-*` phases."""
    return (f"SELECT r.model_id, r.period_no, r.pnl_bp, r.equity_bp, "
            f"r.trades, m.name, m.family "
            f"FROM model_results_{suffix} AS r "
            f"JOIN models_{suffix} AS m ON r.model_id = m.id")


def compare_one_sql(suffix, model_id):
    """One model's history: a FilterScan of `model_results` on `model_id`
    plus the same pk Probe. The shape a per-model page issues, and the one
    a Cabin on `model_results.model_id` serves - eight distinct values is
    well inside any budget, so under --cabin this is the read that is fully
    observed."""
    return (f"SELECT r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name "
            f"FROM model_results_{suffix} AS r "
            f"JOIN models_{suffix} AS m ON r.model_id = m.id "
            f"WHERE r.model_id = {model_id}")


def read_back_results(client, suffix, phases):
    """Reduces the comparison join into per-model statistics.

    Everything here is arithmetic the engine cannot do: totals, means,
    standard deviations, hit rates. It is done on rows the join returned,
    which is what the phase timing above already paid for - so the cost of
    the comparison is the cost of the read, and this reduction is free.
    """
    phase = Phase("compare-all", "every result row, joined to its model")
    reply = client.timed(compare_all_sql(suffix), phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    by_model = {}
    for fields in select_rows(reply):
        if len(fields) < 7:
            continue
        try:
            model_id = int(fields[0])
            period_no = int(fields[1])
            pnl_bp = int(fields[2])
            equity_bp = int(fields[3])
            trades = int(fields[4])
        except ValueError:
            continue
        entry = by_model.setdefault(model_id, {
            "name": fields[5], "family": int(fields[6]) if fields[6].isdigit()
            else FAMILY_MOMENTUM, "pnl": [], "equity_bp": 0, "trades": 0,
            "last_period": -1})
        entry["pnl"].append(pnl_bp)
        entry["trades"] += trades
        # The cumulative figure belongs to the *latest* period, chosen by
        # period_no rather than by arrival. A heap chain is walked in
        # roughly pk order, so the last row to arrive is in fact the latest
        # one - but that is a property of how this relation happens to be
        # stored, and `equity_bp` is the number --verify compares. Taking
        # the maximum period makes it a property of the data instead.
        if period_no > entry["last_period"]:
            entry["last_period"] = period_no
            entry["equity_bp"] = equity_bp
    return by_model


def score(entry):
    """The comparison numbers, all computed client-side from the join.

    `risk_adj` is mean period P&L over its standard deviation, annualized by
    the number of periods in a year - an information ratio in all but name.
    It is not a Sharpe: there is no risk-free rate in this schema, and
    inventing one would be a number about this driver rather than about the
    models.
    """
    pnl = entry["pnl"]
    if not pnl:
        return {"periods": 0, "total_bp": 0, "mean_bp": 0.0, "stdev_bp": 0.0,
                "best_bp": 0, "worst_bp": 0, "win_rate": 0.0,
                "risk_adj": 0.0, "trades": entry["trades"]}
    mean = statistics.fmean(pnl)
    stdev = statistics.pstdev(pnl) if len(pnl) > 1 else 0.0
    wins = sum(1 for p in pnl if p > 0)
    return {
        "periods": len(pnl),
        "total_bp": entry["equity_bp"],
        "mean_bp": mean,
        "stdev_bp": stdev,
        "best_bp": max(pnl),
        "worst_bp": min(pnl),
        "win_rate": wins / len(pnl),
        "risk_adj": (mean / stdev) if stdev > 0 else 0.0,
        "trades": entry["trades"],
    }


# ---- the individual read shapes ------------------------------------------
#
# Priced on their own so the join phases above are readable. Each is one
# access kind against one relation, and the point of running them beside the
# joins is that a join's cost is the sum of its steps - a `backtest-read`
# that is slower than `read-day-slice` plus two probes has a third thing
# going on.

def run_read_phases(client, suffix, symbols, session_count, bar_count, ops,
                    rng, phases):
    symbol_ids = [symbol_id for symbol_id, _, _ in symbols]

    phase = Phase("read-bar-lookup", "pk equality on daily_bars")
    for _ in range(ops):
        client.timed(f"SELECT * FROM daily_bars_{suffix} "
                     f"WHERE id = {rng.randint(1, max(1, bar_count))}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    # A pk BETWEEN compiles to a Range step, which prunes the *tail* of the
    # chain - the first page whose min_key passes the high bound ends the
    # walk - and never the head. A range near the start of the relation is
    # therefore cheap and one near the end reads everything before it, so
    # the low bound is drawn uniformly on purpose: the spread across that is
    # the measurement.
    phase = Phase("read-bar-range", "pk BETWEEN on daily_bars, 200 wide")
    for _ in range(ops):
        low = rng.randint(1, max(1, bar_count - 200))
        client.timed(f"SELECT * FROM daily_bars_{suffix} "
                     f"WHERE id BETWEEN {low} AND {low + 200}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    # One symbol's entire 30 years: the heaviest single read in the
    # scenario, and the one a research notebook issues first.
    phase = Phase("read-symbol-history", "FilterScan, one symbol's whole run")
    for _ in range(max(1, ops // 20)):
        client.timed(f"SELECT * FROM daily_stats_{suffix} "
                     f"WHERE symbol_id = {rng.choice(symbol_ids)}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    phase = Phase("read-day-slice", "FilterScan, one session's features")
    for _ in range(ops):
        client.timed(f"SELECT * FROM daily_stats_{suffix} "
                     f"WHERE session_no = {rng.randrange(session_count)}",
                     phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    # A 3-relation point join, anchored on a bar's pk: Lookup, then two
    # probes. The cheapest possible join in this schema, and the floor the
    # cross-section read should be read against.
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

    # A correlated EXISTS - a semi-join. Same probe as the point join, but
    # it stops at the first qualifying row instead of pairing, so it prices
    # the short circuit rather than the pairing.
    phase = Phase("read-join-exists", "semi-join: symbols with a bar")
    for _ in range(max(1, ops // 20)):
        client.timed(
            f"SELECT s.ticker FROM symbols_{suffix} AS s "
            f"WHERE EXISTS (SELECT b.id FROM daily_bars_{suffix} AS b "
            f"WHERE b.symbol_id = s.id)", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)


# ---- the aggregate shapes (docs/spec/aggregate.md) -----------------------
#
# The rollups a research notebook issues beside the backtest: how many bars
# per symbol, what the price range was, how wide each session was.
#
# **The first four run over one relation, `daily_bars`, and move only the
# group count.** That is what makes them a measurement rather than five
# timings: the row count, the row width, the storage and the walk are held
# fixed, so the spread across them is the one thing about a fold that is not
# obvious - `bench/results-aggregate.md` found that **its cost tracks group
# count, not row count**, and this is that curve drawn on a real relation.
#
#   agg-global      every bar -> 1 row. No key encoded, no map probed; the
#                   shape that can be *faster* than its unaggregated twin,
#                   because it folds every row and serialises one.
#   agg-by-symbol   -> 8 groups (--symbols). The map exists and fits in
#                   cache.
#   agg-by-session  -> one group per trading day, ~7,560 over 30 years. The
#                   high-cardinality end, where nearly every row founds a
#                   group - and the scale `aggregate_max_groups` of 65,536
#                   is a backstop for rather than a limit on.
#   agg-distinct    the one path that allocates per new value: an observed
#                   set per (group, item), over the same encoding the group
#                   key uses.
#
# `agg-day-slice` is the exception and the only one with a **direct
# unaggregated twin in this table**: it is `read-day-slice`'s statement with
# a fold on top, same relation and same predicate. One session's cross
# section is 8 rows in 8 groups, so the fold collapses nothing and pays for
# the map anyway - the shape where it buys least, and the pair is the
# measurement.
#
# Against PostgreSQL these are the sharpest comparison in the scenario, for a
# structural reason: PostgreSQL plans a HashAggregate or a GroupAggregate and
# may read the grouping column from an index, while ckdbs walks the relation
# and folds outside the executor with no plan choice at all
# (docs/spec/aggregate.md AG1).
#
# **This comment used to predict that ckdbs would not be close on the
# high-cardinality shape. It was measured backwards** - see
# bench/results-scenario1-vs-pg.md. ckdbs loses the low-cardinality folds by
# 2-3x, which is the scan and not the fold, and *wins* agg-by-session at
# 1.37x, because PostgreSQL's aggregate degrades with group count about 10x
# harder than this one does (+454% against +46% from 1 group to 7,560). The
# prediction is left here, corrected, because the reasoning that produced it
# - "a planned operator must win by more as the work grows" - is the obvious
# one and worth having on record as wrong.
#
# Note what the two engines do *not* agree on and why it does not matter
# here: ckdbs emits groups in first-seen order and PostgreSQL in whatever
# order its aggregate produced them. This phase measures latency, not row
# order, and neither engine was asked to sort.

def run_aggregate_phases(client, suffix, session_count, ops, rng, phases):
    # A full scan per execution, so these run at the reduced count the other
    # whole-relation shapes use rather than at `ops`.
    scans = max(1, ops // 20)

    # SUM over `volume` cannot overflow at this scale and that is worth
    # knowing rather than discovering: volume is drawn under 50,000,000 and
    # there are ~60,480 bars, so the sum tops out near 3e15 against an
    # int64 accumulator's 9.2e18. A `SUM` that crossed it would fail the
    # statement rather than wrap (docs/spec/aggregate.md §3.3), which would
    # show up here as an error count, not a wrong number.
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

    # `read-day-slice`'s statement with a fold on top - the same relation
    # and the same predicate, so the pair is a controlled A/B and the only
    # one in this table. Read the two rows together.
    phase = Phase("agg-day-slice", "one session's cross section, grouped")
    for _ in range(ops):
        client.timed(f"SELECT symbol_id, COUNT(*), SUM(ret_bp) "
                     f"FROM daily_stats_{suffix} "
                     f"WHERE session_no = {rng.randrange(session_count)} "
                     f"GROUP BY symbol_id", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)

    # COUNT(DISTINCT) keeps an observed-value set per (group, item) over the
    # same encoding the group key uses. Eight distinct symbols over 60,480
    # rows, so all but eight probes are hits - which allocate nothing.
    phase = Phase("agg-distinct", "COUNT(DISTINCT symbol_id) over the run")
    for _ in range(scans):
        client.timed(f"SELECT COUNT(DISTINCT symbol_id) "
                     f"FROM daily_bars_{suffix}", phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)


def run_per_model_reads(client, suffix, models, rounds, phases):
    """The per-model comparison read, `rounds` times over the eight models.

    More than once deliberately. A Cabin observes a value on its first
    equality selection and can only *serve* the second, so a phase that
    reads each `model_id` exactly once measures the observation cost and
    none of the benefit - it reports all misses under --cabin and looks like
    the feature does nothing. Eight values against any budget means every
    round after the first is a hit.
    """
    phase = Phase("compare-one",
                  f"one model's history joined to its model, x{rounds}")
    for _ in range(rounds):
        for model_id, _name, _family, _lookback, _param in models:
            client.timed(compare_one_sql(suffix, model_id), phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)


def run_replay(client, suffix, session_count, rebalance, passes, phases):
    """Re-issues the backtest's cross-section join over the sessions it has
    already visited, `passes` more times.

    This is the research loop, not a synthetic repeat: re-running a strategy
    over the same window is what a backtest is *for*, and the first walk
    forward is the one time each session is read exactly once. It is also
    the only phase in which a Cabin on `daily_stats.session_no` can hit, for
    the reason run_per_model_reads() states - and it is where the budget
    shows: 30 years is ~7,560 distinct sessions against a
    `cabin_max_values` default of 4,096, so a full-length run replays a
    served prefix and a walking remainder.
    """
    if passes < 1:
        return
    phase = Phase("backtest-replay",
                  f"the same cross-sections re-read, x{passes}")
    for _ in range(passes):
        for session_no in range(0, session_count, rebalance):
            client.timed(cross_section_sql(suffix, session_no), phase)
    phase.elapsed = sum(phase.latencies)
    phases.append(phase)


# ---- the QPS sweep -------------------------------------------------------
#
# The point of the tool. Everything above builds a data set with a shape
# worth querying - 30 years of daily bars, their features, and a result
# relation written by eight models - and this measures **queries per second
# for each read shape under each case**, on that one data set, in one
# process.
#
# One process and one data set matter. Two runs of the whole scenario
# compared against each other differ by their page cache, their file layout,
# whatever else the machine was doing, and the load they each just
# performed. Here the cases are switched *between* measurements over
# identical bytes, which is what makes the columns of the matrix comparable
# to each other rather than merely printed side by side.
#
# The four cases:
#
#   cold      every statement uses an argument no statement has used before.
#             No page is warm for it, no Waystone trail names it, no Cabin
#             has observed it. The honest number for a query arriving out of
#             nowhere, and the slowest of the four by construction.
#   warm      the same small set of arguments, cycled. Pages stay resident,
#             the probe memo and any Waystone trail apply, and this is what
#             a dashboard refreshing on a fixed filter actually sees.
#   cabin     `warm` with a Cabin declared on the shape's filter column and
#             warmed once so its values are observed. An observed value's
#             rows are served without walking the relation.
#   dropped   `warm` again, immediately after DROP CABIN. It exists to show
#             the number goes *back* - un-observing is always legal, and a
#             structure whose removal is a performance event and never a
#             correctness one is the whole claim Cabin makes.
#
# A shape whose filter is the primary key gets no cabin case: a cabin on the
# pk column is refused, because the clustered tree already is one.


class Shape:
    """One read shape: how to build its statement, and what it filters on.

    `cabin_on` is (relation, column) when the shape's filter column can
    carry a Cabin, and None when it cannot - a pk equality, a pk range, or a
    join anchored on a pk. `keys` is the callable that draws an argument.
    """

    def __init__(self, name, build, keys, cabin_on=None, detail=""):
        self.name = name
        self.build = build
        self.keys = keys
        self.cabin_on = cabin_on
        self.detail = detail


def build_shapes(suffix, symbols, session_count, bar_count, model_ids):
    """The read shapes measured by the sweep, in the order they are printed.

    Deliberately the same statements the phases above time, not variations
    on them: a sweep that measures something the workload never issues is a
    microbenchmark wearing a scenario's schema.
    """
    symbol_ids = [symbol_id for symbol_id, _, _ in symbols]

    return [
        Shape("bar-lookup",
              lambda key: f"SELECT * FROM daily_bars_{suffix} WHERE id = {key}",
              lambda rng: rng.randint(1, max(1, bar_count)),
              None, "pk equality, a btree descent"),
        Shape("bar-range",
              lambda key: (f"SELECT * FROM daily_bars_{suffix} "
                           f"WHERE id BETWEEN {key} AND {key + 200}"),
              lambda rng: rng.randint(1, max(1, bar_count - 200)),
              None, "pk BETWEEN, 200 wide - a Range step"),
        Shape("day-slice",
              lambda key: (f"SELECT * FROM daily_stats_{suffix} "
                           f"WHERE session_no = {key}"),
              lambda rng: rng.randrange(session_count),
              ("daily_stats", "session_no"),
              "one session's features - a FilterScan"),
        Shape("symbol-history",
              lambda key: (f"SELECT * FROM daily_stats_{suffix} "
                           f"WHERE symbol_id = {key}"),
              lambda rng: rng.choice(symbol_ids),
              ("daily_stats", "symbol_id"),
              "one symbol's whole 30 years - a FilterScan"),
        Shape("cross-join",
              lambda key: cross_section_sql(suffix, key),
              lambda rng: rng.randrange(session_count),
              ("daily_stats", "session_no"),
              "the backtest read - FilterScan + Probe + Probe"),
        Shape("point-join",
              lambda key: (f"SELECT b.close, s.ticker, e.code "
                           f"FROM daily_bars_{suffix} AS b "
                           f"JOIN symbols_{suffix} AS s ON b.symbol_id = s.id "
                           f"JOIN exchanges_{suffix} AS e "
                           f"ON s.exchange_id = e.id WHERE b.id = {key}"),
              lambda rng: rng.randint(1, max(1, bar_count)),
              None, "bar -> symbol -> exchange, anchored on a pk"),
        Shape("model-join",
              lambda key: compare_one_sql(suffix, key),
              lambda rng: rng.choice(model_ids),
              ("model_results", "model_id"),
              "one model's results joined to its model"),
    ]


def measure_qps(client, statements):
    """Sends every statement and returns (qps, mean_us, p50_us, errors).

    The statements are built by the caller and passed as a list, so string
    formatting is outside the timed region: this measures the server and the
    socket, never Python's f-strings.
    """
    errors = 0
    latencies = []
    started = time.perf_counter()
    for statement in statements:
        t0 = time.perf_counter()
        reply = client(statement)
        latencies.append(time.perf_counter() - t0)
        if reply.startswith("ERR"):
            errors += 1
    elapsed = time.perf_counter() - started
    ordered = sorted(latencies)
    p50 = ordered[max(0, (len(ordered) - 1) // 2)] if ordered else 0.0
    return (len(latencies) / elapsed if elapsed > 0 else 0.0,
            statistics.fmean(latencies) * 1e6 if latencies else 0.0,
            p50 * 1e6, errors)


def sweep_shape(client, suffix, shape, ops, warm_keys, rng):
    """One row of the matrix: this shape's QPS in each of the four cases."""
    row = {"shape": shape.name, "detail": shape.detail}

    # cold: distinct arguments, drawn without replacement so no value is
    # ever seen twice. A pool smaller than `ops` (symbol-history has only
    # --symbols values, model-join only eight) is reported as a shorter run
    # rather than padded with repeats, which would make it a warm case
    # wearing a cold label.
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

    # warm: a small fixed set, cycled. Drawn fresh rather than reused from
    # the cold pool, because a value the cold pass just touched is already
    # half warm and the two cases would differ by less than they should.
    keys = [shape.keys(rng) for _ in range(max(1, warm_keys))]
    warm = [shape.build(keys[i % len(keys)]) for i in range(ops)]
    row["warm"] = measure_qps(client, warm)
    row["warm_ops"] = len(warm)

    if shape.cabin_on is None:
        row["cabin"] = None
        row["dropped"] = None
        row["cabin_note"] = "pk - a cabin on the pk column is refused"
        return row

    relation, column = shape.cabin_on
    reply = client(f"CREATE CABIN ON {relation}_{suffix}({column})")
    if reply.startswith("ERR"):
        row["cabin"] = None
        row["dropped"] = None
        row["cabin_note"] = reply[:60]
        return row

    # One warming pass so the values are observed - a declared Cabin
    # observes on the *first* equality read, so measuring without this would
    # price the observation and call it the benefit.
    for statement in warm[:len(keys)]:
        client(statement)
    row["cabin"] = measure_qps(client, warm)

    drop = client(f"DROP CABIN ON {relation}_{suffix}({column})")
    row["dropped"] = (measure_qps(client, warm)
                      if not drop.startswith("ERR") else None)
    row["cabin_note"] = f"{relation}.{column}"
    return row


def run_qps_sweep(client, suffix, symbols, session_count, bar_count,
                  model_ids, ops, warm_keys, rng):
    shapes = build_shapes(suffix, symbols, session_count, bar_count, model_ids)
    rows = []
    for shape in shapes:
        print(f"  sweeping {shape.name}...", flush=True)
        rows.append(sweep_shape(client, suffix, shape, ops, warm_keys, rng))
    return rows


CKDBS_CASE_NOTES = (
    "  cold    every argument seen for the first time: no warm page,",
    "          no Waystone trail, no observed Cabin value.",
    "  warm    the same few arguments cycled - a dashboard on a fixed",
    "          filter. warm/cold is what repetition alone buys.",
    "  cabin   warm, with a Cabin on the filter column, observed. It",
    "          is `-` where the filter is the primary key, because a",
    "          cabin on the pk column is refused: the clustered tree",
    "          already is one.",
    "  dropped warm again straight after DROP CABIN. It should return",
    "          to the warm column - un-observing is always legal, and",
    "          a Cabin's removal is a performance event, never a",
    "          correctness one.",
)


def print_qps_matrix(rows, ops, warm_keys, accel="cabin",
                     notes=CKDBS_CASE_NOTES):
    """The matrix. `accel` names the third case - the engine's own
    accelerator for a non-pk equality, created and dropped at runtime.

    Parameterized rather than hard-coded because the PostgreSQL baseline
    (tools/pg_scenario1_backtest.py) prints the identical table with a btree
    `index` in that column. The two are structurally the same experiment -
    build it, measure, drop it, measure again - and printing them through
    one function is what keeps the columns aligned when they are read side
    by side.
    """
    print()
    print(f"QPS by read shape and case - {ops} statements per cell, "
          f"{warm_keys} distinct arguments in the warm cases")
    header = (f"{'shape':<16}{'cold':>11}{'warm':>11}{accel:>11}"
              f"{'dropped':>11}{'warm/cold':>11}{accel + '/warm':>12}   notes")
    print(header)
    print("-" * (len(header) + 24))
    for row in rows:
        def qps(case):
            return row[case][0] if row.get(case) else None

        def cell(case):
            value = qps(case)
            return f"{value:>11,.0f}" if value is not None else f"{'-':>11}"

        cold, warm, cabin = qps("cold"), qps("warm"), qps("cabin")
        warm_ratio = f"{warm / cold:>10.2f}x" if cold and warm else f"{'-':>11}"
        cabin_ratio = (f"{cabin / warm:>11.2f}x" if warm and cabin
                       else f"{'-':>12}")
        note = row.get("cabin_note", "")
        print(f"{row['shape']:<16}{cell('cold')}{cell('warm')}{cell('cabin')}"
              f"{cell('dropped')}{warm_ratio}{cabin_ratio}   {note}")

    print()
    for row in rows:
        errors = sum(row[c][3] for c in ("cold", "warm", "cabin", "dropped")
                     if row.get(c))
        if errors:
            print(f"  {row['shape']}: {errors} error replies during the sweep")
        if row["cold_ops"] < ops:
            print(f"  {row['shape']}: cold ran {row['cold_ops']} of {ops} - "
                  f"the shape has fewer distinct arguments than that, and "
                  f"repeating one would have made it a warm case")
    print()
    for line in notes:
        print(line)


# ---- write QPS -----------------------------------------------------------

def run_write_sweep(client, suffix, batches, ops):
    """Insert QPS against transaction batch size, on a relation of its own.

    A relation of its own rather than more rows in `daily_bars`: the sweep
    writes tens of thousands of rows for no reason but to time them, and
    putting them in the relation every read shape above measures would
    change the data set halfway through the report.

    Batch size is the largest single lever on write throughput here. Each
    statement is one round trip either way; what changes is how many
    durability points they cost - four autocommit statements are four, one
    transaction of four is one (`docs/spec/txn.md`).
    """
    table = f"write_probe_{suffix}"
    reply = client(f"CREATE TABLE {table} (id int64, a int64, b int64, "
                   f"c int64, d int64) HEAP")
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
            got = client(f"INSERT INTO {table} VALUES "
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


def print_write_sweep(results):
    if not results:
        return
    print()
    print("INSERT QPS by transaction batch size")
    header = f"{'rows/transaction':<20}{'ops':>9}{'rows/s':>12}{'vs autocommit':>16}{'err':>6}"
    print(header)
    print("-" * len(header))
    base = next((r["qps"] for r in results if r["batch"] in (0, 1)), None)
    for r in results:
        label = "1 (autocommit)" if r["batch"] in (0, 1) else f"{r['batch']:,}"
        ratio = f"{r['qps'] / base:>15.2f}x" if base else f"{'-':>16}"
        print(f"{label:<20}{r['ops']:>9,}{r['qps']:>12,.0f}{ratio}"
              f"{r['errors']:>6}")
    print()
    print("  every row is one round trip in every case; what changes is how")
    print("  many durability points they cost. A batch of one is a batch.")


# ---- concurrent QPS ------------------------------------------------------

def run_concurrency_sweep(host, port, timeout, suffix, session_count,
                          rebalance, counts, ops):
    """Aggregate QPS for the backtest's join at several connection counts.

    Threads rather than processes: each one blocks on a socket for
    essentially its whole life, so the GIL is not the thing being measured -
    the server is. Each thread owns its own connection, because the wire
    protocol is one line in, one line out and a shared socket would
    serialize them into a single client anyway.

    The number to read is aggregate throughput against connection count, not
    per-connection latency: the server dispatches a core's statements on one
    thread, so this measures how much of the round trip was the client
    waiting rather than the engine working.
    """
    import threading

    sessions = list(range(0, session_count, rebalance)) or [0]
    results = []

    for count in counts:
        per_thread = max(1, ops // count)
        errors = [0] * count
        clients = []
        try:
            for _ in range(count):
                clients.append(ServerConnection(host, port, timeout=timeout))
        except OSError as e:
            print(f"  concurrency {count}: could not open connections: {e}")
            for c in clients:
                c.close()
            break

        def worker(index):
            conn = clients[index]
            for i in range(per_thread):
                session_no = sessions[(index * per_thread + i) % len(sessions)]
                reply = format_reply(
                    conn.send_command(cross_section_sql(suffix, session_no)))
                if reply.startswith("ERR"):
                    errors[index] += 1

        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(count)]
        started = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        elapsed = time.perf_counter() - started
        for c in clients:
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


CKDBS_CONCURRENCY_NOTES = (
    "  the server dispatches one core's statements on one thread, so",
    "  a rising aggregate here is round-trip overlap, not parallel",
    "  execution. Where it stops rising is where the client stopped",
    "  being the bottleneck.",
)


def print_concurrency_sweep(results, notes=CKDBS_CONCURRENCY_NOTES):
    """`notes` is parameterized because this is the one table whose reading
    genuinely differs between the two engines: ckdbs runs a core's
    statements on one thread, PostgreSQL runs a backend process per
    connection. The numbers are comparable; the explanation is not."""
    if not results:
        return
    print()
    print("QPS by connection count - the backtest's 3-relation join")
    header = (f"{'connections':<14}{'ops':>9}{'q/s':>12}{'vs 1 conn':>12}"
              f"{'per conn':>11}{'err':>6}")
    print(header)
    print("-" * len(header))
    base = results[0]["qps"] if results else 0
    for r in results:
        ratio = f"{r['qps'] / base:>11.2f}x" if base else f"{'-':>12}"
        print(f"{r['connections']:<14}{r['ops']:>9,}{r['qps']:>12,.0f}{ratio}"
              f"{r['qps'] / r['connections']:>11,.0f}{r['errors']:>6}")
    print()
    for line in notes:
        print(line)


# ---- ANALYZE -------------------------------------------------------------

STEP_LINE = re.compile(r"^step (\d+) (\w+) (\S+)")


def plan_of(client, sql):
    """The step chain and examined-row count the engine reports for one
    statement.

    Printed beside the timings because a phase can measure the wrong thing
    while looking entirely plausible: a `Probe` step against a heap relation
    walks the whole chain, so the cross-section join collapses onto a cross
    product and the table shows two believable numbers. `examined` is what
    makes that visible instead of assumed.

    ANALYZE's reply repeats each step - once as the plan, once with per-step
    statistics - so the stats half is identified by its `opens=` field
    rather than by counting, which would break the first time the format
    grows a line.
    """
    reply = client("ANALYZE " + sql)
    if reply.startswith("ERR"):
        return ("?", 0, 0)
    sections = reply.split("\n")
    examined = re.search(r"examined=(\d+)", sections[0])
    rows = re.search(r"rows=(\d+)", sections[0])
    kinds = []
    for line in sections[1:]:
        match = STEP_LINE.match(line.strip())
        if match and "opens=" not in line:
            kinds.append(f"{match.group(2)}")
    return ("+".join(kinds) if kinds else "?",
            int(examined.group(1)) if examined else 0,
            int(rows.group(1)) if rows else 0)


def print_plans(client, suffix, models, session_count, bar_count):
    print()
    print("plans (ANALYZE)")
    header = f"{'statement':<22}{'steps':<34}{'rows':>10}{'examined':>12}"
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
        steps, examined, rows = plan_of(client, sql)
        print(f"{name:<22}{steps:<34}{rows:>10,}{examined:>12,}")
    print()
    print("  a Probe or Lookup step on a HEAP relation has no index to")
    print("  descend and walks the chain instead: `examined` far above")
    print("  `rows` is that, and is what --bars-clustered heap measures.")
    print("  read-bar-lookup is the clean read of it. On the *joins* it can")
    print("  be hidden: these statements have run hundreds of times by now,")
    print("  so their lookup-class steps may be served from a Waystone trail")
    print("  (docs/spec/waystone-concpets.md) - a recorded (page, slot) instead")
    print("  of a search, which is exactly what a trail is for and is why an")
    print("  ANALYZE taken after a warm run is not a cold plan.")


# ---- verification --------------------------------------------------------

def verify(state, by_model):
    """The in-memory P&L against the P&L read back through the join.

    They must agree exactly - integer basis points on both sides, with the
    database holding the same number the driver accumulated. A mismatch is a
    round-trip failure (a row not written, a row not returned, a value
    truncated), never a rounding one.
    """
    problems = []
    for model_id, model in state.items():
        stored = by_model.get(model_id)
        if stored is None:
            problems.append(f"{model['name']}: no rows came back from the "
                            f"comparison join")
            continue
        if stored["equity_bp"] != model["equity_bp"]:
            problems.append(
                f"{model['name']}: equity {stored['equity_bp']} bp read back, "
                f"{model['equity_bp']} bp accumulated")
        if len(stored["pnl"]) != len(model["periods"]):
            problems.append(
                f"{model['name']}: {len(stored['pnl'])} periods read back, "
                f"{len(model['periods'])} written")
        # The cumulative column against the sum of the per-period column,
        # both read back from the same rows. They are written by the same
        # statement and are redundant on purpose: a row that was written but
        # not returned shows up here and nowhere else, because the running
        # total is carried on every row and the periods are not.
        if sum(stored["pnl"]) != stored["equity_bp"]:
            problems.append(
                f"{model['name']}: the periods read back sum to "
                f"{sum(stored['pnl'])} bp, but the latest row's cumulative "
                f"equity is {stored['equity_bp']} bp - a result row is "
                f"missing from the join")
    return problems


# ---- reporting -----------------------------------------------------------

def print_comparison(by_model, periods_per_year, full):
    """The eight models, scored from the rows the comparison join returned.

    Printed compactly unless --show-models. The ranking is a property of
    --seed on a random walk and says nothing about strategies in a market -
    the models exist to make the comparison *join* worth issuing, and it is
    the QPS of that join that this tool measures. What is always printed is
    the family summary, because it is one line, and the --verify verdict,
    because it is the correctness gate on every read above it.
    """
    scored = []
    for model_id, entry in by_model.items():
        scored.append((model_id, entry, score(entry)))
    scored.sort(key=lambda item: item[2]["total_bp"], reverse=True)

    if full:
        print()
        print(f"model comparison - {len(scored)} models, "
              f"{periods_per_year:.1f} rebalance periods per year")
        header = (f"{'#':>3} {'model':<12}{'family':<12}{'total bp':>11}"
                  f"{'mean bp':>10}{'stdev':>9}{'best':>9}{'worst':>9}"
                  f"{'win%':>7}{'risk-adj':>10}{'trades':>9}")
        print(header)
        print("-" * len(header))
        for rank, (_model_id, entry, s) in enumerate(scored, 1):
            family = FAMILY_NAME.get(entry["family"], "?")
            # Annualized by the square root of the period count, which is
            # the only defensible scaling for a ratio of a mean to a
            # standard deviation over independent periods.
            annual = s["risk_adj"] * math.sqrt(periods_per_year)
            print(f"{rank:>3} {entry['name']:<12}{family:<12}"
                  f"{s['total_bp']:>11,}{s['mean_bp']:>10.1f}"
                  f"{s['stdev_bp']:>9.1f}{s['best_bp']:>9,}{s['worst_bp']:>9,}"
                  f"{s['win_rate'] * 100:>6.1f}%{annual:>10.2f}"
                  f"{s['trades']:>9,}")
        print()
        print("  risk-adj is mean/stdev of period P&L, annualized - an")
        print("  information ratio, not a Sharpe: this schema has no")
        print("  risk-free rate and inventing one would describe the driver.")

    # Which family won, since that is the one comparison the model names
    # were chosen to make possible - and one line either way.
    by_family = {}
    for _model_id, entry, s in scored:
        by_family.setdefault(entry["family"], []).append(s["total_bp"])
    print()
    print(f"models: {len(scored)} scored from the comparison join over "
          f"{periods_per_year:.1f} periods/year"
          + ("" if full else "  (--show-models for the full table)"))
    for family, totals in sorted(by_family.items()):
        print(f"  {FAMILY_NAME.get(family, '?'):<12} "
              f"{len(totals)} models, total {sum(totals):,} bp, "
              f"best {max(totals):,} bp, worst {min(totals):,} bp")
    return [
        {"model_id": model_id, "name": entry["name"],
         "family": FAMILY_NAME.get(entry["family"], "?"), **s}
        for model_id, entry, s in scored
    ]


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"default: {DEFAULT_PORT}")
    parser.add_argument("--suffix", default=None,
                        help="table-name suffix; default <epoch>_<rand>, i.e. "
                             "fresh relations per run (there is no DROP TABLE)")

    parser.add_argument("--years", type=int, default=30,
                        help="years of daily history (default: 30), at "
                             f"{SESSIONS_PER_YEAR} business days each")
    parser.add_argument("--symbols", type=int, default=8,
                        help="instruments (default: 8). Bars ingested is "
                             "--years x 252 x --symbols, and the feature "
                             "relation is written one row per bar, so this "
                             "is the knob that sets the load's length")
    parser.add_argument("--exchanges", type=int, default=2,
                        help="venues (default: 2)")
    parser.add_argument("--start-year", type=int, default=1995,
                        help="first year of the calendar (default: 1995)")

    parser.add_argument("--rebalance", type=int, default=21,
                        help="sessions between rebalances (default: 21, "
                             "about monthly). Each one is a 3-relation join "
                             "read plus 8 result inserts")
    parser.add_argument("--top-k", type=int, default=3,
                        help="positions each ranking model holds (default: 3)")

    parser.add_argument("--bars-clustered", choices=("btree", "heap"),
                        default="btree",
                        help="storage for daily_bars (default: btree). It is "
                             "probed by primary key once per feature row in "
                             "every backtest read; on a heap relation that "
                             "probe has no index and walks the chain, which "
                             "is exactly what running this as `heap` "
                             "measures. --fk requires btree")

    parser.add_argument("--batch", type=int, default=200,
                        help="rows per BEGIN/COMMIT during the load "
                             "(default: 200). 0 sends every insert as its "
                             "own transaction")
    parser.add_argument("--no-load-txn", dest="batch", action="store_const",
                        const=0,
                        help="the same as --batch 0: one durability point "
                             "per row, which is the pre-transaction baseline")

    parser.add_argument("--ops", type=int, default=200,
                        help="operations per individual read phase "
                             "(default: 200); the two full-relation phases "
                             "run a twentieth of it")
    parser.add_argument("--replay", type=int, default=1,
                        help="extra passes of the backtest's cross-section "
                             "join over the sessions it already walked "
                             "(default: 1). Re-running a strategy over the "
                             "same window is the research loop, and it is "
                             "the only phase where a Cabin on session_no can "
                             "hit: the walk forward reads each session once, "
                             "which observes every value and serves none. 0 "
                             "skips it")
    parser.add_argument("--compare-rounds", type=int, default=4,
                        help="passes of the per-model comparison read "
                             "(default: 4, i.e. 32 statements over 8 "
                             "models). Same reason as --replay: the first "
                             "pass observes, the rest are what a Cabin on "
                             "model_id serves")

    parser.add_argument("--cabin", dest="cabin", action="store_true",
                        default=False,
                        help="declare Cabins on daily_stats.session_no and "
                             "model_results.model_id (docs/spec/cabin.md): "
                             "the two columns every FilterScan here filters "
                             "on. Default off, so the baseline stays what it "
                             "has always been - run it both ways and compare "
                             "backtest-read and compare-one. It is not free "
                             "on the write side: every insert into those "
                             "relations pays a directory probe")
    parser.add_argument("--no-cabin", dest="cabin", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--fk", dest="fk", action="store_true", default=False,
                        help="declare the four foreign keys this data "
                             "already satisfies (docs/spec/foreign-keys.md). "
                             "Every ingested row then probes its parents "
                             "before it is written; nothing here deletes, so "
                             "the reverse check never fires. Requires "
                             "--bars-clustered btree")
    parser.add_argument("--no-fk", dest="fk", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--sweep", dest="sweep", action="store_true",
                        default=True,
                        help="measure QPS for every read shape in each of "
                             "four cases - cold, warm, with a Cabin, and "
                             "after dropping it - on one data set in one "
                             "process (default: on). This is the tool's "
                             "headline table; the backtest above it exists "
                             "to build something worth querying")
    parser.add_argument("--no-sweep", dest="sweep", action="store_false",
                        help="skip the QPS matrix")
    parser.add_argument("--aggregates", dest="aggregates",
                        action="store_true", default=True,
                        help="time the GROUP BY rollups beside the read "
                             "shapes (default: on). Each is priced against "
                             "the unaggregated statement above it, and the "
                             "set spans the group-count range that decides "
                             "a fold's cost (docs/spec/aggregate.md)")
    parser.add_argument("--no-aggregates", dest="aggregates",
                        action="store_false",
                        help="skip the aggregate phases - needed against a "
                             "server older than the aggregation work, where "
                             "GROUP BY is a syntax error")
    parser.add_argument("--qps-ops", type=int, default=100,
                        help="statements per cell of the QPS matrix "
                             "(default: 100). The matrix has 22 cells and "
                             "the heavy shapes dominate it: at 30 years a "
                             "cold cross-join is ~390ms a statement, so this "
                             "is the knob that decides whether the sweep "
                             "takes two minutes or twenty. Raise it to "
                             "tighten the numbers, not to change them")
    parser.add_argument("--warm-keys", type=int, default=8,
                        help="distinct arguments cycled in the warm, cabin "
                             "and dropped cases (default: 8). Raising it "
                             "moves those cases toward cold, which is the "
                             "point of the knob: repetition is the variable")
    parser.add_argument("--write-sweep", dest="write_sweep",
                        action="store_true", default=True,
                        help="measure INSERT QPS against transaction batch "
                             "size, on a relation of its own (default: on)")
    parser.add_argument("--no-write-sweep", dest="write_sweep",
                        action="store_false", help="skip it")
    parser.add_argument("--write-batches", default="1,10,100,1000",
                        help="comma-separated batch sizes for the write "
                             "sweep (default: 1,10,100,1000). 1 is "
                             "autocommit")
    parser.add_argument("--write-ops", type=int, default=2000,
                        help="rows inserted per batch size (default: 2000)")
    parser.add_argument("--connections", default="1,2,4,8",
                        help="comma-separated connection counts for the "
                             "concurrency sweep (default: 1,2,4,8); empty "
                             "skips it")
    parser.add_argument("--conn-ops", type=int, default=200,
                        help="join statements spread across the connections "
                             "at each count (default: 200)")

    parser.add_argument("--show-models", action="store_true",
                        help="print the full eight-model comparison table. "
                             "Off by default: the ranking is a property of "
                             "--seed on a random walk, and the backtest is "
                             "here to generate a workload, not a result. The "
                             "one-line family summary and the --verify "
                             "verdict are always printed")

    parser.add_argument("--analyze", action="store_true",
                        help="print the step chain and examined-row count "
                             "ANALYZE reports for each read shape. Worth it "
                             "the first time: it is how a Probe that "
                             "silently became a chain scan shows up")
    parser.add_argument("--verify", dest="verify", action="store_true",
                        default=True,
                        help="check every model's P&L read back through the "
                             "comparison join against the driver's own "
                             "running total (default: on)")
    parser.add_argument("--no-verify", dest="verify", action="store_false",
                        help="skip the round-trip check")

    parser.add_argument("--echo", action="store_true",
                        help="print every statement and reply to stderr. Off "
                             "by default: it costs a write per statement on "
                             "a tool that sends a hundred thousand of them")
    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed (default: 1). Prices are generated "
                             "from it, so two runs with the same seed ingest "
                             "byte-identical data - which is what makes a "
                             "--cabin run comparable to a plain one")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="socket timeout in seconds (default: 300); one "
                             "symbol's whole history is a single slow reply")
    parser.add_argument("--sync", action="store_true",
                        help="send SYNC after the run and time it")
    parser.add_argument("--json", metavar="PATH",
                        help="also write results as JSON")
    parser.add_argument("--server-log", metavar="PATH",
                        help="the server's log at --log-level debug: adds its "
                             "own per-statement microseconds, which is the "
                             "only view of engine cost separate from this "
                             "client's round trip")
    args = parser.parse_args()

    set_echo(args.echo)

    if args.years < 1:
        abort("--years must be at least 1")
    if args.symbols < 1:
        abort("--symbols must be at least 1")
    if args.rebalance < 1:
        abort("--rebalance must be at least 1")
    if args.top_k < 1:
        abort("--top-k must be at least 1")
    if args.compare_rounds < 1:
        abort("--compare-rounds must be at least 1: the comparison read is "
              "how the models are scored, not an optional phase")
    if args.sweep and args.cabin:
        abort("--sweep and --cabin are mutually exclusive: the sweep creates "
              "and drops its own Cabins to measure the `cabin` and `dropped` "
              "columns, and a column declared CABIN at CREATE TABLE already "
              "carries one, so CREATE CABIN on it is refused.\n  Use --sweep "
              "for the A/B, or --cabin --no-sweep for a whole run with "
              "Cabins declared up front.")

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
    if args.fk and args.bars_clustered != "btree":
        abort("--fk needs --bars-clustered btree: daily_stats.bar_id "
              "references daily_bars, and a foreign key references the "
              "parent's primary key.\n  A heap relation has no pk index, so "
              "the declaration is refused rather than made slow.")

    session_count = args.years * SESSIONS_PER_YEAR
    bar_total = session_count * args.symbols
    suffix = (args.suffix
              or f"{time.time_ns() // 1_000_000_000}_{random.randrange(1 << 16)}")
    rng = random.Random(args.seed)

    client = Client(args.host, args.port, args.timeout)

    print(f"loading: {args.years} years x {SESSIONS_PER_YEAR} sessions x "
          f"{args.symbols} symbols = {bar_total:,} bars, "
          f"{bar_total:,} feature rows  (tables suffixed _{suffix})"
          + (f"  [daily_bars {args.bars_clustered.upper()}]")
          + ("  [cabins on session_no, model_id]" if args.cabin else "")
          + ("  [4 foreign keys]" if args.fk else "")
          + (f"  [{args.batch}-row transactions]" if args.batch else
             "  [one transaction per row]"),
          flush=True)

    create_tables(client, suffix, args.cabin, args.fk, args.bars_clustered)

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

    # ---- the backtest ----------------------------------------------------
    run_phases = []
    print(f"backtesting: {len(models)} models over "
          f"{len(session_ids) // args.rebalance:,} rebalance periods "
          f"({args.rebalance} sessions apart)", flush=True)
    t_run = time.perf_counter()
    state, periods = run_backtest(client, suffix, models, len(session_ids),
                                  args.rebalance, args.top_k, run_phases)
    run_elapsed = time.perf_counter() - t_run
    print(f"  {periods:,} periods scored in {run_elapsed:.1f}s", flush=True)

    # ---- the comparison --------------------------------------------------
    run_replay(client, suffix, len(session_ids), args.rebalance, args.replay,
               run_phases)
    by_model = read_back_results(client, suffix, run_phases)
    run_per_model_reads(client, suffix, models, args.compare_rounds,
                        run_phases)

    # ---- the individual read shapes --------------------------------------
    read_phases = []
    run_read_phases(client, suffix, symbols, len(session_ids), bar_total,
                    args.ops, rng, read_phases)
    if args.aggregates:
        run_aggregate_phases(client, suffix, len(session_ids), args.ops, rng,
                             read_phases)

    # ---- the QPS sweeps --------------------------------------------------
    #
    # After every phase above, deliberately: the sweep's `cold` case needs
    # arguments nothing has touched, and running it first would leave the
    # rest of the report measuring a warmed relation.
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
            args.host, args.port, args.timeout, suffix, len(session_ids),
            args.rebalance, connection_counts, args.conn_ops)

    sync_phase = None
    if args.sync:
        sync_phase = Phase("sync", "SYNC after the run")
        client.timed("SYNC", sync_phase)
        sync_phase.elapsed = sum(sync_phase.latencies)

    problems = verify(state, by_model) if args.verify else []

    # ---- report ----------------------------------------------------------
    phases = load_phases + run_phases + read_phases
    if sync_phase:
        phases.append(sync_phase)

    periods_per_year = SESSIONS_PER_YEAR / args.rebalance
    meta = {
        "engine": "ckdbs",
        "scenario": "backtest",
        "columns": sum(len(SCHEMA[base][0].split(",")) for base in SCHEMA),
        "rows": bar_total * 2 + len(session_ids) + len(symbols) + len(MODELS),
        "host": args.host,
        "port": args.port,
        "table": f"7 relations, suffix _{suffix}",
        "clustered": args.bars_clustered,
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
        "cabin": args.cabin,
        "fk": args.fk,
        "seed": args.seed,
        # Which accelerator the sweep's third column measured. Read by
        # tools/compare_scenario1.py so it can label the column without
        # knowing which engine wrote the file.
        "accelerator": "cabin",
        "durability": read_durability(args.server_log) if args.server_log
                      else None,
    }

    report(phases, meta, footer=(
        f"ingest: {bar_total:,} bars + {bar_total:,} feature rows, "
        f"{'batched ' + str(args.batch) + ' per transaction' if args.batch else 'one transaction per row'}",
        f"backtest-read is a 3-relation join: FilterScan(daily_stats) + "
        f"Probe(daily_bars) + Probe(symbols)",
        f"daily_bars is {args.bars_clustered.upper()}: a pk Probe "
        f"{'descends' if args.bars_clustered == 'btree' else 'has no index and walks the chain'}",
        "money and P&L are int64 basis points; float/decimal columns are "
        "refused at CREATE TABLE",
        "the model totals are computed client-side because the comparison "
        "phase prices a join, not a fold; the agg-* phases price the fold "
        "on its own, and means and deviations stay client-side because "
        "there is still no arithmetic in a select list",
        "this table is the workload as it ran, with tail latency; the QPS "
        "matrix below re-measures the same shapes as a controlled A/B. Both "
        "are here because throughput and p99 are different questions",
    ))

    # The QPS tables come first: they are what this tool is for. Everything
    # above is the workload that made them worth measuring.
    if sweep_rows:
        print_qps_matrix(sweep_rows, args.qps_ops, args.warm_keys)
    print_write_sweep(write_results)
    print_concurrency_sweep(concurrency_results)

    if args.analyze:
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

    if args.server_log:
        server_us = server_side_us(args.server_log, f"daily_bars_{suffix}")
        if server_us:
            print()
            print("  server-side microseconds, by statement kind:")
            for kind, values in sorted(server_us.items()):
                print(f"    {kind:<10} n={len(values):<8} "
                      f"mean={statistics.fmean(values):.1f}us")

    if args.json:
        meta["ranking"] = ranking
        meta["verify_problems"] = problems
        # The sweeps are the deliverable, so they go in the JSON in a form
        # a script can diff: one object per shape, each case a [qps, mean_us,
        # p50_us, errors] tuple or null where the case does not apply.
        def case_json(cell):
            # Rounded to the same precision bench_common uses, so a diff of
            # two JSON files shows changes and not float noise.
            if not cell:
                return None
            qps, mean_us, p50_us, errors = cell
            return {"qps": round(qps, 1), "mean_us": round(mean_us, 1),
                    "p50_us": round(p50_us, 1), "errors": errors}

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

    client.close()
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
