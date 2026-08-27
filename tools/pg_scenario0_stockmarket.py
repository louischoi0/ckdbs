#!/usr/bin/env python3
"""PostgreSQL baseline for tools/scenario0_stockmarket.py - the same scenario, other engine.

Where tools/pg_benchmark.py prices one statement kind at a time against one
synthetic table, this drives the **identical five-relation brokerage workload**
scenario0_stockmarket.py drives against ckdbs, and reports the same number the same
way: completed business transactions per second, under concurrent periodic
reporting. The two tools share bench_common.py, so their `--json` outputs are
diffable phase by phase and scenario line by scenario line.

The schema, five relations, in the same column order with the closest
PostgreSQL types:

    users                  one row per customer
    accounts               many per user (the user->accounts fan-out)
    assets                 the instruments that get bought and sold
    trades                 append-only history; two legs per executed trade
    user_periodic_profit   written by the *reporting* process, not the traders

The measured transaction - the unit TPS counts - is one executed trade
between two accounts, and it is the same four statements:

    INSERT INTO trades ...     the buy leg  (side=0, buyer's account)
    INSERT INTO trades ...     the sell leg (side=1, seller's account)
    UPDATE accounts SET ...    buyer:  balance down, asset_qty up
    UPDATE accounts SET ...    seller: balance up,   asset_qty down

A transaction counts only if all four statements replied without `ERR`.

Concurrently, a **separate process** (`--profit`, on by default) plays the
periodic reporting job: every `--profit-interval` seconds it wakes, reads each
sampled user's accounts with `SELECT * FROM accounts WHERE user_id = <n>` - a
non-pk equality, which without an index on that column is a seqscan - and
appends one `user_periodic_profit` row per user per period.

## Four deliberate fidelity choices, and why each one is not the "natural"
## PostgreSQL way to write it

A baseline that lets each engine play to its strengths measures two different
workloads and licenses no comparison at all. These four keep the workload one
workload; every one of them makes PostgreSQL look *worse* than it would if
written idiomatically, and each is stated here so a quoted number carries it.

1. **The four statements are not wrapped in BEGIN/COMMIT by default**,
   exactly as scenario0_stockmarket.py leaves them unwrapped, and `--txn` wraps
   them on both sides. PostgreSQL has had transactions since before this repo
   existed, so the default is not a limitation being matched - it is the
   *statement count* being matched. Wrapping turns four commits into one and
   roughly quarters the fsync bill, which prices a feature ckdbs also has
   (docs/spec/txn.md is built) rather than the statement throughput both tools set
   out to measure - so it is a flag on both, off on both, and meaningful only
   when set on both. Unwrapped, the tool counts partial applications and
   prints them as `torn`, the same as the ckdbs side; wrapped, `torn` is zero
   on both by construction.

2. **Balances are computed client-side and sent as literals.** ckdbs's
   `UPDATE ... SET col = <val>` takes a literal, not an expression, so
   `balance = balance - notional` is not expressible there. It is trivially
   expressible here - and writing it that way would delete a client round
   trip's worth of arithmetic on one side only, and would additionally make
   the update atomic-per-row in a way the ckdbs run is not. So each trader
   owns a disjoint partition of accounts (`--traders` splits them), keeps that
   partition's balances in memory, and sends the computed number. `--verify`
   reads a sample back and compares.

3. **Money is int64 minor units** (`bigint` cents), never `numeric`. ckdbs
   refuses float/decimal columns at CREATE TABLE; `numeric` here would be both
   the correct financial type and a slower one, and would make the row a
   different size.

4. **`id` is server-generated on both sides.** ckdbs invariant 11 forbids a
   caller-supplied pk, so every relation uses
   `id bigint GENERATED ALWAYS AS IDENTITY` and INSERT names only the body
   columns. An identity column is a sequence, not an index, which is what lets
   the heap-analogue relations below have genuinely zero indexes.

Storage organization mirrors the ckdbs `clustered_type` choice per relation,
which is the part of that run that is a measurement rather than a workload:

    accounts, users, assets   PRIMARY KEY on id  <- ckdbs BTREE: every access
                              is `WHERE id = <n>`, an index descent on both
    trades, user_periodic_profit
                              identity, no index <- ckdbs HEAP: insert-only,
                              never probed by pk, so a plain heap append on
                              both and no index maintenance per row on either

Simulated time is identical: `--days` (default 180) is a **business** span
compressed into the `--seconds` the run takes, and both the trade's
`trade_day` and the reporter's period boundaries derive from progress through
the run. Nothing sleeps to make it true. The clock function is imported from
scenario0_stockmarket.py rather than reimplemented, so the two runs cannot drift.

## The `--cabin` counterpart

ckdbs's `--cabin` gives `accounts.user_id` a **Cabin** (docs/spec/cabin.md):
a store authoritative for the values queries have actually observed, which
serves the reporter's `WHERE user_id = <n>` from an observed value's entry set
instead of walking the relation, and charges each account UPDATE a directory
probe to keep that set complete.

The PostgreSQL counterpart of "make that predicate not a full walk" is a btree
index on the column, and `--user-id-index` (aliased `--cabin`) creates one. It
is an **analogue, not an equivalence**, and the difference is the interesting
part rather than a caveat to skip:

  * an index is authoritative for *every* value from the moment it exists; a
    Cabin is authoritative only for values that have been observed, and
    answers a miss by walking and recording.
  * an index is maintained on every write unconditionally; a Cabin's write
    hook is an in-memory directory probe that appends only for observed
    values and is free to decline (un-observing is always legal).
  * an index is durable and rebuilt by recovery; a Cabin v1 is memory-resident
    and does not survive a restart.

So compare the pairs, not the mechanisms: reporting `profit-scan` against
`profit-scan`, and the TPS beside it, on both engines with and without. A
structure that speeds the reader by slowing the writer is judged on both
numbers, which is why this scenario - one reader and one writer contending on
one relation - is where either belongs.

Usage:
    ./tools/pg_setup.sh init                      # scratch cluster on :15433
    python3 tools/pg_scenario0_stockmarket.py --port 15433 --database bench
    python3 tools/pg_scenario0_stockmarket.py --users 10000 --assets 10000 --seconds 120
    python3 tools/pg_scenario0_stockmarket.py --traders 4 --json pg.json
    python3 tools/pg_scenario0_stockmarket.py --no-profit    # OLTP alone, for the delta
    python3 tools/pg_scenario0_stockmarket.py --user-id-index

    # the cross-engine comparison, on a fresh data file / fresh cluster:
    python3 tools/scenario0_stockmarket.py    --json ckdbs.json
    python3 tools/pg_scenario0_stockmarket.py --json pg.json --synchronous-commit on

Durability is not silently different, and it is the one setting that decides
whether the comparison means anything. ckdbs INSERT is WAL-logged and its
UPDATE is logged as of the transaction work, so `synchronous_commit = on`
(PostgreSQL's default and this tool's) is the analogue of
`durability = strict|group`, and `off` of `durability = relaxed`. Say which
you mean, and do not measure either engine on tmpfs, where fsync is free and
every durability class looks identical.

Each run creates its own five tables, suffixed `_<epoch>_<rand>`, and drops
them on exit unless `--keep` - the suffix exists on the ckdbs side because
there is no DROP TABLE, and is kept here so a shared cluster can be run
repeatedly and both tools name their relations the same way.
"""

import argparse
import multiprocessing
import queue
import random
import sys
import time

from bench_common import Phase, report, write_json
from pg_benchmark import server_side_us
from pg_wire import DEFAULT_HOST, PgConnection, PgError

# The scenario's own constants and its two backend-agnostic functions come
# from the ckdbs driver rather than being restated here: an opening balance
# or a business clock that differed by a digit between the two runs would be
# invisible in both reports and would invalidate every number in them.
from scenario0_stockmarket import (ASSET_CLASSES, COUNTRIES, CREATE_ORDER,
                             OPENING_BALANCE, SIDE_BUY, SIDE_SELL,
                             merge_phase, sim_day)

# ---- schema --------------------------------------------------------------
#
# Column 0 of every relation is the generated id, matching ckdbs's Keystone
# primary key: system-generated, never supplied on INSERT. Types are the
# closest PostgreSQL equivalents of ckdbs's int64 / int32 / varchar, and the
# column *order* is identical, which is what lets both drivers read a
# `SELECT *` row back by the same field indexes.
#
#     int64   -> bigint          int32 -> integer
#     varchar -> varchar(64)     (ckdbs's inline cell width, kds.inline_cell_width)
#
# The second element is the ckdbs clustered_type this relation uses, mapped
# here to whether id carries a PRIMARY KEY.

SCHEMA = {
    # BTREE: every account access in the transaction is `WHERE id = <n>`.
    "accounts": (
        "user_id bigint, balance bigint, asset_qty bigint, "
        "trade_count bigint, opened_day integer", "BTREE"),
    "users": (
        "name varchar(64), country varchar(64), tier integer, "
        "created_day integer", "BTREE"),
    "assets": (
        "symbol varchar(64), asset_class integer, last_price bigint", "BTREE"),
    # HEAP: insert-only, appended at the end of the heap, never probed by pk.
    "trades": (
        "account_id bigint, asset_id bigint, side integer, qty bigint, "
        "price bigint, trade_day integer", "HEAP"),
    "user_periodic_profit": (
        "user_id bigint, period_day integer, realized bigint, "
        "trade_count bigint", "HEAP"),
}

# The column the reporting job filters on - `--user-id-index` puts a btree on
# exactly this one, the same column `--cabin` gives a Cabin on the ckdbs side.
INDEX_RELATION, INDEX_COLUMN = "accounts", "user_id"

# ---- the foreign key ------------------------------------------------------
#
# `--fk` declares the same relationship the ckdbs run declares under its own
# `--fk`: every trade leg names the account it was executed against.
FK_CHILD, FK_COLUMN, FK_PARENT = "trades", "account_id", "accounts"

# **Three differences from ckdbs worth knowing before comparing the two.**
#
#   - PostgreSQL's default action is NO ACTION, checked at the end of the
#     statement; ckdbs checks immediately and calls it RESTRICT. On this
#     workload - single-row inserts, no deferred constraints - they do the
#     same work at the same moment.
#   - PostgreSQL takes a `KEY SHARE` row lock on the parent for each check,
#     so two traders inserting against one account serialize briefly on it.
#     ckdbs takes no lock: a check that meets an in-flight writer fails fast
#     and retryably (docs/spec/foreign-keys.md F3). This scenario gives each
#     trader a disjoint account partition, so the lock is uncontended here -
#     which is a property of the workload, not of either engine.
#   - Neither engine indexes the *child* column for it. That only matters
#     for the reverse check, which this workload never triggers: nothing
#     deletes an account.

# Field positions in a `SELECT * FROM accounts` row, counting the generated
# id as 0. Named because both drivers depend on them and a schema edit that
# moved a column would otherwise fail as a wrong number rather than as an
# error.
ACC_BALANCE, ACC_QTY, ACC_TRADES = 2, 3, 4


def abort(message, reply=None):
    print(f"pg stress aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


# ---- --echo: every statement, as it is sent ------------------------------
#
# scenario0_stockmarket.py's, verbatim in behaviour, so a transcript from the two
# engines can be diffed. Off by default and not free: a write per statement,
# on a tool whose unit is statements per second.
ECHO = False
ECHO_TAG = "main"
ECHO_REPLY_MAX = 96


def set_echo(enabled, tag=None):
    global ECHO, ECHO_TAG
    if enabled is not None:
        ECHO = bool(enabled)
    if tag is not None:
        ECHO_TAG = tag


def echo_query(command, reply):
    """One `<tag> <statement>  ->  <reply>` line on **stderr**, so the report
    and --json stay pipeable with the transcript on."""
    if not ECHO:
        return
    text = str(reply)
    shown = text if len(text) <= ECHO_REPLY_MAX else text[:ECHO_REPLY_MAX] + "..."
    print(f"[{ECHO_TAG}] {command}  ->  {shown}", file=sys.stderr, flush=True)


class Client:
    """One connection plus the one-command-one-reply callable everything below
    is written against - the same surface scenario0_stockmarket.Client presents, so
    the trader and reporter bodies are the same code shape on both engines.

    Counts errors, so a caller that does not inspect every reply still cannot
    report a clean run over a failing one."""

    def __init__(self, args):
        try:
            self._conn = PgConnection(args.host, args.port, args.user,
                                      args.database, args.password,
                                      timeout=args.timeout,
                                      application_name="pg_scenario0_stockmarket.py")
        except (OSError, PgError) as e:
            abort(f"could not connect to {args.host}:{args.port}/{args.database}: {e}\n"
                  f"  start one with: ./tools/pg_setup.sh init")
        self.errors = 0
        self.first_error = None
        if args.synchronous_commit:
            reply = self(f"SET synchronous_commit = {args.synchronous_commit}")
            if reply.startswith("ERR"):
                abort("SET synchronous_commit failed", reply)

    def __call__(self, command):
        reply = self._conn.send_command(command)
        echo_query(command, reply)
        if reply.startswith("ERR"):
            self._note(command, reply)
        return reply

    def timed(self, command, phase):
        """Sends `command`, charging its round trip to `phase`."""
        t0 = time.perf_counter()
        reply = self(command)
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def rows(self, command):
        """Sends `command` and returns its rows as lists of raw column bytes,
        or None if the server answered with an error."""
        # Unlike the ckdbs client this class has **three** send paths, not
        # one - send_command, fetch and scalar - so --echo hooks each of
        # them rather than one choke point. A path added later without the
        # hook is a statement missing from the transcript.
        result, error = self._conn.fetch(command)
        if error is not None:
            echo_query(command, f"ERR {error}")
            self._note(command, f"ERR {error}")
            return None
        echo_query(command, f"ROWS {len(result)}")
        return result

    def timed_rows(self, command, phase):
        t0 = time.perf_counter()
        result = self.rows(command)
        phase.record(time.perf_counter() - t0,
                     "ERR" if result is None else f"ROWS {len(result)}")
        return result

    def scalar(self, command):
        try:
            value = self._conn.scalar(command)
        except PgError as e:
            echo_query(command, f"ERR {e}")
            self._note(command, f"ERR {e}")
            return None
        echo_query(command, value)
        return value

    def _note(self, command, reply):
        self.errors += 1
        if self.first_error is None:
            self.first_error = f"{command}  ->  {reply}"

    def close(self):
        self._conn.close()


# ---- load ----------------------------------------------------------------

def create_tables(client, suffix, want_index=False, fk=False):
    # CREATE_ORDER puts `accounts` before `trades`, which the foreign key
    # depends on rather than merely reads better: the parent has to exist
    # before the child may reference it.
    for base in CREATE_ORDER:
        columns, clustered = SCHEMA[base]
        if fk and base == FK_CHILD:
            columns = columns.replace(
                f"{FK_COLUMN} bigint",
                f"{FK_COLUMN} bigint REFERENCES {FK_PARENT}_{suffix}(id)", 1)
        identity = "id bigint GENERATED ALWAYS AS IDENTITY"
        if clustered == "BTREE":
            identity += " PRIMARY KEY"
        reply = client(f"CREATE TABLE {base}_{suffix} ({identity}, {columns})")
        if reply.startswith("ERR"):
            abort(f"could not create {base}_{suffix}", reply)

    if want_index:
        table = f"{INDEX_RELATION}_{suffix}"
        reply = client(f"CREATE INDEX {table}_{INDEX_COLUMN}_idx "
                       f"ON {table} ({INDEX_COLUMN})")
        if reply.startswith("ERR"):
            abort(f"could not index {table}.{INDEX_COLUMN}", reply)


def drop_tables(client, suffix):
    # Reverse creation order, which `--fk` makes load-bearing: a parent may
    # not be dropped while a child still references it. Without the flag,
    # referential integrity here is a property of how the driver generates
    # ids and nothing enforces it.
    for base in reversed(CREATE_ORDER):
        client(f"DROP TABLE IF EXISTS {base}_{suffix}")


def index_report(client, suffix, wanted):
    """What the accounts relation's access actually looked like, from the
    server's own statistics.

    This is the counterpart of the ckdbs run's `SHOW CABINS` block: `idx_scan`
    against `seq_scan` is the honest analogue of a Cabin's hits against its
    misses, since it counts how the reporter's predicate was answered rather
    than what was declared."""
    table = f"{INDEX_RELATION}_{suffix}"
    # Statistics are snapshotted per transaction in a session; without this
    # the numbers could be as old as this connection's last statement.
    client("SELECT pg_stat_clear_snapshot()")
    rows = client.rows(
        "SELECT seq_scan, seq_tup_read, idx_scan, idx_tup_fetch "
        f"FROM pg_stat_user_tables WHERE relname = '{table}'")
    if not rows:
        return None

    def number(value):
        return int(value) if value is not None else 0

    seq_scan, seq_tup, idx_scan, idx_tup = (number(v) for v in rows[0][:4])
    size = client.scalar(f"SELECT pg_total_relation_size('{table}')")
    return {
        "index": wanted,
        "seq_scan": seq_scan,
        "seq_tup_read": seq_tup,
        "idx_scan": idx_scan,
        "idx_tup_fetch": idx_tup,
        "total_bytes": int(size) if size is not None else None,
    }


def insert_returning_id(client, phase, command):
    """One INSERT ... RETURNING id, timed. The ckdbs server replies `id=<n>` to
    every INSERT whether asked or not, so the load phase needs RETURNING here
    to learn the same thing - and pays for the RowDescription/DataRow it costs.
    The trader's own trade inserts do *not* use it: nothing reads a trade back,
    and a real application would not ask."""
    t0 = time.perf_counter()
    rows = client.rows(command)
    phase.record(time.perf_counter() - t0, "ERR" if rows is None else "OK")
    if not rows or rows[0][0] is None:
        return None
    return int(rows[0][0])


def load_users(client, table, count, rng, phase):
    """Users are the ids the server hands back. Read back rather than assumed:
    identity values are ascending but not gapless - a failed insert burns one -
    so a run that hits an error must not go on addressing users by ordinal."""
    ids = []
    for i in range(count):
        name = f"user{i:07d}"
        got = insert_returning_id(
            client, phase,
            f"INSERT INTO {table} (name, country, tier, created_day) VALUES "
            f"('{name}', '{rng.choice(COUNTRIES)}', {rng.randint(0, 3)}, 0) "
            f"RETURNING id")
        if got is not None:
            ids.append(got)
    return ids


def load_assets(client, table, count, rng, phase):
    ids = []
    for i in range(count):
        symbol = f"SYM{i:06d}"
        got = insert_returning_id(
            client, phase,
            f"INSERT INTO {table} (symbol, asset_class, last_price) VALUES "
            f"('{symbol}', {rng.randint(0, ASSET_CLASSES - 1)}, "
            f"{rng.randint(1_000, 5_000_000)}) RETURNING id")
        if got is not None:
            ids.append(got)
    return ids


def load_accounts(client, table, user_ids, per_user, rng, phase):
    """One row per account, `per_user` of them per user on average.

    Returns [(account_id, user_id)] in creation order. The count per user is
    randomized around `per_user` for the same reason as on the ckdbs side: a
    fan-out where every user has the same number of accounts makes the
    reporting job's per-user read cost constant, and the variance is the more
    interesting half of that measurement."""
    accounts = []
    for user_id in user_ids:
        n = max(1, rng.randint(max(1, per_user - 1), per_user + 1))
        for _ in range(n):
            got = insert_returning_id(
                client, phase,
                f"INSERT INTO {table} (user_id, balance, asset_qty, "
                f"trade_count, opened_day) VALUES ({user_id}, "
                f"{OPENING_BALANCE}, 0, 0, 0) RETURNING id")
            if got is not None:
                accounts.append((got, user_id))
    return accounts


# ---- the trader process --------------------------------------------------

def trader_process(index, args, suffix, accounts, asset_ids, started_at, result_q,
                   target=0, stop_event=None, progress=None):
    """Runs 4-statement business transactions against a disjoint account
    partition until the run's wall clock is up, or until `target` transactions
    have committed - whichever comes first.

    Statement for statement the same body as scenario0_stockmarket.trader_process;
    only the connection underneath it differs. `target` is this trader's own
    share of `--txn-per-user x --users`, not the run's total; 0 is unlimited,
    the time-based default.

    `accounts` is this trader's partition only. Disjointness is what makes the
    in-memory balance authoritative: no other process updates these rows, so
    the driver's number and the stored number can only diverge through an
    error it saw, which is what --verify checks."""
    set_echo(getattr(args, "echo", False), f"trader-{index}")
    rng = random.Random(args.seed + 1000 + index)
    trades = f"trades_{suffix}"
    accounts_table = f"accounts_{suffix}"
    txn = bool(getattr(args, "txn", False))

    client = Client(args)

    insert_phase = Phase("trade-insert")
    update_phase = Phase("account-update")
    txn_phase = Phase("txn")

    # (account_id) -> [balance, asset_qty, trade_count]. Seeded from the load,
    # then owned entirely by this process.
    state = {aid: [OPENING_BALANCE, 0, 0] for aid, _ in accounts}
    ids = [aid for aid, _ in accounts]

    committed = torn = rejected = rolled_back = 0
    deadline = started_at + args.seconds
    started_running = time.perf_counter()

    try:
        while time.time() < deadline:
            # The work target, checked at the top of the iteration so the count
            # is exact rather than "the first check after". `torn` deliberately
            # does not count toward it: the target is *committed* transactions,
            # and a partial application is not one.
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
                # skipping it: a skipped iteration is a transaction that never
                # happened, and counting it as anything would bias the TPS.
                qty = max(1, state[buyer][0] // price)
                notional = price * qty
                if state[buyer][0] < notional:
                    rejected += 1
                    continue

            t0 = time.perf_counter()
            ok = True

            # The two balance moves, computed before anything is sent.
            # Literal values, not `balance = balance - <n>`: see fidelity
            # note 2 in the module docstring.
            b_bal = state[buyer][0] - notional
            b_qty = state[buyer][1] + qty
            b_cnt = state[buyer][2] + 1
            s_bal = state[seller][0] + notional
            s_qty = state[seller][1] - qty
            s_cnt = state[seller][2] + 1

            # (statement, phase, the in-memory effect it earns)
            steps = (
                (f"INSERT INTO {trades} (account_id, asset_id, side, qty, price, "
                 f"trade_day) VALUES ({buyer}, {asset}, {SIDE_BUY}, {qty}, "
                 f"{price}, {day})", insert_phase, None),
                (f"INSERT INTO {trades} (account_id, asset_id, side, qty, price, "
                 f"trade_day) VALUES ({seller}, {asset}, {SIDE_SELL}, {qty}, "
                 f"{price}, {day})", insert_phase, None),
                (f"UPDATE {accounts_table} SET balance = {b_bal}, "
                 f"asset_qty = {b_qty}, trade_count = {b_cnt} WHERE id = {buyer}",
                 update_phase, (buyer, [b_bal, b_qty, b_cnt])),
                (f"UPDATE {accounts_table} SET balance = {s_bal}, "
                 f"asset_qty = {s_qty}, trade_count = {s_cnt} WHERE id = {seller}",
                 update_phase, (seller, [s_bal, s_qty, s_cnt])),
            )

            legs_written = 0
            pending = []

            # Untimed but inside the window, exactly as the ckdbs driver
            # does it, so the two runs' per-statement phases stay
            # comparable and the transaction latency carries the commit.
            if txn:
                client("BEGIN")

            for index_of_step, (sql, phase, effect) in enumerate(steps):
                reply = client.timed(sql, phase)
                if reply.startswith("ERR"):
                    ok = False
                    if txn:
                        # PostgreSQL aborts the whole transaction on the
                        # first error (25P02: every later statement is
                        # refused until ROLLBACK), the same shape ckdbs's
                        # failed-transaction gate has.
                        break
                    continue
                if index_of_step < 2:
                    legs_written += 1
                if effect is None:
                    continue
                if txn:
                    # All or nothing: the driver's balance may only move
                    # once the server has committed the move.
                    pending.append(effect)
                else:
                    # Applied as each statement is accepted, so a failure
                    # leaves the driver's balances matching what is stored.
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
                # unsynchronized double store per transaction; the traders are
                # symmetric, so any one of them is representative and a torn
                # read costs a simulated day, not a row.
                if progress is not None and target:
                    progress.value = min(1.0, committed / target)
                txn_phase.record(latency, "OK")
            elif txn:
                # Nothing was applied: the server unwound it. A failed
                # transaction, never a torn one.
                txn_phase.record(latency, "ERR rolled back")
            else:
                # A partial application. These four statements are four
                # transactions as this tool sends them by default - fidelity
                # note 1 - so there is nothing to roll back and the only
                # honest thing to do is count it and keep going.
                if legs_written:
                    torn += 1
                txn_phase.record(latency, "ERR partial")
    except (ConnectionError, OSError, PgError) as e:
        result_q.put({"index": index, "fatal": str(e)})
        return
    finally:
        elapsed = time.perf_counter() - started_running
        # Tell the reporting process the measured window is over. Without this
        # a count-targeted run that finishes in 40s of a 600s budget would
        # leave the reporter reading for another 560s, and the run would take
        # as long as its *ceiling* rather than as long as its work.
        if stop_event is not None:
            stop_event.set()
        try:
            client.close()
        except OSError:
            pass

    for phase in (insert_phase, update_phase, txn_phase):
        phase.elapsed = elapsed

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
    """The 'other process': periodic per-user profit, written while the traders
    are writing trades.

    Reads with `SELECT * FROM accounts WHERE user_id = <n>` - a non-pk
    equality. Without `--user-id-index` the planner has nothing to use and it
    is a seqscan of the whole accounts relation per user, which is what a
    reporting job on an unindexed foreign key costs and is exactly the shape
    the ckdbs run's FilterScan has.

    Its unit of work is a *period*: one row per sampled user per
    `--profit-period-days` of simulated time. Wall-clock ticks
    (`--profit-interval`) only decide how often it checks whether a period
    boundary has been crossed."""
    set_echo(getattr(args, "echo", False), "reporter")
    rng = random.Random(args.seed + 7777)
    accounts_table = f"accounts_{suffix}"
    profit_table = f"user_periodic_profit_{suffix}"

    client = Client(args)

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
                account_rows = client.timed_rows(
                    f"SELECT * FROM {accounts_table} WHERE user_id = {user_id}",
                    scan_phase)
                if account_rows is None:
                    continue

                total_balance = 0
                total_trades = 0
                for row in account_rows:
                    total_balance += int(row[ACC_BALANCE])
                    total_trades += int(row[ACC_TRADES])

                # Realized profit = cash now, less the cash this user's
                # accounts opened with. Positions (asset_qty) are excluded on
                # purpose: marking them would need each asset's price, which is
                # a second relation read per row and would make this a
                # different measurement.
                realized = total_balance - len(account_rows) * OPENING_BALANCE
                reply = client.timed(
                    f"INSERT INTO {profit_table} (user_id, period_day, realized, "
                    f"trade_count) VALUES ({user_id}, {period_day}, {realized}, "
                    f"{total_trades})", write_phase)
                if not reply.startswith("ERR"):
                    rows += 1

            periods += 1
            next_period = period_day + args.profit_period_days
    except (ConnectionError, OSError, PgError) as e:
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

def verify_balances(client, table, states, sample_size, rng):
    """Reads a sample of accounts back and compares stored balance /
    asset_qty / trade_count against what the traders believe they wrote.

    The check that matters is not "is arithmetic right" - it is whether an
    update was lost. With disjoint partitions and one autocommitted statement
    per update, a mismatch means either a torn transaction or a statement the
    driver counted as applied that was not."""
    combined = {}
    for state in states:
        combined.update(state)
    if not combined:
        return 0, 0, None
    ids = rng.sample(list(combined), min(sample_size, len(combined)))
    checked = mismatched = 0
    first = None
    for account_id in ids:
        rows = client.rows(f"SELECT * FROM {table} WHERE id = {account_id}")
        if not rows:
            continue
        row = rows[0]
        checked += 1
        stored = (int(row[ACC_BALANCE]), int(row[ACC_QTY]), int(row[ACC_TRADES]))
        expected = tuple(combined[account_id])
        if stored != expected:
            mismatched += 1
            if first is None:
                first = (f"account {account_id}: stored "
                         f"balance/qty/trades {stored}, driver expected {expected}")
    return checked, mismatched, first


def print_index(meta):
    """How the accounts relation was actually read, which is the counterpart of
    the ckdbs run's cabin block.

    Printed whether or not an index was asked for, and printed even when it is
    unflattering: a run whose planner preferred a seqscan despite the index is
    a true statement about the workload rather than a failure of the tool."""
    stats = meta.get("index_state")
    column = f"{INDEX_RELATION}.{INDEX_COLUMN}"
    if not meta.get("user_id_index"):
        print(f"  index               none on {column} (--user-id-index creates one); "
              f"the reporting scan above is a seqscan")
    else:
        print(f"  index               btree on {column} - the counterpart of the ckdbs "
              f"run's --cabin, not an equivalent of it")
    if stats is None:
        print("                      (pg_stat_user_tables had no row for this relation)")
        print()
        return

    total = stats["seq_scan"] + stats["idx_scan"]
    served = (stats["idx_scan"] / total * 100.0) if total else 0.0
    print(f"    index / seq scans {stats['idx_scan']:>12,} / {stats['seq_scan']:,}   "
          f"{served:.1f}% of accounts accesses used an index")
    print(f"    tuples read       {stats['idx_tup_fetch']:>12,} by index, "
          f"{stats['seq_tup_read']:,} by seqscan")
    if stats["total_bytes"] is not None:
        print(f"    relation + index  {stats['total_bytes'] / (1 << 20):>12,.2f} MB   "
              f"pg_total_relation_size")
    print("                      every UPDATE of accounts above maintained the pk index, "
          "and the user_id one when present")
    if meta.get("user_id_index") and stats["idx_scan"] == 0:
        # The counterpart of the ckdbs block's "no probe was ever repeated":
        # the structure exists, was maintained on every write, and answered
        # nothing. Said plainly, because the flag alone would read as a claim.
        print(f"                      the planner never chose an index: at "
              f"{meta['accounts']:,} accounts a seqscan wins, so this run paid the "
              f"index's write cost and got none of its read benefit. Raise --users to "
              f"measure the index path.")
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
        print(f"  torn                {torn:>12,}   partially applied; the four "
              f"statements are four transactions here (--txn groups them)")
    print(f"  underfunded         {rejected:>12,}   skipped before any statement was sent")
    if meta["traders"] > 1:
        per = "  ".join(f"#{r['index']}:{r['committed']:,}" for r in sorted(
            trader_results, key=lambda r: r["index"]))
        print(f"  per trader          {per}")
    print()

    print_index(meta)

    if profit_result:
        stats = meta.get("index_state")
        # What the planner did, not what the flag asked for - `--user-id-index`
        # on a small relation is still a seqscan, and claiming otherwise here
        # would describe the run as it was requested rather than as it ran.
        used_index = bool(stats and stats["idx_scan"])
        print(f"  reporting process   {profit_result['periods']:>12,} periods, "
              f"{profit_result['rows']:,} user_periodic_profit rows written")
        print("                      "
              + ("one read of accounts per user per period, index scans available"
                 if used_index else
                 "one seqscan of accounts per user per period"))
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
    parser.add_argument("--port", type=int, default=15433,
                        help="default: 15433, the scratch cluster tools/pg_setup.sh builds")
    parser.add_argument("--user", default=None, help="default: $PGUSER, else $USER")
    parser.add_argument("--database", default="bench", help="default: bench")
    parser.add_argument("--password", default=None, help="default: $PGPASSWORD")
    parser.add_argument("--suffix", default=None,
                        help="table-name suffix; default <epoch>_<rand>, i.e. fresh "
                             "relations per run, named as scenario0_stockmarket.py names them")

    parser.add_argument("--users", type=int, default=10000,
                        help="rows in users (default: 10000)")
    parser.add_argument("--assets", type=int, default=10000,
                        help="rows in assets (default: 10000)")
    parser.add_argument("--accounts-per-user", type=int, default=2,
                        help="average accounts per user (default: 2); the actual count "
                             "per user varies by +-1 so the reporting job's per-user "
                             "read is not a constant")
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
                        help="seconds between reporter wake-ups (default: 1.0); it only "
                             "checks whether a simulated period boundary passed")
    parser.add_argument("--profit-period-days", type=int, default=7,
                        help="simulated days per reporting period (default: 7)")
    parser.add_argument("--profit-users", type=int, default=50,
                        help="users sampled per period (default: 50); each costs one "
                             "read of accounts plus one INSERT")

    parser.add_argument("--txn-per-user", type=float, default=0.0, metavar="N",
                        help="stop after N x --users transactions have committed, "
                             "instead of running for the whole --seconds. 0 (default) "
                             "keeps the time-based behaviour. --seconds becomes a "
                             "ceiling, and TPS is reported over the time the work "
                             "actually took")

    parser.add_argument("--echo", dest="echo", action="store_true", default=False,
                        help="print every statement this tool sends, and its reply, to "
                             "stderr as `[<who>] <statement>  ->  <reply>`. The same "
                             "flag and the same line format scenario0_stockmarket.py has, so "
                             "the two transcripts can be diffed. Off by default: it "
                             "costs a write per statement on a tool that measures "
                             "statements per second")

    parser.add_argument("--txn", dest="txn", action="store_true", default=False,
                        help="wrap each business transaction's four statements in "
                             "BEGIN/COMMIT instead of sending them as four autocommit "
                             "statements. Default off, matching the ckdbs run's "
                             "default. It drives `torn` to zero by construction, and "
                             "it is the same lever on both engines: four commits "
                             "become one")
    parser.add_argument("--no-txn", dest="txn", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--fk", dest="fk", action="store_true", default=False,
                        help=f"declare a foreign key on {FK_CHILD}.{FK_COLUMN} -> "
                             f"{FK_PARENT}(id), the same relationship the ckdbs run's "
                             f"--fk declares. Every trade leg then checks the account "
                             f"it names before the row is written. Default off. Note "
                             f"PostgreSQL takes a KEY SHARE row lock on the parent per "
                             f"check where ckdbs takes none - see the note beside "
                             f"FK_CHILD for the three differences that matter when "
                             f"comparing the two runs")
    parser.add_argument("--no-fk", dest="fk", action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--user-id-index", "--cabin", dest="user_id_index",
                        action="store_true", default=False,
                        help=f"create a btree index on {INDEX_RELATION}.{INDEX_COLUMN}: "
                             f"the reporting job's `WHERE {INDEX_COLUMN} = <n>` is then "
                             f"an index scan instead of a seqscan. Default off, so the "
                             f"baseline matches the ckdbs run's default. This is the "
                             f"counterpart of that run's --cabin and NOT an equivalent "
                             f"of it - see the module docstring. It is not free on the "
                             f"write side either: every account UPDATE maintains it, so "
                             f"read TPS and profit-scan together")
    parser.add_argument("--no-user-id-index", "--no-cabin", dest="user_id_index",
                        action="store_false",
                        help="the default; stated so a script can be explicit")

    parser.add_argument("--synchronous-commit",
                        choices=["on", "off", "local", "remote_write"], default=None,
                        help="SET synchronous_commit for every connection this run "
                             "opens. Unset leaves the server default (on): every "
                             "statement waits for a WAL fsync, the analogue of ckdbs "
                             "durability = strict|group. off is the analogue of relaxed")
    parser.add_argument("--analyze", dest="analyze", action="store_true", default=True,
                        help="ANALYZE the loaded tables before the measured phase "
                             "(default: on); without it a freshly loaded table may be "
                             "seqscanned despite --user-id-index")
    parser.add_argument("--no-analyze", dest="analyze", action="store_false",
                        help="skip it, and measure whatever the planner picks cold")

    parser.add_argument("--verify", type=int, default=200,
                        help="accounts read back and compared against the drivers' "
                             "in-memory balances after the run (default: 200); 0 skips")
    parser.add_argument("--seed", type=int, default=1, help="RNG seed (default: 1)")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="socket timeout in seconds (default: 120); a seqscan over "
                             "a large accounts relation is one slow reply")
    parser.add_argument("--sync", action="store_true",
                        help="issue CHECKPOINT after the run and time it, the "
                             "counterpart of ckdbs's SYNC; needs a superuser role")
    parser.add_argument("--keep", action="store_true",
                        help="do not DROP the run's five tables on exit")
    parser.add_argument("--json", metavar="PATH", help="also write results as JSON")
    parser.add_argument("--server-log", metavar="PATH",
                        help="the cluster's log file: adds the per-statement "
                             "microseconds PostgreSQL itself measured, with "
                             "log_min_duration_statement = 0 "
                             "(./tools/pg_setup.sh timing on)")
    args = parser.parse_args()

    # Before anything is sent, so the DDL and the load are in the transcript
    # too. The trader and reporter processes inherit it and re-tag.
    set_echo(args.echo, "loader")

    if args.traders < 1:
        abort("--traders must be at least 1")
    if args.days < 1:
        abort("--days must be at least 1")

    suffix = args.suffix or f"{time.time_ns() // 1_000_000_000}_{random.randrange(1 << 16)}"
    rng = random.Random(args.seed)

    loader = Client(args)
    server_version = (loader.scalar("SHOW server_version") or b"?").decode()
    sync_commit = (loader.scalar("SHOW synchronous_commit") or b"?").decode()

    # ---- load ------------------------------------------------------------
    print(f"loading: users={args.users:,} assets={args.assets:,} "
          f"accounts~{args.users * args.accounts_per_user:,}  "
          f"(tables suffixed _{suffix})"
          + (f"  [index on {INDEX_RELATION}.{INDEX_COLUMN}]"
             if args.user_id_index else "")
          + (f"  [fk {FK_CHILD}.{FK_COLUMN} -> {FK_PARENT}]" if args.fk else "")
          + ("  [BEGIN/COMMIT per transaction]" if args.txn else ""), flush=True)

    create_tables(loader, suffix, args.user_id_index, args.fk)

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

    if args.analyze:
        # Untimed: it is setup, and ckdbs has no planner statistics to build,
        # so charging it to a phase would invent a cost the other side cannot
        # have. Without it the planner may seqscan a freshly loaded table and
        # --user-id-index would look like it bought nothing.
        for base in CREATE_ORDER:
            loader(f"ANALYZE {base}_{suffix}")

    # ---- run -------------------------------------------------------------
    #
    # Partitioned round-robin rather than in contiguous blocks, so every
    # trader's accounts are spread across the whole relation rather than one
    # trader owning the low ids and another the high ones.
    partitions = [accounts[i::args.traders] for i in range(args.traders)]

    result_q = multiprocessing.Queue()
    started_at = time.time()

    # The work target, split across traders. Split rather than shared: a shared
    # counter would need a lock on the hot path, and the partitions are already
    # equal-sized, so an equal split is the same total.
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
    # sample and the whole balance state, which is far more than a pipe buffer
    # holds: the child's feeder thread blocks until the parent reads, and the
    # child cannot exit while it blocks. Joining first is therefore a deadlock,
    # not a race - it hangs every time, not occasionally.
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
        if not args.keep:
            drop_tables(loader, suffix)
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
    # count-targeted run that hit its target in 40s of a 600s ceiling must be
    # divided by 40 or the number is meaningless.
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
            merged.detail = ("heap append into an index-free relation, WAL-logged, one "
                             "commit per statement")
        else:
            merged.detail = ("new row version after a pk index scan (MVCC), WAL-logged, "
                             "one commit per statement")
        phases.append(merged)

    if profit_result:
        for name in ("profit-scan", "profit-insert"):
            merged = merge_phase(name, [profit_result["phases"][name]], wall)
            if name == "profit-scan":
                merged.detail = (
                    f"index scan: WHERE {INDEX_COLUMN} = <n> over the btree on "
                    f"{INDEX_RELATION}.{INDEX_COLUMN}"
                    if args.user_id_index else
                    f"seqscan: WHERE {INDEX_COLUMN} = <n>, a non-pk equality with no "
                    f"index, so the whole accounts relation is read per user")
                merged.detail += "; values are decoded client-side, unlike the " \
                                 "benchmark phases"
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
        sync_reply = loader("CHECKPOINT")
        sync_phase = Phase("checkpoint", "server-wide checkpoint; needs a superuser role")
        sync_phase.record(time.perf_counter() - t0, sync_reply)
        sync_phase.elapsed = sync_phase.latencies[0]
        phases.append(sync_phase)

    # Read *after* the run, so the numbers describe what the workload actually
    # did rather than what the load phase left behind.
    index_state = index_report(loader, suffix, args.user_id_index)

    trade_rows = None
    if args.users <= 200:
        value = loader.scalar(f"SELECT count(*) FROM trades_{suffix}")
        trade_rows = int(value) if value is not None else None

    committed = sum(r["committed"] for r in trader_results)
    meta = {
        "engine": f"PostgreSQL {server_version}",
        "scenario": "brokerage: users/accounts/assets/trades/user_periodic_profit",
        "host": args.host,
        "port": args.port,
        "database": args.database,
        "table": f"*_{suffix}",
        "suffix": suffix,
        # The generated id plus the body columns, matching how the ckdbs run
        # counts its own five relations' columns.
        "columns": sum(1 + len(SCHEMA[t][0].split(",")) for t in CREATE_ORDER),
        "rows": len(accounts),
        "users": len(user_ids),
        "accounts": len(accounts),
        "assets": len(asset_ids),
        "days": args.days,
        "seconds": round(wall, 3),
        "seconds_budget": args.seconds,
        # What ended the run. A target-limited run is a *fixed amount of work*
        # measured in time; a time-limited one is fixed time measured in work.
        # They are not the same experiment.
        "limit": ("transactions" if any(r.get("hit_target") for r in trader_results)
                  else "seconds"),
        "txn_target": total_target,
        "traders": args.traders,
        # One per trader, plus the reporting process's own, plus this loader -
        # which the ckdbs run also holds open, and counts the same way.
        "connections": args.traders + (1 if profit_result else 0),
        "profit": bool(profit_result),
        "profit_users": args.profit_users,
        # The index, as a pair: what was asked for, and what the server says
        # the workload actually did with it.
        "user_id_index": bool(args.user_id_index),
        "fk": bool(args.fk),
        "txn": bool(args.txn),
        "index_column": f"{INDEX_RELATION}.{INDEX_COLUMN}",
        "index_state": index_state,
        "seed": args.seed,
        "synchronous_commit": sync_commit,
        "analyze": args.analyze,
        "tps": round(committed / wall, 2) if wall > 0 else 0.0,
        "committed": committed,
        "torn": sum(r["torn"] for r in trader_results),
    }

    if not args.keep:
        drop_tables(loader, suffix)

    report(phases, meta, footer=[
        "the four statements are NOT one transaction as this tool runs them - four "
        "autocommits, matching scenario0_stockmarket.py statement for statement. PostgreSQL "
        "would happily wrap them; wrapping them would price the commit, not the "
        "statements, and would drive `torn` to zero.",
        "balances are computed client-side and sent as literals because ckdbs's UPDATE "
        "takes a literal, not an expression. `balance = balance - <n>` is expressible "
        "here and is deliberately not used; each trader owns a disjoint account "
        "partition so no update can be lost between them, and --verify reads a sample "
        "back to confirm it.",
        "money is int64 minor units (bigint cents), not numeric - the type ckdbs can "
        "store, and the faster of the two here.",
        f"synchronous_commit = {sync_commit}: "
        + ("every INSERT and UPDATE above waited for a WAL fsync, which is the "
           "analogue of ckdbs durability = strict|group. Do not measure this on tmpfs, "
           "where fsync is free."
           if sync_commit in ("on", "remote_apply") else
           "writes did NOT wait for a WAL fsync - the analogue of ckdbs "
           "durability = relaxed, and not a durable configuration."),
        (f"--user-id-index put a btree on {INDEX_RELATION}.{INDEX_COLUMN}: the "
         "reporter's non-pk equality is an index scan instead of a seqscan, and every "
         "account UPDATE maintains that index. It is the counterpart of the ckdbs run's "
         "--cabin, not an equivalent - an index is authoritative for every value and "
         "durable; a Cabin is authoritative only for observed values and lives in memory."
         if args.user_id_index else
         f"no index on {INDEX_RELATION}.{INDEX_COLUMN} (--user-id-index creates one): "
         "the reporter's `WHERE user_id = <n>` seqscans the whole accounts relation per "
         "user, per period, which is what a reporting job on an unindexed foreign key "
         "costs."),
        "accounts/users/assets carry a PRIMARY KEY and trades/user_periodic_profit carry "
        "none, mirroring the ckdbs run's BTREE/HEAP choice per relation - so neither "
        "engine is paying for an index the other does not have.",
        "PostgreSQL UPDATE writes a new row version and leaves dead tuples behind; "
        "autovacuum ran or did not run according to the cluster's settings, and no "
        "manual VACUUM was issued between phases.",
        "latencies include the Python client's own socket cost, as in every tool here.",
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
            by_kind = server_side_us(args.server_log, f"_{suffix}")
            if by_kind:
                print("  server-side, as measured by the server itself:")
                for kind in sorted(by_kind):
                    v = by_kind[kind]
                    print(f"    {kind:<8}{len(v):>8,} stmts  p50 {v[len(v) // 2]:>7.0f} us"
                          f"  p95 {v[int(len(v) * 0.95)]:>7.0f} us")
            else:
                print("  --server-log: no timed statements for this run "
                      "(run ./tools/pg_setup.sh timing on)")
        except OSError as e:
            print(f"  --server-log: could not read {args.server_log}: {e}")

    if args.json:
        write_json(args.json, meta, phases)

    loader.close()
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
