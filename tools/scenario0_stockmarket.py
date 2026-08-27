#!/usr/bin/env python3
"""Business-scenario stress test: a brokerage book on ckdbs, measured in TPS.

Where tools/benchmark.py prices one statement kind at a time against one
synthetic table, this drives a **five-relation OLTP workload** and reports
the number that a financial system is actually operated on: completed
business transactions per second, under concurrent periodic reporting.

The schema, five relations:

    users                  one row per customer
    accounts               many per user (the user->accounts fan-out)
    assets                 the instruments that get bought and sold
    trades                 append-only history; two legs per executed trade
    user_periodic_profit   written by the *reporting* process, not the traders

The measured transaction - the unit TPS counts - is one executed trade
between two accounts, and it is exactly the four statements asked for:

    INSERT INTO trades ...     the buy leg  (side=0, buyer's account)
    INSERT INTO trades ...     the sell leg (side=1, seller's account)
    UPDATE accounts SET ...    buyer:  balance down, asset_qty up
    UPDATE accounts SET ...    seller: balance up,   asset_qty down

A transaction counts only if all four statements replied without `ERR`.

Concurrently, a **separate process** (`--profit`, on by default) plays the
periodic reporting job: every `--profit-interval` seconds it wakes, reads
each sampled user's accounts with `SELECT * FROM accounts WHERE user_id =
<n>` - a non-pk equality, so a `FilterScan` step - and appends one
`user_periodic_profit` row per user per period. That is the contention the
scenario exists to create: an analytic full-relation walk running against a
point-lookup write workload, on a server that dispatches every client on
one thread.

Optionally (`--cabin`, off by default) that reporting query is given a
**Cabin** on `accounts.user_id` (docs/spec/cabin.md) - a store that is
authoritative for the values queries have actually observed. The reporter's
`WHERE user_id = <n>` is then served from an observed value's entry set
instead of walking the relation, and the two `UPDATE accounts` statements in
every transaction pay one in-memory directory probe each to keep that set
complete. Both halves are the measurement: a structure that speeds the
reader by slowing the writer is judged on TPS and `profit-scan` together,
which is why this scenario - a reader and a writer contending on one
relation - is where it belongs rather than in a read-only benchmark.

Run it both ways on a fresh data file and compare. The one thing that must
*not* move is the answers: a Cabin chooses where to look, never what is
visible, so the trades, the balances and the `--verify` verdict are
identical with it and without it.

Optionally (`--fk`, off by default) the relationship this data has always
had is **declared**: `trades.account_id REFERENCES accounts`
(docs/spec/foreign-keys.md). Every trade leg then probes the account it
names before the row is written, and a leg naming no account is refused
instead of stored. Referential integrity stops being a property of how this
driver generates ids and becomes one of the database.

Unlike the Cabin, this one is *supposed* to be able to change an answer -
that is what a constraint is - but not on this workload, where every id the
driver writes is one it created. So the `--verify` verdict and the balances
are identical with it and without it, and what moves is the trade-insert
latency: the price of the check. Both halves of the constraint exist in the
engine; only the forward one fires here, because nothing deletes an account.

Three things this tool is NOT, and cannot be, on today's engine:

1. **The four statements are not atomic by default** - `--txn` makes them
   so. Without it each statement is its own transaction, so a failure
   between statement 2 and 3 leaves a trade recorded against balances that
   were never moved; the tool counts those and prints them as `torn`. With
   it, the four run inside one BEGIN/COMMIT, a failure unwinds the whole
   thing, and `torn` is zero by construction.

   The default is off so the scenario stays comparable to every result
   recorded before transactions landed, and because the two modes measure
   different things. Grouping is **not** a free win to be assumed: it is
   worth about **4.7x** here (bench/results-txn-layers.md measures 4,712 us
   against 998 us) because four autocommit statements are four `fsync`s and
   one transaction is one - but it also holds a write transaction open
   across four round trips, which is where commit cost and inter-trader
   conflict start to matter. Run it both ways.
2. **Balances are computed client-side.** `UPDATE ... SET col = <val>`
   takes a *literal*, not an expression (parser.cpp's ParseUpdate ->
   ParseValue), so `balance = balance - notional` is not expressible. Each
   trader therefore owns a disjoint partition of accounts (`--traders`
   splits them) and keeps that partition's balances in memory. Disjoint
   partitions are what make that safe: two traders never touch one account,
   so no update is lost, and the in-memory number stays the truth. The
   `--verify` pass reads a sample back and compares.
3. **Money is int64 minor units** (cents), never a decimal. `float` and
   `decimal` columns are refused at CREATE TABLE - under the fixed-length
   rule a relation's row size is a schema constant, and their on-disk width
   is still an open decision (docs/spec/client-manual.md section 3).

Storage organization is chosen per relation, and the choices are the
measurement as much as the workload is:

    accounts, users, assets   BTREE - every access is `WHERE id = <n>`, and
                              on a heap relation that is a full chain scan,
                              which would make the update phase a function
                              of the account count rather than of the engine
    trades, user_periodic_profit
                              HEAP - insert-only, never read by pk here, so
                              a tail append is exactly right and a tree
                              would buy a descent and a split per row

Simulated time. `--days` (default 180) is a **business** span compressed
into the `--seconds` the run actually takes: a trade's `trade_day` and the
reporting job's period boundaries both derive from wall-clock progress
through the run. 180 days in 60 seconds means one simulated day every
333 ms; nothing sleeps to make that true.

Usage:
    # start a server first - Release build, scratch data file:
    #   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    #   cmake --build build-release -j
    #   ./build-release/kds_server /tmp/stress.db --port 15599 \
    #       --log-dir /tmp --log-file s.log --log-level debug
    python3 tools/scenario0_stockmarket.py --port 15599
    python3 tools/scenario0_stockmarket.py --users 10000 --assets 10000 --seconds 120
    python3 tools/scenario0_stockmarket.py --traders 4 --json out.json
    python3 tools/scenario0_stockmarket.py --no-profit      # OLTP alone, for the delta
    python3 tools/scenario0_stockmarket.py --cabin          # the same run, with a Cabin on
                                                      # accounts.user_id
    # the A/B, on two fresh data files - the numbers to compare are TPS and
    # the profit-scan p50, and the --verify verdict must match:
    python3 tools/scenario0_stockmarket.py --json plain.json
    python3 tools/scenario0_stockmarket.py --cabin --json cabin.json
    python3 tools/scenario0_stockmarket.py --fk --json fk.json
    python3 tools/scenario0_stockmarket.py --txn --json txn.json

Every run creates its own five tables, suffixed `_<epoch>_<rand>`, because
there is no DROP TABLE: a shared data file would otherwise accumulate
relations and the load phase would time a growing catalog. Use a scratch
data file and delete it between runs.
"""

import argparse
import multiprocessing
import queue
import random
import re
import sys
import time

from bench_common import Phase, report, write_json
from benchmark import read_durability, reply_rows, server_side_us
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply

# ---- schema --------------------------------------------------------------
#
# Column 0 of every relation is the Keystone primary key: system-generated,
# not supplied on INSERT (invariant 11). It is written out in each column
# list anyway because CREATE TABLE declares it; only INSERT omits it.
#
# These five relations spend 27 columns per run. That used to be a third of
# the instance's whole capacity - the catalog's pages did not chain, so
# `sys.columns` held ~68 rows and the third consecutive run on one data file
# failed - and it is now ~0.3% of it, since the catalog relations chain
# (docs/rules/keystoneid-k0-findings.md). The schema is left lean anyway: there is
# still no `notional` column on trades (it is qty * price) and no
# `unrealized` on the profit relation, because a derived column is a column
# to keep consistent for no answer it makes possible.

SCHEMA = {
    # BTREE: every account access in the transaction is `WHERE id = <n>`.
    "accounts": (
        "id int64, user_id int64, balance int64, asset_qty int64, "
        "trade_count int64, opened_day int32", "BTREE"),
    "users": (
        "id int64, name varchar, country varchar, tier int32, created_day int32", "BTREE"),
    "assets": (
        "id int64, symbol varchar, asset_class int32, last_price int64", "BTREE"),
    # HEAP: insert-only, appended at the chain tail, never probed by pk.
    "trades": (
        "id int64, account_id int64, asset_id int64, side int32, qty int64, "
        "price int64, trade_day int32", "HEAP"),
    "user_periodic_profit": (
        "id int64, user_id int64, period_day int32, realized int64, "
        "trade_count int64", "HEAP"),
}

# Creation order is load-bearing under `--fk` and cosmetic without it: a
# parent has to exist before a child may reference it, and there is no
# ALTER TABLE to add the constraint later. Without the flag, referential
# integrity here is a property of how the driver generates ids and nothing
# enforces it - which is the baseline `--fk` is measured against.
CREATE_ORDER = ("users", "assets", "accounts", "trades", "user_periodic_profit")

# ---- the foreign key (docs/spec/foreign-keys.md) -------------------------
#
# `--fk` declares one, on exactly this column:
FK_CHILD, FK_COLUMN, FK_PARENT = "trades", "account_id", "accounts"

# It is the relationship the scenario's data has always had and nothing
# enforced: every trade leg names the account it was executed against, and
# the driver generates that id, so referential integrity was a property of
# the *driver* rather than of the database. Declaring it moves that property
# into the engine, and the cost is what this flag exists to measure.
#
# **What it exercises is the forward check only.** Each `INSERT INTO trades`
# now probes `accounts` for the row it names (§2 of the spec) - a btree
# descent into the parent, plus the visibility test - and the workload never
# deletes an account, so the reverse check never runs. That is the honest
# shape of an insert-dominated OLTP workload, and it is the half that fires
# per transaction rather than per operator action.
#
# On ckdbs the parent must be a **BTREE** relation, which `accounts` already
# is: a foreign key references the parent's Keystone id, and a heap relation
# has no index to descend, so the declaration is refused rather than made to
# scan. The two `UPDATE accounts` statements pay nothing - accounts declares
# no outgoing key, and the SET lists touch no fk column.

# ---- the Cabin (docs/spec/cabin.md) --------------------------------------
#
# `--cabin` declares one, on exactly this column:
CABIN_RELATION, CABIN_COLUMN = "accounts", "user_id"

# It is the column the reporting job filters on, and the scenario is the one
# a Cabin exists for - a non-pk equality, on a foreign key, with the same
# values probed repeatedly. Without one, `WHERE user_id = <n>` is a
# `FilterScan`: a walk of the whole accounts relation, per user, per period.
# With one, the second read of a user is served from that value's entry set
# and reads only the rows that match.
#
# **What it costs is the other half of the measurement**, and this scenario
# is deliberately shaped to show it. A Cabin is authoritative for observed
# values, and the price of that authority is a witness on every write to the
# relation: each of the two `UPDATE accounts` statements per trade pays one
# in-memory directory probe. That is why the number to look at is not
# `profit-scan` alone but the TPS beside it - a structure that speeds up the
# reader by slowing the writer has to be judged on both.
#
# It is declared as a **column policy** (`user_id int64 CABIN`) rather than
# by a separate `CREATE CABIN`, for one reason worth stating: a declared
# Cabin observes a value on its *first* selection, where an engine-created
# one waits for the second. The reporter samples `--profit-users` users at
# random out of `--users`, so a given user is re-sampled rarely; at n=2 most
# of a short run would be spent counting first sightings and the measurement
# would be of the miss path.
#
# Everything else about the run is unchanged, and that is checkable: a Cabin
# may only change *which rows are looked at*, never which rows come back. A
# `--cabin` run and a plain one must produce the same trades, the same
# balances, and the same `--verify` verdict.


def schema_for(base, cabin, fk=False, suffix=None):
    """The column list for `base`, with the cabin policy and the foreign key
    applied if asked.

    Textual substitutions rather than a second SCHEMA table: the point is
    that the runs are the *same* schema apart from one clause each, and two
    tables would let them drift.

    `suffix` is needed for the foreign key and not for the cabin, because
    REFERENCES names a relation and every relation in a run carries the
    suffix - `REFERENCES accounts` would point at another run's table, or at
    nothing."""
    columns, clustered = SCHEMA[base]
    if cabin and base == CABIN_RELATION:
        columns = columns.replace(f"{CABIN_COLUMN} int64",
                                  f"{CABIN_COLUMN} int64 CABIN", 1)
    if fk and base == FK_CHILD:
        if suffix is None:
            raise ValueError("a foreign key needs the run suffix to name its parent")
        columns = columns.replace(
            f"{FK_COLUMN} int64",
            f"{FK_COLUMN} int64 REFERENCES {FK_PARENT}_{suffix}", 1)
    return columns, clustered

SIDE_BUY, SIDE_SELL = 0, 1

# Every account opens with this much cash, in minor units: 1,000,000.00.
# High enough that a trader partition does not run itself dry inside a
# 60-second run and start rejecting trades for want of funds, which would
# quietly turn a TPS measurement into a measurement of the retry path.
OPENING_BALANCE = 100_000_000

COUNTRIES = ("KR", "US", "JP", "GB", "DE", "SG", "HK", "AU")
ASSET_CLASSES = 4

INSERTED_ID = re.compile(r"\bid=(\d+)")


def abort(message, reply=None):
    print(f"stress aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def connect(host, port, timeout):
    try:
        return ServerConnection(host, port, timeout=timeout)
    except OSError as e:
        abort(f"could not connect to {host}:{port}: {e}\n"
              f"  start one with: ./build-release/kds_server /tmp/stress.db --port {port}")


# ---- --echo: every statement, as it is sent ------------------------------
#
# Off by default and **not free**: a write per statement, on a tool whose
# unit of measurement is statements per second. A run with it on is a run to
# read, not a run to quote.
#
# Set per process rather than passed down, because every send goes through
# one place (Client.__call__) and threading a flag to it would mean touching
# every construction site in every process. `ECHO` is inherited by the
# trader and reporter processes through fork; `ECHO_TAG` is set by each of
# them, because "which process sent this" is the one thing an interleaved
# transcript cannot be read without.
ECHO = False
ECHO_TAG = "main"

# A reply is echoed truncated: a SELECT answers with every matching row on
# one line, and a transcript that reproduces result sets is a transcript
# nobody can read.
ECHO_REPLY_MAX = 96


def set_echo(enabled, tag=None):
    global ECHO, ECHO_TAG
    if enabled is not None:
        ECHO = bool(enabled)
    if tag is not None:
        ECHO_TAG = tag


def echo_query(command, reply):
    """Writes one `<tag> <statement>  ->  <reply>` line to **stderr**.

    stderr rather than stdout so the report and `--json` stay pipeable with
    the transcript on, and so the two can be separated after the fact."""
    if not ECHO:
        return
    shown = reply if len(reply) <= ECHO_REPLY_MAX else reply[:ECHO_REPLY_MAX] + "..."
    print(f"[{ECHO_TAG}] {command}  ->  {shown}", file=sys.stderr, flush=True)


class Client:
    """One connection plus the one-command-one-reply callable everything
    below is written against. Counts errors so a caller that does not
    inspect every reply still cannot report a clean run over a failing one."""

    def __init__(self, host, port, timeout):
        self._conn = connect(host, port, timeout)
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        # Every statement this tool sends passes through here - the load, the
        # DDL, the four in a transaction, the reporter's reads, the verify
        # pass - so this is the one place --echo has to hook to see all of
        # them.
        echo_query(command, reply)
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


# ---- simulated business clock --------------------------------------------

def sim_day(started_at, seconds, days, progress=None):
    """Which of the `days` business days the run is currently in.

    Two clocks, because there are two kinds of run and they measure
    different things:

      time-based (`--seconds`)   wall-clock progress through the run. A pure
                                 function of the shared start timestamp, so
                                 the traders and the reporter derive the same
                                 day without coordinating.
      work-based (`--txn-per-user`)  progress through the *work*. `--seconds`
                                 is only a ceiling there, and it is usually
                                 much larger than the run - scaling the
                                 business clock to it would leave a run that
                                 finished in 68s of a 900s budget having
                                 simulated 13 days of 180, and the reporting
                                 job would barely run. `--days` means "the
                                 span this workload represents", so for a
                                 work target that is the target itself.

    `progress` is a shared fraction in [0, 1] the traders publish; None
    falls back to the wall clock."""
    if progress is not None:
        done = progress.value
        if done > 0.0:
            return min(days - 1, max(0, int(done * days)))
        # Before the first transaction commits there is no work to measure,
        # so day 0 - not a fall-through to the wall clock, which would jump
        # the business date backwards on the first update.
        return 0
    if seconds <= 0:
        return 0
    elapsed = (time.time() - started_at) / seconds
    return min(days - 1, max(0, int(elapsed * days)))


# ---- load ----------------------------------------------------------------

def create_tables(exec_, suffix, cabin=False, fk=False):
    # CREATE_ORDER already puts `accounts` before `trades`, which the
    # foreign key now depends on rather than merely reads better: a parent
    # has to exist before a child can reference it, and there is no
    # ALTER TABLE to add the constraint afterwards.
    for base in CREATE_ORDER:
        columns, clustered = schema_for(base, cabin, fk, suffix)
        reply = exec_(f"CREATE TABLE {base}_{suffix} ({columns}) {clustered}")
        if reply.startswith("ERR"):
            # A server built before the Cabin feature parses the column list
            # up to `CABIN` and then refuses the rest. Saying so beats a
            # syntax error pointing into the middle of a column definition.
            if fk and base == FK_CHILD and "REFERENCES" in reply.upper():
                abort(f"--fk: this server does not understand REFERENCES.\n  "
                      f"`{FK_COLUMN} int64 REFERENCES {FK_PARENT}_{suffix}` needs a build "
                      f"with docs/spec/foreign-keys.md in it (FK-M1); re-run without "
                      f"--fk, or rebuild the server.", reply)
            if fk and base == FK_CHILD and "heap relation" in reply:
                abort(f"--fk: {FK_PARENT} is a heap relation, and a foreign key "
                      f"references the parent's primary key.\n  A heap relation has no "
                      f"pk index, so every check would scan it; the declaration is "
                      f"refused rather than made slow.\n  This scenario declares "
                      f"{FK_PARENT} BTREE, so seeing this means SCHEMA was edited.",
                      reply)
            if cabin and "CABIN" in reply.upper():
                abort(f"--cabin: this server does not understand the column cabin "
                      f"policy.\n  `{CABIN_RELATION}.{CABIN_COLUMN} int64 CABIN` needs a "
                      f"build with docs/spec/cabin.md in it; re-run without --cabin, or "
                      f"rebuild the server.", reply)
            # This used to be the likeliest failure by a wide margin: the
            # catalog's column page did not chain, so the instance held ~68
            # column rows in total and a data file survived exactly two runs
            # of this scenario. The catalog relations chain now, into a
            # reserved range of ~114 pages, so reaching this means the whole
            # range is full - thousands of columns, and still no DROP TABLE
            # to reclaim any of them.
            if "no room" in reply or "reserved catalog page range" in reply:
                abort(f"could not create {base}_{suffix}: the catalog is out of column "
                      f"space.\n  Catalog relations chain into a reserved range of ~114 "
                      f"pages (~7,800 columns for the whole instance); this scenario "
                      f"needs 27 per run and nothing reclaims them, because there is no "
                      f"DROP TABLE.\n  Restart the server on a fresh data file.", reply)
            abort(f"could not create {base}_{suffix}", reply)


def read_cabin_state(exec_, suffix):
    """`SHOW CABINS`, reduced to the one row this run declared.

    Returns None when the server reports no cabin for it - which is the
    honest answer for a run that did not ask for one, and also for a server
    that has the feature switched off (`cabins = off`), where the catalog
    row exists and the runtime fields read `-`."""
    reply = exec_("SHOW CABINS")
    if reply.startswith("ERR"):
        return None

    want_rel = f"{CABIN_RELATION}_{suffix}"
    for section in reply.split("\n")[1:]:
        fields = dict(
            part.split("=", 1) for part in section.split() if "=" in part)
        if fields.get("rel") != want_rel or fields.get("column") != CABIN_COLUMN:
            continue

        def number(key):
            # `-` is what the server prints when cabins are switched off:
            # the counts are *unknown*, which is not the same as zero, so it
            # is carried through as None rather than flattened.
            raw = fields.get(key, "-")
            return int(raw) if raw.isdigit() else None

        return {
            "cabin_id": number("cabin_id"),
            "origin": fields.get("origin"),
            "status": fields.get("status"),
            "observed": number("observed"),
            "entries": number("entries"),
            "hits": number("hits"),
            "misses": number("misses"),
        }
    return None


def load_users(exec_, table, count, rng, phase):
    """Users are ids 1..count. Read back rather than assumed: ids are
    ascending but **not gapless** - a failed insert burns one - so a run
    that hits an error must not go on addressing users by ordinal."""
    ids = []
    for i in range(count):
        name = f"user{i:07d}"
        reply = phase_send(exec_, phase,
                           f"INSERT INTO {table} VALUES ('{name}', "
                           f"'{rng.choice(COUNTRIES)}', {rng.randint(0, 3)}, 0)")
        got = INSERTED_ID.search(reply)
        if got:
            ids.append(int(got.group(1)))
    return ids


def load_assets(exec_, table, count, rng, phase):
    ids = []
    for i in range(count):
        symbol = f"SYM{i:06d}"
        reply = phase_send(exec_, phase,
                           f"INSERT INTO {table} VALUES ('{symbol}', "
                           f"{rng.randint(0, ASSET_CLASSES - 1)}, "
                           f"{rng.randint(1_000, 5_000_000)})")
        got = INSERTED_ID.search(reply)
        if got:
            ids.append(int(got.group(1)))
    return ids


def load_accounts(exec_, table, user_ids, per_user, rng, phase):
    """One row per account, `per_user` of them per user on average.

    Returns [(account_id, user_id)] in creation order. The account count is
    randomized around `per_user` rather than fixed at it, because a fan-out
    where every user has exactly the same number of accounts makes the
    reporting job's per-user FilterScan cost constant, and the variance is
    the more interesting half of that measurement."""
    accounts = []
    for user_id in user_ids:
        n = max(1, rng.randint(max(1, per_user - 1), per_user + 1))
        for _ in range(n):
            reply = phase_send(exec_, phase,
                               f"INSERT INTO {table} VALUES ({user_id}, "
                               f"{OPENING_BALANCE}, 0, 0, 0)")
            got = INSERTED_ID.search(reply)
            if got:
                accounts.append((int(got.group(1)), user_id))
    return accounts


def phase_send(exec_, phase, command):
    t0 = time.perf_counter()
    reply = exec_(command)
    phase.record(time.perf_counter() - t0, reply)
    return reply


# ---- the trader process --------------------------------------------------

def trader_process(index, args, suffix, accounts, asset_ids, started_at, result_q,
                   target=0, stop_event=None, progress=None):
    """Runs 4-statement business transactions against a disjoint account
    partition until the run's wall clock is up, or until `target`
    transactions have committed - whichever comes first.

    `target` is this trader's own share of `--txn-per-user x --users`, not
    the run's total. 0 means unlimited, which is the time-based default and
    what every run before --txn-per-user existed did.

    `accounts` is this trader's partition only. Disjointness is what makes
    the in-memory balance authoritative: no other process updates these
    rows, so the driver's number and the stored number can only diverge
    through an error it saw, which is what --verify checks."""
    set_echo(getattr(args, "echo", False), f"trader-{index}")
    rng = random.Random(args.seed + 1000 + index)
    trades = f"trades_{suffix}"
    accounts_table = f"accounts_{suffix}"
    txn = bool(getattr(args, "txn", False))

    client = Client(args.host, args.port, args.timeout)

    insert_phase = Phase("trade-insert")
    update_phase = Phase("account-update")
    txn_phase = Phase("txn")

    # --latency-trace: keep (arrival, duration) per statement so the slow
    # ones can be located in time rather than only counted. What it answers
    # is whether a stall is periodic or proportional, which no percentile
    # can (bench/results-latency-matrix.md's unattributed floor).
    if getattr(args, "latency_trace", None):
        insert_phase.trace = []
        update_phase.trace = []

    # (account_id, user_id) -> [balance, asset_qty, trade_count]. Seeded
    # from the load, then owned entirely by this process.
    state = {aid: [OPENING_BALANCE, 0, 0] for aid, _ in accounts}
    ids = [aid for aid, _ in accounts]

    committed = torn = rejected = rolled_back = 0
    deadline = started_at + args.seconds
    started_running = time.perf_counter()

    try:
        while time.time() < deadline:
            # The work target, checked at the top of the iteration so the
            # count is exact rather than "the first check after". `torn`
            # deliberately does not count toward it: the target is
            # *committed* transactions, and a partial application is not one.
            if target and committed >= target:
                break
            buyer = rng.choice(ids)
            seller = rng.choice(ids)
            if seller == buyer:
                continue
            asset = rng.choice(asset_ids)
            day = sim_day(started_at, args.seconds, args.days, progress)

            price = rng.randint(1_000, 500_000)
            qty = rng.randint(1, 100)
            notional = price * qty
            if state[buyer][0] < notional:
                # The buyer cannot pay. Scale the trade down rather than
                # skipping it: a skipped iteration is a transaction that
                # never happened, and counting it as anything would bias
                # the TPS. Sized to what the buyer can afford, floor 1.
                qty = max(1, state[buyer][0] // price)
                notional = price * qty
                if state[buyer][0] < notional:
                    rejected += 1
                    continue

            # The two balance moves, computed before anything is sent:
            # nothing about them depends on a reply, and having all four
            # statements in hand is what lets the loop below treat them
            # uniformly.
            b_bal = state[buyer][0] - notional
            b_qty = state[buyer][1] + qty
            b_cnt = state[buyer][2] + 1
            s_bal = state[seller][0] + notional
            s_qty = state[seller][1] - qty
            s_cnt = state[seller][2] + 1

            # (statement, phase, the in-memory effect it earns)
            steps = (
                (f"INSERT INTO {trades} VALUES ({buyer}, {asset}, {SIDE_BUY}, "
                 f"{qty}, {price}, {day})", insert_phase, None),
                (f"INSERT INTO {trades} VALUES ({seller}, {asset}, {SIDE_SELL}, "
                 f"{qty}, {price}, {day})", insert_phase, None),
                (f"UPDATE {accounts_table} SET balance = {b_bal}, "
                 f"asset_qty = {b_qty}, trade_count = {b_cnt} WHERE id = {buyer}",
                 update_phase, (buyer, [b_bal, b_qty, b_cnt])),
                (f"UPDATE {accounts_table} SET balance = {s_bal}, "
                 f"asset_qty = {s_qty}, trade_count = {s_cnt} WHERE id = {seller}",
                 update_phase, (seller, [s_bal, s_qty, s_cnt])),
            )

            t0 = time.perf_counter()
            ok = True
            legs_written = 0
            pending = []

            # BEGIN and COMMIT are untimed but *inside* the window, so the
            # per-statement phases stay comparable between the two modes
            # while the transaction latency carries the commit where it
            # belongs. Untimed because they are not one of the four
            # statements the scenario is defined as.
            if txn:
                client("BEGIN")

            for index_of_step, (sql, phase, effect) in enumerate(steps):
                reply = client.timed(sql, phase)
                if reply.startswith("ERR"):
                    ok = False
                    if txn:
                        # A failed statement poisons the session: the engine
                        # admits only ROLLBACK/ABORT/SYNC/STOP/PING until it
                        # is unwound (docs/spec/txn.md). Sending the rest would
                        # measure the refusal path, not the workload.
                        break
                    continue
                if index_of_step < 2:
                    legs_written += 1
                if effect is None:
                    continue
                if txn:
                    # All or nothing: the driver's idea of the balance may
                    # only move once the engine has committed the move.
                    pending.append(effect)
                else:
                    # Applied as each statement is accepted, so a failure
                    # leaves the driver's balances matching what is stored.
                    # That is what keeps --verify meaningful after an error
                    # rather than only before the first one.
                    state[effect[0]] = effect[1]

            if txn:
                if ok:
                    ok = not client("COMMIT").startswith("ERR")
                if ok:
                    for who, values in pending:
                        state[who] = values
                else:
                    client("ROLLBACK")
                    rolled_back += 1

            latency = time.perf_counter() - t0
            if ok:
                committed += 1
                # Published for the reporting process's business clock. One
                # unsynchronized double store per transaction; the traders
                # are symmetric, so any one of them is representative and a
                # torn read costs a simulated day, not a row.
                if progress is not None and target:
                    progress.value = min(1.0, committed / target)
                txn_phase.record(latency, "OK")
            elif txn:
                # Nothing was applied: the engine unwound it. Counted as a
                # failed transaction and never as a torn one, which is the
                # whole point of the flag.
                txn_phase.record(latency, "ERR rolled back")
            else:
                # A partial application. No transaction was open, so there
                # is nothing to roll back - see the module docstring, point
                # 1 - and the only honest thing to do is count it and keep
                # going.
                if legs_written:
                    torn += 1
                txn_phase.record(latency, "ERR partial")
    except (ConnectionError, OSError) as e:
        result_q.put({"index": index, "fatal": str(e)})
        return
    finally:
        elapsed = time.perf_counter() - started_running
        # Tell the reporting process the measured window is over. Without
        # this a count-targeted run that finishes in 40s of a 600s budget
        # would leave the reporter walking the relation for another 560s,
        # and the run would take as long as its *ceiling* rather than as
        # long as its work.
        if stop_event is not None:
            stop_event.set()
        try:
            client.close()
        except OSError:
            pass

    for phase in (insert_phase, update_phase, txn_phase):
        phase.elapsed = elapsed

    if getattr(args, "latency_trace", None):
        # One file per trader, since two processes cannot share a handle
        # without ordering their writes - and the ordering is the data.
        with open(f"{args.latency_trace}.{index}", "w") as f:
            for name, phase in (("trade-insert", insert_phase),
                                ("account-update", update_phase)):
                for at, took in phase.trace:
                    f.write(f"{name} {at:.6f} {took * 1e6:.1f}\n")

    result_q.put({
        "index": index,
        "committed": committed,
        "torn": torn,
        "rolled_back": rolled_back,
        "rejected": rejected,
        "elapsed": elapsed,
        "target": target,
        "hit_target": bool(target and committed >= target),
        "errors": client.errors,
        "first_error": client.first_error,
        "phases": {p.name: (p.latencies, p.errors, p.first_error, p.elapsed)
                   for p in (insert_phase, update_phase, txn_phase)},
        # Final balances, so the parent can verify a sample against storage.
        "state": {aid: v for aid, v in state.items()},
    })


# ---- the periodic reporting process --------------------------------------

def profit_process(args, suffix, user_ids, started_at, result_q, stop_event=None,
                   progress=None):
    """The 'other process': periodic per-user profit, written while the
    traders are writing trades.

    Reads with `SELECT * FROM accounts WHERE user_id = <n>` - a non-pk
    equality, which the step compiler makes a `FilterScan`: a walk of the
    whole accounts relation per user, deliberately. That is what a reporting
    job on an unindexed foreign key costs, and pretending otherwise by
    driving it off pk lookups would measure a different job.

    Its unit of work is a *period*: one row per sampled user per
    `--profit-period-days` of simulated time. Wall-clock ticks
    (`--profit-interval`) only decide how often it checks whether a period
    boundary has been crossed."""
    set_echo(getattr(args, "echo", False), "reporter")
    rng = random.Random(args.seed + 7777)
    accounts_table = f"accounts_{suffix}"
    profit_table = f"user_periodic_profit_{suffix}"

    client = Client(args.host, args.port, args.timeout)

    scan_phase = Phase("profit-scan")
    write_phase = Phase("profit-insert")

    deadline = started_at + args.seconds
    started_running = time.perf_counter()
    next_period = args.profit_period_days
    periods = rows = 0

    try:
        while time.time() < deadline:
            if stop_event is not None and stop_event.is_set():
                break
            time.sleep(min(args.profit_interval, max(0.0, deadline - time.time())))
            day = sim_day(started_at, args.seconds, args.days, progress)
            if day < next_period:
                continue

            # A tick can cross several period boundaries when the simulated
            # clock runs fast; only the latest is reported, and the skipped
            # ones are counted as skipped rather than backfilled - a report
            # that could not keep up did not run.
            period_day = day
            sample = rng.sample(user_ids, min(args.profit_users, len(user_ids)))
            for user_id in sample:
                if time.time() >= deadline:
                    break
                if stop_event is not None and stop_event.is_set():
                    break
                text = client.timed(
                    f"SELECT * FROM {accounts_table} WHERE user_id = {user_id}",
                    scan_phase)
                if text.startswith("ERR"):
                    continue

                total_balance = 0
                total_trades = 0
                account_rows = 0
                for line in text.split("\n")[1:]:
                    fields = line.split(",")
                    if len(fields) < 6:
                        continue
                    try:
                        total_balance += int(fields[2])
                        total_trades += int(fields[4])
                    except ValueError:
                        continue
                    account_rows += 1

                # Realized profit = cash now, less the cash this user's
                # accounts opened with. Positions (asset_qty) are excluded
                # on purpose: marking them would need each asset's price,
                # which is a second relation read per row and would make
                # this a different measurement.
                realized = total_balance - account_rows * OPENING_BALANCE
                reply = client.timed(
                    f"INSERT INTO {profit_table} VALUES ({user_id}, "
                    f"{period_day}, {realized}, {total_trades})", write_phase)
                if not reply.startswith("ERR"):
                    rows += 1

            periods += 1
            next_period = period_day + args.profit_period_days
    except (ConnectionError, OSError) as e:
        result_q.put({"fatal": str(e)})
        return
    finally:
        elapsed = time.perf_counter() - started_running
        try:
            client.close()
        except OSError:
            pass

    for phase in (scan_phase, write_phase):
        phase.elapsed = elapsed

    result_q.put({
        "periods": periods,
        "rows": rows,
        "elapsed": elapsed,
        "errors": client.errors,
        "first_error": client.first_error,
        "phases": {p.name: (p.latencies, p.errors, p.first_error, p.elapsed)
                   for p in (scan_phase, write_phase)},
    })


# ---- result assembly -----------------------------------------------------

def merge_phase(name, contributions, elapsed):
    """One Phase covering every process's samples for a statement kind.

    `elapsed` is the run's wall clock, not the sum of the contributors':
    with N traders the throughput being reported is aggregate, so dividing
    by any one process's elapsed time would multiply it by N."""
    merged = Phase(name)
    for latencies, errors, first_error, _ in contributions:
        merged.latencies.extend(latencies)
        merged.errors += errors
        if merged.first_error is None:
            merged.first_error = first_error
    merged.elapsed = elapsed
    return merged


def verify_balances(exec_, table, states, sample_size, rng):
    """Reads a sample of accounts back and compares stored balance /
    asset_qty / trade_count against what the traders believe they wrote.

    The check that matters is not "is arithmetic right" - it is whether an
    update was lost. With disjoint partitions and no rollback, a mismatch
    means either a torn transaction or a statement the driver counted as
    applied that was not."""
    combined = {}
    for state in states:
        combined.update(state)
    if not combined:
        return 0, 0, None
    ids = rng.sample(list(combined), min(sample_size, len(combined)))
    checked = mismatched = 0
    first = None
    for account_id in ids:
        text = exec_(f"SELECT * FROM {table} WHERE id = {account_id}")
        if text.startswith("ERR"):
            continue
        rows = [line for line in text.split("\n")[1:] if line]
        if not rows:
            continue
        fields = rows[0].split(",")
        if len(fields) < 6:
            continue
        checked += 1
        stored = (int(fields[2]), int(fields[3]), int(fields[4]))
        expected = tuple(combined[account_id])
        if stored != expected:
            mismatched += 1
            if first is None:
                first = (f"account {account_id}: stored "
                         f"balance/qty/trades {stored}, driver expected {expected}")
    return checked, mismatched, first


def print_cabin(meta):
    """What the Cabin did, or why there is nothing to say.

    `hits` and `misses` are the whole story of the layer and are printed
    even when they are unflattering: a run whose reporter samples users too
    sparsely to re-probe one will show misses and no hits, and that is a
    true statement about the workload rather than a failure of the
    structure."""
    cabin = meta.get("cabin_state")
    if not meta.get("cabin"):
        print("  cabin               disabled (--cabin declares one on "
              f"{CABIN_RELATION}.{CABIN_COLUMN}); the reporting scan above is a "
              "FilterScan")
        print()
        return
    if cabin is None:
        print("  cabin               requested, but the server reports none - is this "
              "build older than docs/spec/cabin.md?")
        print()
        return
    if cabin["observed"] is None:
        print("  cabin               declared, but the server has cabins switched off "
              "(`cabins = off`): the catalog row exists and nothing is observed")
        print()
        return

    hits, misses = cabin["hits"] or 0, cabin["misses"] or 0
    probes = hits + misses
    rate = (hits / probes * 100.0) if probes else 0.0
    print(f"  cabin               on {CABIN_RELATION}.{CABIN_COLUMN} "
          f"(origin={cabin['origin']}, {cabin['status']})")
    print(f"    observed values   {cabin['observed']:>12,}   of {meta['users']:,} users")
    print(f"    entries           {cabin['entries']:>12,}   ~24 bytes each")
    print(f"    served / walked   {hits:>12,} / {misses:,}   {rate:.1f}% of probes "
          f"served authoritatively")
    if probes and hits == 0:
        print("                      no probe was ever repeated: with "
              f"--profit-users {meta['profit_users']} sampled out of "
              f"{meta['users']:,}, a user is rarely seen twice. Raise --profit-users "
              "or lower --users to measure the served path.")
    print()


def print_fk(meta):
    """What the foreign key was doing, if anything.

    Deliberately short next to `print_cabin`: a Cabin has runtime state worth
    reporting - observed values, hits, misses - and a foreign key has none.
    It either constrained every write or it was not declared, and the cost of
    it is in the trade-insert latency rather than in a counter of its own."""
    if not meta.get("fk"):
        print(f"  foreign key         none (--fk declares {FK_CHILD}.{FK_COLUMN} -> "
              f"{FK_PARENT})")
        print()
        return
    print(f"  foreign key         {FK_CHILD}.{FK_COLUMN} -> {FK_PARENT} (RESTRICT)")
    print("    checked           every trade leg, before it is written: one btree "
          "descent into")
    print("                      the parent plus the visibility test. Its cost is "
          "inside")
    print("                      trade-insert below, not beside it.")
    print("    not exercised     the reverse check - this workload deletes no "
          "accounts")
    print()


def print_scenario(meta, trader_results, profit_result, verify):
    print()
    print("business scenario")
    print("-" * 72)
    print(f"  users {meta['users']:,}   accounts {meta['accounts']:,}   "
          f"assets {meta['assets']:,}   simulated span {meta['days']} days "
          f"in {meta['seconds']:.1f}s wall")
    print(f"  transaction = 2 INSERT trades + 2 UPDATE accounts, "
          f"{meta['traders']} trader process(es)")
    print("  statement grouping  "
          + ("BEGIN/COMMIT: one transaction, one durability point"
             if meta.get("txn") else
             "four autocommit statements: four transactions, four durability "
             "points (--txn groups them)"))
    if meta.get("limit") == "transactions":
        print(f"  ran to a work target of {meta['txn_target']:,} transactions "
              f"(ceiling was {meta['seconds_budget']:.0f}s); TPS below is over the "
              f"{meta['seconds']:.1f}s the work took")
    elif meta.get("txn_target"):
        print(f"  work target of {meta['txn_target']:,} transactions NOT reached in "
              f"{meta['seconds_budget']:.0f}s - raise --seconds to complete it")
    print()

    committed = sum(r["committed"] for r in trader_results)
    torn = sum(r["torn"] for r in trader_results)
    rolled_back = sum(r.get("rolled_back", 0) for r in trader_results)
    rejected = sum(r["rejected"] for r in trader_results)
    wall = meta["seconds"]
    tps = committed / wall if wall > 0 else 0.0

    print(f"  TPS                 {tps:>12,.1f}   committed transactions/sec")
    print(f"  statements/sec      {tps * 4:>12,.1f}   4 statements per transaction")
    print(f"  committed           {committed:>12,}")
    if meta.get("txn"):
        print(f"  rolled back         {rolled_back:>12,}   unwound whole; nothing was "
              f"half-applied")
        print(f"  torn                {torn:>12,}   zero by construction under --txn")
    else:
        print(f"  torn                {torn:>12,}   partially applied; no transaction "
              f"was open to roll back (--txn opens one)")
    print(f"  underfunded         {rejected:>12,}   skipped before any statement was sent")
    if meta["traders"] > 1:
        per = "  ".join(f"#{r['index']}:{r['committed']:,}" for r in sorted(
            trader_results, key=lambda r: r["index"]))
        print(f"  per trader          {per}")
    print()

    print_cabin(meta)
    print_fk(meta)

    if profit_result:
        print(f"  reporting process   {profit_result['periods']:>12,} periods, "
              f"{profit_result['rows']:,} user_periodic_profit rows written")
        print("                      "
              + ("one cabin probe of accounts per user per period, falling back to a "
                 "walk on an unobserved value"
                 if meta.get("cabin") else
                 "one FilterScan of accounts per user per period"))
    else:
        print("  reporting process   disabled (--no-profit): the TPS above has no "
              "concurrent reader")
    print()

    if verify is not None:
        checked, mismatched, first = verify
        verdict = "all match" if mismatched == 0 else f"{mismatched} MISMATCH"
        print(f"  balance verify      {checked:>12,} accounts read back - {verdict}")
        if first:
            print(f"                      {first}")
        print()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"default: {DEFAULT_PORT}")
    parser.add_argument("--suffix", default=None,
                        help="table-name suffix; default <epoch>_<rand>, i.e. fresh "
                             "relations per run (there is no DROP TABLE)")

    parser.add_argument("--users", type=int, default=10000,
                        help="rows in users (default: 10000)")
    parser.add_argument("--assets", type=int, default=10000,
                        help="rows in assets (default: 10000)")
    parser.add_argument("--accounts-per-user", type=int, default=2,
                        help="average accounts per user (default: 2); the actual "
                             "count per user varies by +-1 so the reporting job's "
                             "per-user scan is not a constant")
    parser.add_argument("--days", type=int, default=180,
                        help="simulated business days the run represents (default: "
                             "180); compressed into --seconds of wall clock")

    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long the measured transaction phase runs, in wall-"
                             "clock seconds (default: 60)")
    parser.add_argument("--traders", type=int, default=1,
                        help="concurrent trader processes (default: 1). Each owns a "
                             "disjoint partition of accounts, which is what makes the "
                             "client-side balance arithmetic safe without locks")

    parser.add_argument("--profit", dest="profit", action="store_true", default=True,
                        help="run the periodic profit reporter in a separate process "
                             "(default: on)")
    parser.add_argument("--no-profit", dest="profit", action="store_false",
                        help="run the traders alone, to price what the reporter costs")
    parser.add_argument("--profit-interval", type=float, default=1.0,
                        help="seconds between reporter wake-ups (default: 1.0); it "
                             "only checks whether a simulated period boundary passed")
    parser.add_argument("--profit-period-days", type=int, default=7,
                        help="simulated days per reporting period (default: 7)")
    parser.add_argument("--profit-users", type=int, default=50,
                        help="users sampled per period (default: 50); each costs one "
                             "FilterScan of accounts plus one INSERT")

    parser.add_argument("--txn-per-user", type=float, default=0.0, metavar="N",
                        help="stop after N x --users transactions have committed, "
                             "instead of running for the whole --seconds. 0 (default) "
                             "keeps the time-based behaviour. --seconds becomes a "
                             "ceiling, and TPS is reported over the time the work "
                             "actually took - so a run that hits its target early is "
                             "measured over that shorter window, not over the budget")

    parser.add_argument("--latency-trace", metavar="PATH", default=None,
                        help="write every statement's arrival time and duration to "
                             "PATH.<trader> - one line per statement, for locating slow "
                             "ones in time. A percentile says how bad the tail is and "
                             "never when it happened, which is what tells a periodic "
                             "stall from a proportional one")

    parser.add_argument("--echo", dest="echo", action="store_true", default=False,
                        help="print every statement this tool sends, and its reply, to "
                             "stderr as `[<who>] <statement>  ->  <reply>`. Covers the "
                             "load, the DDL, the four statements of every transaction, "
                             "the reporter's reads and the verify pass - everything "
                             "goes through one send. Off by default because it costs a "
                             "write per statement on a tool that measures statements "
                             "per second: a run with it on is a run to read, not one to "
                             "quote. Replies are truncated, and the lines from "
                             "concurrent traders interleave, which is what the tag is "
                             "for")

    parser.add_argument("--txn", dest="txn", action="store_true", default=False,
                        help="wrap each business transaction's four statements in "
                             "BEGIN/COMMIT (docs/spec/txn.md), instead of sending them as "
                             "four autocommit statements. Default off, so the baseline "
                             "stays what it has always been. It is the largest single "
                             "lever on this workload - four durability points become "
                             "one - and it drives `torn` to zero by construction, "
                             "because a failure now unwinds instead of leaving half a "
                             "trade behind")
    parser.add_argument("--no-txn", dest="txn", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--fk", dest="fk", action="store_true", default=False,
                        help=f"declare a foreign key on {FK_CHILD}.{FK_COLUMN} -> "
                             f"{FK_PARENT} (docs/spec/foreign-keys.md): every trade leg "
                             f"then probes the account it names before it is written, "
                             f"and a leg naming no account is refused instead of "
                             f"stored. Default off, so the baseline stays what it has "
                             f"always been - run it both ways and compare. The "
                             f"reverse check never fires here: this workload deletes "
                             f"no accounts")
    parser.add_argument("--no-fk", dest="fk", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--cabin", dest="cabin", action="store_true", default=False,
                        help=f"declare a Cabin on {CABIN_RELATION}.{CABIN_COLUMN} "
                             f"(docs/spec/cabin.md): the reporting job's "
                             f"`WHERE {CABIN_COLUMN} = <n>` is then served from an "
                             f"observed value's entry set instead of walking the "
                             f"relation. Default off, so the baseline stays what it "
                             f"has always been - run it both ways and compare. It is "
                             f"not free on the write side: each account UPDATE pays a "
                             f"directory probe, so read TPS and profit-scan together")
    parser.add_argument("--no-cabin", dest="cabin", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--verify", type=int, default=200,
                        help="accounts read back and compared against the drivers' "
                             "in-memory balances after the run (default: 200); 0 skips")
    parser.add_argument("--seed", type=int, default=1, help="RNG seed (default: 1)")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="socket timeout in seconds (default: 120); a FilterScan "
                             "over a large accounts relation is one slow reply")
    parser.add_argument("--sync", action="store_true",
                        help="send SYNC after the run and time it - UPDATE is unlogged, "
                             "so without this the balance moves survive only a clean "
                             "shutdown (docs/spec/wal.md)")
    parser.add_argument("--json", metavar="PATH", help="also write results as JSON")
    parser.add_argument("--server-log", metavar="PATH",
                        help="the server's log at --log-level debug: adds its own "
                             "per-statement microseconds, which is the only view of "
                             "engine cost separate from this client's round trip")
    args = parser.parse_args()

    # Set before anything is sent, so the DDL and the load are in the
    # transcript too. The trader and reporter processes inherit it and
    # re-tag themselves.
    set_echo(args.echo, "loader")

    if args.traders < 1:
        abort("--traders must be at least 1")
    if args.days < 1:
        abort("--days must be at least 1")

    suffix = args.suffix or f"{time.time_ns() // 1_000_000_000}_{random.randrange(1 << 16)}"
    rng = random.Random(args.seed)

    loader = Client(args.host, args.port, args.timeout)

    # ---- load ------------------------------------------------------------
    print(f"loading: users={args.users:,} assets={args.assets:,} "
          f"accounts~{args.users * args.accounts_per_user:,}  "
          f"(tables suffixed _{suffix})"
          + (f"  [cabin on {CABIN_RELATION}.{CABIN_COLUMN}]" if args.cabin else "")
          + (f"  [fk {FK_CHILD}.{FK_COLUMN} -> {FK_PARENT}]" if args.fk else "")
          + ("  [BEGIN/COMMIT per transaction]" if args.txn else ""),
          flush=True)

    create_tables(loader, suffix, args.cabin, args.fk)

    load_phases = []
    t_load = time.perf_counter()

    users_phase = Phase("load-users", "one row per customer")
    user_ids = load_users(loader, f"users_{suffix}", args.users, rng, users_phase)
    users_phase.elapsed = sum(users_phase.latencies)
    load_phases.append(users_phase)
    print(f"  users    {len(user_ids):>8,} rows", flush=True)

    assets_phase = Phase("load-assets", "instruments")
    asset_ids = load_assets(loader, f"assets_{suffix}", args.assets, rng, assets_phase)
    assets_phase.elapsed = sum(assets_phase.latencies)
    load_phases.append(assets_phase)
    print(f"  assets   {len(asset_ids):>8,} rows", flush=True)

    accounts_phase = Phase("load-accounts", f"{args.accounts_per_user}+-1 per user")
    accounts = load_accounts(loader, f"accounts_{suffix}", user_ids,
                             args.accounts_per_user, rng, accounts_phase)
    accounts_phase.elapsed = sum(accounts_phase.latencies)
    load_phases.append(accounts_phase)
    print(f"  accounts {len(accounts):>8,} rows   "
          f"(load took {time.perf_counter() - t_load:.1f}s)", flush=True)

    if not user_ids or not asset_ids:
        abort("the load produced no users or no assets - nothing to trade",
              loader.first_error)
    if len(accounts) < 2 * args.traders:
        abort(f"{len(accounts)} accounts is too few for {args.traders} traders: each "
              f"needs at least 2 in its own partition")

    # ---- run -------------------------------------------------------------
    #
    # Partitioned round-robin rather than in contiguous blocks, so every
    # trader's accounts are spread across the whole relation. Contiguous
    # blocks would give trader 0 the pages at the head of the chain and
    # trader N-1 the tail, and on a btree that is a different descent
    # profile per process.
    partitions = [accounts[i::args.traders] for i in range(args.traders)]

    result_q = multiprocessing.Queue()
    started_at = time.time()

    # The work target, split across traders. Split rather than shared: a
    # shared counter would need a lock on the hot path, and the partitions
    # are already equal-sized, so an equal split is the same total.
    total_target = int(round(args.txn_per_user * len(user_ids))) if args.txn_per_user else 0
    per_trader = (total_target + args.traders - 1) // args.traders if total_target else 0

    # Set by whichever trader finishes first, so the reporting process ends
    # with the measured window instead of running out the --seconds ceiling.
    stop_event = multiprocessing.Event()

    # Shared work progress, for the business clock. Only meaningful for a
    # target run; a time-based one leaves it None and keeps the wall clock.
    progress = multiprocessing.Value("d", 0.0, lock=False) if total_target else None

    workers = [multiprocessing.Process(
        target=trader_process,
        args=(i, args, suffix, partitions[i], asset_ids, started_at, result_q,
              per_trader, stop_event, progress))
        for i in range(args.traders)]

    reporter_q = multiprocessing.Queue()
    reporter = None
    if args.profit:
        reporter = multiprocessing.Process(
            target=profit_process,
            args=(args, suffix, user_ids, started_at, reporter_q, stop_event, progress))

    if total_target:
        print(f"running until {total_target:,} transactions commit "
              f"({args.txn_per_user:g} per user, ceiling {args.seconds:.0f}s): "
              f"{args.traders} trader process(es)"
              f"{' + 1 reporting process' if reporter else ''}, "
              f"{args.days} simulated days", flush=True)
    else:
        print(f"running {args.seconds:.0f}s: {args.traders} trader process(es)"
              f"{' + 1 reporting process' if reporter else ''}, "
              f"{args.days} simulated days", flush=True)

    for w in workers:
        w.start()
    if reporter:
        reporter.start()

    # Drain the queues **before** joining. A result carries every latency
    # sample and the whole balance state, which is far more than a pipe
    # buffer holds: the child's feeder thread blocks until the parent
    # reads, and the child cannot exit while it blocks. Joining first is
    # therefore a deadlock, not a race - it hangs every time, not
    # occasionally.
    grace = args.seconds + 120.0
    try:
        trader_results = [result_q.get(timeout=grace) for _ in workers]
        profit_result = reporter_q.get(timeout=grace) if reporter else None
        for w in workers:
            w.join(timeout=grace)
        if reporter:
            reporter.join(timeout=grace)
    except KeyboardInterrupt:
        for p in workers + ([reporter] if reporter else []):
            p.terminate()
        abort("interrupted")
    except queue.Empty:
        for p in workers + ([reporter] if reporter else []):
            p.terminate()
        abort("a worker process produced no result and was terminated - it most "
              "likely died on an unhandled exception; re-run with --traders 1 "
              "--no-profit to see it in the foreground")

    fatal = [r for r in trader_results if "fatal" in r]
    if fatal:
        abort(f"a trader process lost the server: {fatal[0]['fatal']}")

    if profit_result and "fatal" in profit_result:
        print(f"warning: the reporting process lost the server: "
              f"{profit_result['fatal']}", file=sys.stderr)
        profit_result = None

    # **The measured window, not the budget.** TPS is committed/wall, so a
    # count-targeted run that hit its target in 40s of a 600s ceiling must
    # be divided by 40 or the number is meaningless. The traders' own
    # measured elapsed is the truth; the max across them is the span in
    # which all the work happened.
    #
    # For a time-based run this is the same number it always was, to within
    # process startup - the traders run to the deadline by construction.
    wall = max((r.get("elapsed", 0.0) for r in trader_results), default=args.seconds)
    if wall <= 0:
        wall = args.seconds

    # ---- report ----------------------------------------------------------
    phases = list(load_phases)
    for name in ("txn", "trade-insert", "account-update"):
        contributions = [r["phases"][name] for r in trader_results]
        merged = merge_phase(name, contributions, wall)
        if name == "txn":
            merged.detail = ("one business transaction: 2 INSERT trades + 2 UPDATE "
                             "accounts, latency measured across all four; qps here IS "
                             "the TPS")
        elif name == "trade-insert":
            merged.detail = "heap chain tail append, WAL-logged"
        else:
            merged.detail = "in-place overwrite after a clustered-btree descent, unlogged"
        phases.append(merged)

    if profit_result:
        for name in ("profit-scan", "profit-insert"):
            merged = merge_phase(name, [profit_result["phases"][name]], wall)
            if name == "profit-scan":
                merged.detail = (
                    f"CabinProbe: WHERE {CABIN_COLUMN} = <n> served from the cabin's "
                    f"entry set once that value is observed; a miss walks the relation "
                    f"and records while it does"
                    if args.cabin else
                    f"FilterScan: WHERE {CABIN_COLUMN} = <n>, a non-pk equality, so "
                    f"the whole accounts relation is walked per user")
            else:
                merged.detail = "one user_periodic_profit row per user per period"
            phases.append(merged)

    verify = None
    if args.verify > 0:
        verify = verify_balances(loader, f"accounts_{suffix}",
                                 [r["state"] for r in trader_results],
                                 args.verify, rng)

    if args.sync:
        t0 = time.perf_counter()
        sync_reply = loader("SYNC")
        sync_phase = Phase("sync", "page store flushed to the data file")
        sync_phase.record(time.perf_counter() - t0, sync_reply)
        sync_phase.elapsed = sync_phase.latencies[0]
        phases.append(sync_phase)

    # Read *after* the run, so the numbers describe what the workload
    # actually did rather than what the load phase left behind.
    cabin_state = read_cabin_state(loader, suffix) if args.cabin else None

    trade_rows = reply_rows(loader(f"SELECT * FROM trades_{suffix}")) \
        if args.users <= 200 else None
    loader.close()

    durability = read_durability(args.server_log) if args.server_log else None

    committed = sum(r["committed"] for r in trader_results)
    meta = {
        "engine": "ckdbs",
        "scenario": "brokerage: users/accounts/assets/trades/user_periodic_profit",
        "host": args.host,
        "port": args.port,
        "table": f"*_{suffix}",
        "suffix": suffix,
        "columns": sum(len(SCHEMA[t][0].split(",")) for t in CREATE_ORDER),
        "rows": len(accounts),
        "users": len(user_ids),
        "accounts": len(accounts),
        "assets": len(asset_ids),
        "days": args.days,
        "seconds": round(wall, 3),
        "seconds_budget": args.seconds,
        # What ended the run. A target-limited run is a *fixed amount of
        # work* measured in time; a time-limited one is fixed time measured
        # in work. They are not the same experiment and the numbers should
        # not be read as though they were.
        "limit": ("transactions" if any(r.get("hit_target") for r in trader_results)
                  else "seconds"),
        "txn_target": total_target,
        "traders": args.traders,
        # One per trader, plus the reporting process's own.
        "connections": args.traders + (1 if profit_result else 0),
        "profit": bool(profit_result),
        "profit_users": args.profit_users,
        # The Cabin, as a pair: what was asked for, and what the server says
        # happened. Both, because "requested but the server reports none" is
        # a real and confusing outcome (an older build, or `cabins = off`)
        # and a single field could not express it.
        "cabin": bool(args.cabin),
        "fk": bool(args.fk),
        "txn": bool(args.txn),
        "cabin_column": f"{CABIN_RELATION}.{CABIN_COLUMN}" if args.cabin else None,
        "cabin_state": cabin_state,
        "seed": args.seed,
        "durability": durability,
        "tps": round(committed / wall, 2) if wall > 0 else 0.0,
        "committed": committed,
        "torn": sum(r["torn"] for r in trader_results),
        "rolled_back": sum(r.get("rolled_back", 0) for r in trader_results),
    }

    report(phases, meta, footer=[
        "the four statements are NOT atomic as this tool runs them: the engine has "
        "BEGIN/COMMIT (docs/spec/txn.md, built) but this tool does not use them, so a "
        "failure between them leaves a trade recorded against balances that never "
        "moved. `torn` in the scenario block below counts those.",
        "balances are computed client-side because UPDATE ... SET takes a literal, not "
        "an expression; each trader owns a disjoint account partition so no update can "
        "be lost between them, and --verify reads a sample back to confirm it.",
        "money is int64 minor units - float/decimal columns are refused at CREATE "
        "TABLE under the fixed-length rule.",
        f"INSERT is the one logged statement"
        + (f" (durability={durability})" if durability else "")
        + "; UPDATE and CREATE TABLE are unlogged, so the balance moves survive only "
          "a SYNC or a clean shutdown. Do not measure this on tmpfs, where fsync is "
          "free and every durability class looks identical.",
        (f"--cabin declares a Cabin on {CABIN_RELATION}.{CABIN_COLUMN} "
         "(docs/spec/cabin.md): the reporter's non-pk equality is then served from an "
         "observed value's entry set instead of walking the relation, and the account "
         "UPDATEs pay a directory probe each for it. A Cabin may only change which "
         "rows are *looked at* - the trades, the balances and the --verify verdict "
         "must be identical to a run without one."
         if args.cabin else
         f"no Cabin (--cabin declares one on {CABIN_RELATION}.{CABIN_COLUMN}): the "
         "reporter's `WHERE user_id = <n>` walks the whole accounts relation per user, "
         "per period, which is what a reporting job on an unindexed foreign key "
         "costs."),
        "accounts/users/assets are BTREE and trades/user_periodic_profit are HEAP - a "
        "heap accounts relation would make every UPDATE a full chain scan, and the "
        "update number would then be a function of --users, not of the engine.",
    ])

    print_scenario(meta, trader_results, profit_result, verify)

    if trade_rows is not None:
        print(f"  trades relation holds {trade_rows:,} rows "
              f"(2 per committed transaction)")
        print()

    errors = sum(r["errors"] for r in trader_results)
    if profit_result:
        errors += profit_result["errors"]
    if errors:
        first = next((r["first_error"] for r in trader_results if r["first_error"]), None)
        print(f"  {errors:,} statements replied ERR; first: {first}", file=sys.stderr)

    if args.server_log:
        try:
            by_kind = server_side_us(args.server_log, suffix)
            if by_kind:
                print("  server-side, as measured by the server itself:")
                for kind in sorted(by_kind):
                    v = by_kind[kind]
                    print(f"    {kind:<8}{len(v):>8,} stmts  p50 {v[len(v) // 2]:>7} us"
                          f"  p95 {v[int(len(v) * 0.95)]:>7} us")
            else:
                print("  --server-log: no timed query lines for this run "
                      "(is the server at --log-level debug?)")
        except OSError as e:
            print(f"  --server-log: could not read {args.server_log}: {e}")

    if args.json:
        write_json(args.json, meta, phases)

    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
