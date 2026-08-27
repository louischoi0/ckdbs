#!/usr/bin/env python3
"""What an unfiltered catalog read costs an ordinary statement (DT9).

`docs/spec/ddl-transactional.md` §5a's DT9 changed one arm of the catalog's
`ScanAll`: a delete-marked catalog row now counts as deleted only once its
deleter is no longer in flight, which costs a `TransactionManager::IsInFlight`
walk of `live_` per delete-marked row. The claim under test is that this is
**unmeasurable on an ordinary statement**, because `ScanAll` runs only on a
catalog-cache miss and the new branch runs only for delete-marked rows.

The claim is only checkable by an A/B across two binaries, and this driver is
built around the one thing that makes such an A/B honest on a small box:

**Interleaving, not sequence.** Two sequential runs of any driver here
disagree with themselves by more than the effect being measured. Every arm
therefore runs block by block across *both* servers inside one process, and
the block order of the two sides **alternates** - so an arm whose cost grows
with the catalog (`ddl-create` does, since nothing reclaims a catalog row)
does not systematically favour whichever side went first.

**A noise floor from inside the run.** `pk-select-again` is `pk-select`
repeated, same server, same statements, same relation. A delta smaller than
the gap between those two rows is not a finding.

**A control that cannot reach the changed code.** `ping` is `SHOW META`,
which resolves no relation, and `ins-plain` inserts into a relation carrying
no secondary index. A delta that shows up on those as well is the host, not
the engine.

---- The arms -------------------------------------------------------------

Reads run before writes and writes before DDL, because DDL grows the catalog
and every catalog-reading arm would then be measured against a different
catalog than the arm before it.

    ping             SHOW META - client + socket floor, no catalog read
    pk-select        clustered descent on the pk; no secondary structure
    pk-select-again  the same arm again - the in-run noise floor
    idx-probe        equality on the indexed column - kIndexProbe
    ins-idx          INSERT into the indexed relation - the write path that
                     reads the relation's index list (`InitTableAccess`)
    ins-plain        INSERT into the index-free relation - the control
    show-tables      SHOW TABLES - `ListTables()`, which is an **unfiltered
                     `ScanAll<SysObjectRow>` on every statement**. This is
                     the arm where DT9's branch is reached per statement
                     rather than per cache miss, and it is here so that a
                     cost the ordinary arms cannot see still has somewhere to
                     show up
    ddl-create       CREATE TABLE - the DDL arm: two unfiltered `ScanAll`s
                     (sys.tables for placement, sys.objects for the name)
                     plus the catalog writes
    ddl-cidx         CREATE INDEX on an *empty* relation, so the arm prices
                     the catalog path and not a backfill
    ddl-didx         DROP INDEX of the index the arm before it created
    txn-begin        BEGIN, inside a transaction that goes on to do DDL
    txn-create       CREATE TABLE inside that transaction
    txn-commit       the COMMIT that resolves it - **the arm carrying the
                     commit-side cache invalidation** (`EndDdlScope`), which
                     a committed DDL transaction did not pay before DT9
    txn-rollback     the same shape ended by ROLLBACK, which paid the
                     invalidation on both binaries and must not move
    drop-txn         DROP TABLE inside a transaction - the statement that
                     *creates* delete-marked catalog rows
    drop-commit      the COMMIT after it

---- The adversarial half: cold catalog, with delete-marks present ---------

The arms above all read the catalog through its cache, which is the reason
the hypothesis expects no cost at all. So the driver also builds the state
where DT9's branch is reached on a hot path and measures there.

**Delete-marked rows.** A transactional `DROP TABLE` delete-marks the
relation's `sys.tables` row and every one of its `sys.columns` rows, and
nothing purges them (txn.md's no-purge gap). `--marks N` therefore runs N
transactional drops per side and leaves ~6N delete-marked catalog rows
standing - each of which costs side B one `IsInFlight` call per unfiltered
scan and side A nothing. `DROP TABLE` is used rather than `DROP INDEX`
because it is the only transactional drop **both** binaries accept: the
baseline refuses `DROP INDEX` inside a transaction, which is what DT9
changed.

**A cold cache.** `cold-pk-select` and `cold-ins-idx` precede each timed
statement with an *untimed* `ALTER TABLE ... RENAME COLUMN`, which bumps the
catalog version and drops every cached fact. The timed statement then pays
`FindTableOidByName` + `GetSysTableRow` + `ScanSchemaFromColumns` +
`ListCabins` + `ListForeignKeys` + `ListIndexes` - five unfiltered `ScanAll`s
over the whole catalog, walking every delete-mark. RENAME COLUMN rather than
another `CREATE TABLE` because it adds no catalog rows, so the catalog the
arm reads is the same size at op 1 and op N.

**A populated `live_`.** `IsInFlight` is a linear walk of the core's live
transaction list, so its cost per delete-marked row is proportional to how
many transactions are open. `--live-txns K` parks K idle `BEGIN`s on their
own connections and repeats both cold arms as `cold-pk-select-live` /
`cold-ins-idx-live`. That is the worst case the structure admits, and it is
measured rather than reasoned about.

---- Server CPU -----------------------------------------------------------

`/proc/<pid>/stat` advances in whole scheduler ticks (10 ms here), so CPU is
measured in a **second pass** of contiguous windows - one window per (arm,
side) per round - rather than summed over the latency pass's small blocks,
where a +-1 tick error per block would swamp the signal. Pass the
`kds_server` pid, not a shell wrapper's.

---- Row-set size ---------------------------------------------------------

`--rows` **is** the row count: sweep 200 / 1000 / 10000, one invocation per
size against a fresh pair of servers and a fresh pair of data files. Nothing
here reclaims a catalog row, so a second invocation against one data file is
not a repeat of the first.

Flags, invocations and the PostgreSQL position: `bench/docs/README.md`.
"""

import argparse
import json
import os
import random
import re
import sys
import time

from bench_common import Phase
from ckdbs_cli import DEFAULT_HOST, ServerConnection, format_reply

COLUMNS = "id int64, cust_id int64, status int32, amount int64, ref varchar"
CLUSTERED = "BTREE"
STATUSES = 6
ROW_COUNT = re.compile(r"\((\d+) rows?\)")


def abort(message, reply=None):
    print(f"catalog_read_ab aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


ECHO = False


class Side:
    """One server: its connection, its relation names, its CPU meter."""

    def __init__(self, label, host, port, pid, suffix, timeout):
        self.label = label
        self.port = port
        self.pid = pid
        try:
            self._conn = ServerConnection(host, port, timeout=timeout)
        except OSError as e:
            abort(f"could not connect to {label} at {host}:{port}: {e}")
        self.orders = f"cra_ord_{suffix}"
        # The write arms get their own indexed relation, so `orders` stays
        # at exactly --rows for every read arm. An insert arm that grew the
        # relation the reads are measured against would move two variables.
        self.wins = f"cra_win_{suffix}"
        self.plain = f"cra_pln_{suffix}"
        self.scratch = f"cra_scr_{suffix}"
        self.index = f"ix_cra_cust_{suffix}"
        self.win_index = f"ix_cra_win_{suffix}"
        self.scratch_index = f"ix_cra_scr_{suffix}"
        self.errors = 0
        self.first_error = None
        self.host = host
        self.timeout = timeout
        self.idle = []
        self.renamed = False
        self.marks = 0

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            print(f"[{self.label}] {command}  ->  {reply[:110]}",
                  file=sys.stderr, flush=True)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def must(self, command):
        reply = self(command)
        if reply.startswith("ERR"):
            abort(f"{self.label}: {command}", reply)
        return reply

    def cpu_seconds(self):
        """utime + stime of the server process, in seconds, or None."""
        if self.pid is None:
            return None
        try:
            with open(f"/proc/{self.pid}/stat") as f:
                fields = f.read().rsplit(") ", 1)[1].split()
        except OSError:
            return None
        ticks = int(fields[11]) + int(fields[12])
        return ticks / os.sysconf("SC_CLK_TCK")

    def park_idle(self, n):
        """`n` connections each parked inside an open `BEGIN`.

        Nothing but `live_` cares that they exist: an idle transaction that
        wrote no DDL leaves `ViewFor` on its `nullopt` fast path for every
        other session, so the reads being measured stay unfiltered. What it
        changes is the length of the list `IsInFlight` walks.
        """
        for _ in range(n):
            conn = ServerConnection(self.host, self.port, timeout=self.timeout)
            reply = format_reply(conn.send_command("BEGIN"))
            if reply.startswith("ERR"):
                abort(f"{self.label}: could not park an idle transaction "
                      f"({len(self.idle)} parked)", reply)
            self.idle.append(conn)

    def release_idle(self):
        for conn in self.idle:
            try:
                conn.send_command("ROLLBACK")
            except OSError:
                pass
            conn.close()
        self.idle = []

    def invalidate(self):
        """One statement that drops every cached catalog fact and adds no
        catalog row: `BumpVersion` from a RENAME COLUMN, toggled between two
        names so the arm can be repeated indefinitely."""
        a, b = ("ref", "ref2") if not self.renamed else ("ref2", "ref")
        self.renamed = not self.renamed
        return self.must(f"ALTER TABLE {self.scratch} RENAME COLUMN {a} TO {b}")

    def close(self):
        self.release_idle()
        self._conn.close()


class Meter:
    """Phases and CPU totals, keyed by (side label, arm)."""

    def __init__(self):
        self.phases = {}
        self.cpu = {}

    def phase(self, side, arm):
        key = (side.label, arm)
        if key not in self.phases:
            self.phases[key] = Phase(f"{arm}[{side.label}]")
        return self.phases[key]

    def add_cpu(self, side, arm, seconds, ops):
        key = (side.label, arm)
        total_s, total_ops = self.cpu.get(key, (0.0, 0))
        self.cpu[key] = (total_s + seconds, total_ops + ops)


# ---- schema and load ------------------------------------------------------

def make_rows(rng, rows, customers):
    return [(rng.randrange(customers), rng.randrange(STATUSES),
             rng.randrange(1, 100000), f"r{rng.randrange(10**6):06d}")
            for _ in range(rows)]


def values(row):
    cust, status, amount, ref = row
    return f"{cust}, {status}, {amount}, '{ref}'"


def build(side, rows_data, batch):
    side.must(f"CREATE TABLE {side.orders} ({COLUMNS}) {CLUSTERED}")
    side.must(f"CREATE TABLE {side.wins} ({COLUMNS}) {CLUSTERED}")
    side.must(f"CREATE TABLE {side.plain} ({COLUMNS}) {CLUSTERED}")
    side.must(f"CREATE TABLE {side.scratch} ({COLUMNS}) {CLUSTERED}")
    side.must(f"CREATE INDEX {side.index} ON {side.orders} (cust_id)")
    side.must(f"CREATE INDEX {side.win_index} ON {side.wins} (cust_id)")
    for start in range(0, len(rows_data), batch):
        chunk = rows_data[start:start + batch]
        side.must("BEGIN")
        for row in chunk:
            side.must(f"INSERT INTO {side.orders} VALUES ({values(row)})")
            side.must(f"INSERT INTO {side.wins} VALUES ({values(row)})")
            side.must(f"INSERT INTO {side.plain} VALUES ({values(row)})")
        side.must("COMMIT")


def check_plan(side):
    """The index is declared *and descended*, or the run is a lie.

    A relation small enough for a chain walk to look fast is exactly where a
    plan that quietly regressed to a scan passes unnoticed."""
    reply = side.must(f"ANALYZE SELECT id, status FROM {side.orders} "
                      f"WHERE cust_id = 1")
    if "IndexProbe" not in reply:
        abort(f"{side.label}: the idx-probe shape did not compile to an "
              f"IndexProbe, so this run would price a chain walk under an "
              f"index heading", reply)
    return reply.replace("\n", " | ")


# ---- the passes -----------------------------------------------------------

def ordered(sides, block_no):
    """Sides in alternating order, so a growth-sensitive arm does not
    systematically favour whichever side is measured first."""
    return sides if block_no % 2 == 0 else list(reversed(sides))


def latency_pass(sides, meter, arms, block):
    for arm, per_side in arms:
        # `per_side` is a dict label -> list of already-formatted statements,
        # equal in length across sides.
        n = len(next(iter(per_side.values())))
        for i, start in enumerate(range(0, n, block)):
            for side in ordered(sides, i):
                phase = meter.phase(side, arm)
                for stmt in per_side[side.label][start:start + block]:
                    t0 = time.perf_counter()
                    reply = side(stmt)
                    phase.record(time.perf_counter() - t0, reply)


def cpu_pass(sides, meter, arms, rounds):
    """One contiguous window per (arm, side) per round.

    The window is the unit because /proc advances in whole ticks: two reads
    around a window cost at most one tick of error, and re-interleaving the
    rounds makes a machine that drifts cost every arm equally.
    """
    for r in range(rounds):
        for arm, per_side in arms:
            for side in ordered(sides, r):
                stmts = per_side[side.label]
                before = side.cpu_seconds()
                for stmt in stmts:
                    side(stmt)
                after = side.cpu_seconds()
                if before is not None and after is not None:
                    meter.add_cpu(side, arm, after - before, len(stmts))


# ---- statement generators -------------------------------------------------

def read_arms(sides, ops, rows, customers, rng):
    """Arms whose statements are identical modulo the relation name, and
    driven with the *same* arguments on both sides in the same position."""
    keys = [rng.randrange(1, rows + 1) for _ in range(ops)]
    custs = [rng.randrange(customers) for _ in range(ops)]
    out = []
    out.append(("ping", {s.label: ["SHOW META"] * ops for s in sides}))
    out.append(("pk-select", {
        s.label: [f"SELECT amount FROM {s.orders} WHERE id = {k}" for k in keys]
        for s in sides}))
    out.append(("pk-select-again", {
        s.label: [f"SELECT amount FROM {s.orders} WHERE id = {k}" for k in keys]
        for s in sides}))
    out.append(("idx-probe", {
        s.label: [f"SELECT id, status FROM {s.orders} WHERE cust_id = {c}"
                  for c in custs]
        for s in sides}))
    out.append(("show-tables", {s.label: ["SHOW TABLES"] * ops for s in sides}))
    return out


def write_arms(sides, ops, customers, rng):
    rows_data = make_rows(rng, ops, customers)
    return [
        ("ins-idx", {s.label: [f"INSERT INTO {s.wins} VALUES ({values(r)})"
                               for r in rows_data] for s in sides}),
        ("ins-plain", {s.label: [f"INSERT INTO {s.plain} VALUES ({values(r)})"
                                 for r in rows_data] for s in sides}),
    ]


def ddl_pass(sides, meter, ops, tag, timed):
    """create/drop/create/drop on one scratch relation, per side.

    Not routed through `latency_pass` because the two index arms have an
    ordering constraint the block interleave would break.
    """
    for i in range(ops):
        for side in ordered(sides, i):
            for arm, stmt in (
                    ("ddl-create",
                     f"CREATE TABLE cra_d_{side.label}_{tag}_{i} "
                     f"({COLUMNS}) {CLUSTERED}"),
                    ("ddl-cidx",
                     f"CREATE INDEX {side.scratch_index}_{tag}_{i} "
                     f"ON {side.scratch} (cust_id)"),
                    ("ddl-didx",
                     f"DROP INDEX {side.scratch_index}_{tag}_{i}")):
                if timed:
                    phase = meter.phase(side, arm)
                    t0 = time.perf_counter()
                    reply = side(stmt)
                    phase.record(time.perf_counter() - t0, reply)
                else:
                    side(stmt)


def ddl_cpu_pass(sides, meter, ops, rounds, tag_base):
    # CPU for the DDL arms is measured as one window over the whole
    # create/index/drop triple rather than per arm: the three are entangled
    # by construction (an index needs its relation) and splitting the window
    # three ways would put each below one scheduler tick.
    for r in range(rounds):
        for side in ordered(sides, r):
            tag = f"{tag_base}c{r}"
            before = side.cpu_seconds()
            for i in range(ops):
                side(f"CREATE TABLE cra_d_{side.label}_{tag}_{i} "
                     f"({COLUMNS}) {CLUSTERED}")
                side(f"CREATE INDEX {side.scratch_index}_{tag}_{i} "
                     f"ON {side.scratch} (cust_id)")
                side(f"DROP INDEX {side.scratch_index}_{tag}_{i}")
            after = side.cpu_seconds()
            if before is not None and after is not None:
                meter.add_cpu(side, "ddl-triple", after - before, ops)


def txn_ddl_pass(sides, meter, ops, tag):
    """`BEGIN` / `CREATE TABLE` / `COMMIT`, with all three timed separately.

    This is where a *commit-side* cost shows up. `EndDdlScope` invalidates
    the catalog cache when a transaction that wrote catalog rows resolves,
    and as of DT9's follow-up it does so on **both** endings rather than on
    rollback alone - so a commit that used to pay nothing now pays one
    `BumpVersion`: a cache clear plus the `on_invalidate_` hook, which
    flushes the catalog pages and broadcasts `kCatalogInvalidate`.

    The three arms are separate because only one of them can carry the cost:
    `txn-begin` and `txn-create` are the control, `txn-commit` is the arm.
    The rollback twin is `txn-rollback`, which paid the invalidation on both
    binaries and must therefore not move.
    """
    for i in range(ops):
        for side in ordered(sides, i):
            for arm, stmt in (
                    ("txn-begin", "BEGIN"),
                    ("txn-create",
                     f"CREATE TABLE cra_t_{side.label}_{tag}_{i} "
                     f"({COLUMNS}) {CLUSTERED}"),
                    ("txn-commit", "COMMIT")):
                phase = meter.phase(side, arm)
                t0 = time.perf_counter()
                reply = side(stmt)
                phase.record(time.perf_counter() - t0, reply)
    for i in range(ops):
        for side in ordered(sides, i):
            side.must("BEGIN")
            side.must(f"CREATE TABLE cra_r_{side.label}_{tag}_{i} "
                      f"({COLUMNS}) {CLUSTERED}")
            phase = meter.phase(side, "txn-rollback")
            t0 = time.perf_counter()
            reply = side("ROLLBACK")
            phase.record(time.perf_counter() - t0, reply)


def txn_ddl_cpu_pass(sides, meter, ops, rounds, tag_base):
    """Server CPU over the whole BEGIN/CREATE/COMMIT triple.

    One window rather than three: the commit alone is far below a scheduler
    tick, and the triple is what a DDL-heavy setup phase actually costs.
    """
    for r in range(rounds):
        for side in ordered(sides, r):
            tag = f"{tag_base}t{r}"
            before = side.cpu_seconds()
            for i in range(ops):
                side("BEGIN")
                side(f"CREATE TABLE cra_tc_{side.label}_{tag}_{i} "
                     f"({COLUMNS}) {CLUSTERED}")
                side("COMMIT")
            after = side.cpu_seconds()
            if before is not None and after is not None:
                meter.add_cpu(side, "txn-ddl-triple", after - before, ops)


def mark_pass(sides, meter, marks, tag):
    """`marks` transactional DROP TABLEs per side, timed on the DROP and on
    the COMMIT that follows it.

    Each one delete-marks the relation's sys.tables row and its five
    sys.columns rows, and nothing purges them - which is how the cold arms
    below come to walk ~6 x marks delete-marked rows per unfiltered scan.

    Timed on the DROP alone: BEGIN and COMMIT are their own statements and
    are not what this arm is about.
    """
    for i in range(marks):
        for side in ordered(sides, i):
            name = f"cra_m_{side.label}_{tag}_{i}"
            side.must(f"CREATE TABLE {name} ({COLUMNS}) {CLUSTERED}")
    for i in range(marks):
        for side in ordered(sides, i):
            name = f"cra_m_{side.label}_{tag}_{i}"
            side.must("BEGIN")
            phase = meter.phase(side, "drop-txn")
            t0 = time.perf_counter()
            reply = side(f"DROP TABLE {name}")
            phase.record(time.perf_counter() - t0, reply)
            commit = meter.phase(side, "drop-commit")
            t0 = time.perf_counter()
            reply = side("COMMIT")
            commit.record(time.perf_counter() - t0, reply)
            side.marks += 6


def cold_pass(sides, meter, ops, rows, rng, suffix, timed):
    """Every timed statement preceded by an untimed catalog invalidation.

    The two arms are the two shapes the task asks about that actually reach
    an unfiltered `ScanAll` per statement: a pk point SELECT and an INSERT
    into the indexed relation, both resolving their relation from cold.
    """
    keys = [rng.randrange(1, rows + 1) for _ in range(ops)]
    rows_data = make_rows(rng, ops, max(1, rows // 6))
    arms = (("cold-pk-select" + suffix,
             [f"SELECT amount FROM %s WHERE id = {k}" for k in keys]),
            ("cold-ins-idx" + suffix,
             [f"INSERT INTO %s VALUES ({values(r)})" for r in rows_data]))
    for arm, templates in arms:
        for i, template in enumerate(templates):
            for side in ordered(sides, i):
                side.invalidate()
                target = side.wins if "ins" in arm else side.orders
                stmt = template % target
                if timed:
                    phase = meter.phase(side, arm)
                    t0 = time.perf_counter()
                    reply = side(stmt)
                    phase.record(time.perf_counter() - t0, reply)
                else:
                    side(stmt)


def cold_cpu_pass(sides, meter, ops, rounds, rows, rng, suffix):
    """Server CPU for the cold arms, one contiguous window per (arm, side).

    The invalidating ALTER is *inside* the window, because there is no way to
    take it out - the /proc counters cannot be started and stopped between
    two statements without a tick of error each time. The window therefore
    prices `invalidate + statement`, and the arm's delta between the sides is
    still exactly the DT9 difference: the ALTER is identical on both.
    """
    for r in range(rounds):
        keys = [rng.randrange(1, rows + 1) for _ in range(ops)]
        rows_data = make_rows(rng, ops, max(1, rows // 6))
        arms = (("cold-pk-select" + suffix,
                 [f"SELECT amount FROM %s WHERE id = {k}" for k in keys]),
                ("cold-ins-idx" + suffix,
                 [f"INSERT INTO %s VALUES ({values(x)})" for x in rows_data]))
        for arm, templates in arms:
            target_is_write = "ins" in arm
            for side in ordered(sides, r):
                before = side.cpu_seconds()
                target = side.wins if target_is_write else side.orders
                for template in templates:
                    side.invalidate()
                    side(template % target)
                after = side.cpu_seconds()
                if before is not None and after is not None:
                    meter.add_cpu(side, arm, after - before, len(templates))


# ---- verification ---------------------------------------------------------

def count(side, relation):
    reply = side.must(f"SELECT COUNT(*) FROM {relation}")
    lines = [ln for ln in reply.splitlines() if ln.strip()]
    return int(lines[-1].split(",")[0].strip())


def verify(sides, rng, rows, customers):
    """Both engines answered the same thing, and no arm lost a write.

    Compares row counts and two replies field for field across the sides. A
    throughput number over a workload whose two sides disagree is a
    measurement of nothing.
    """
    problems = []
    counts = {}
    for side in sides:
        counts[side.label] = (count(side, side.orders), count(side, side.wins),
                              count(side, side.plain))
    distinct = set(counts.values())
    if len(distinct) != 1:
        problems.append(f"row counts differ across sides: {counts}")

    for k in [rng.randrange(1, rows + 1) for _ in range(8)]:
        replies = {s.label: s(f"SELECT amount FROM {s.orders} WHERE id = {k}")
                   for s in sides}
        if len(set(replies.values())) != 1:
            problems.append(f"pk-select id={k} differs across sides: {replies}")
    for c in [rng.randrange(customers) for _ in range(8)]:
        replies = {}
        for s in sides:
            reply = s(f"SELECT id, status FROM {s.orders} WHERE cust_id = {c} "
                      f"ORDER BY id ASC")
            replies[s.label] = reply
        if len(set(replies.values())) != 1:
            problems.append(f"idx-probe cust_id={c} differs across sides")
    for side in sides:
        if side.errors:
            problems.append(f"{side.label}: {side.errors} error replies, "
                            f"first {side.first_error}")
    return problems, counts


# ---- reporting ------------------------------------------------------------

def table(meter, sides, arms):
    labels = [s.label for s in sides]
    width = max(len(a) for a in arms) + 2
    header = (f"{'arm':<{width}}{'side':<10}{'ops':>7}{'mean':>9}{'p0':>8}"
              f"{'p25':>8}{'p50':>8}{'p95':>9}{'p99':>9}{'max':>10}{'err':>5}")
    print(header)
    print("-" * len(header))
    for arm in arms:
        for label in labels:
            phase = meter.phases.get((label, arm))
            if phase is None or phase.ops == 0:
                continue
            s = phase.summary()
            print(f"{arm:<{width}}{label:<10}{s['ops']:>7}{s['mean_us']:>9.1f}"
                  f"{s['p0_us']:>8.1f}{s['p25_us']:>8.1f}{s['p50_us']:>8.1f}"
                  f"{s['p95_us']:>9.1f}{s['p99_us']:>9.1f}{s['max_us']:>10.1f}"
                  f"{s['errors']:>5}")
        if len(labels) == 2:
            a = meter.phases.get((labels[0], arm))
            b = meter.phases.get((labels[1], arm))
            if a and b and a.ops and b.ops:
                sa, sb = a.summary(), b.summary()
                print(f"{'':<{width}}{'delta':<10}{'':>7}"
                      f"{sb['mean_us'] - sa['mean_us']:>+9.1f}"
                      f"{sb['p0_us'] - sa['p0_us']:>+8.1f}"
                      f"{sb['p25_us'] - sa['p25_us']:>+8.1f}"
                      f"{sb['p50_us'] - sa['p50_us']:>+8.1f}"
                      f"{sb['p95_us'] - sa['p95_us']:>+9.1f}"
                      f"{sb['p99_us'] - sa['p99_us']:>+9.1f}"
                      f"{'':>10}{'':>5}")
        print()


def cpu_table(meter, sides):
    if not meter.cpu:
        print("  no server CPU: pass --server-pid / --ab-server-pid")
        return
    arms = []
    for (_, arm) in meter.cpu:
        if arm not in arms:
            arms.append(arm)
    width = max(len(a) for (_, a) in meter.cpu) + 2
    header = f"{'arm':<{width}}{'side':<8}{'ops':>9}{'cpu us/op':>12}"
    print(header)
    print("-" * len(header))
    for arm in arms:
        per = {}
        for side in sides:
            entry = meter.cpu.get((side.label, arm))
            if entry is None or entry[1] == 0:
                continue
            per[side.label] = entry[0] / entry[1] * 1e6
            print(f"{arm:<{width}}{side.label:<8}{entry[1]:>9}"
                  f"{per[side.label]:>12.2f}")
        if len(per) == 2:
            a, b = [per[s.label] for s in sides]
            print(f"{'':<{width}}{'delta':<8}{'':>9}{b - a:>+12.2f}")
        print()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=15432,
                   help="side B - the server under test")
    p.add_argument("--label", default="B")
    p.add_argument("--server-pid", type=int, default=None)
    p.add_argument("--ab-port", type=int, default=None,
                   help="side A - the baseline server, on a second binary")
    p.add_argument("--ab-label", default="A")
    p.add_argument("--ab-server-pid", type=int, default=None)
    p.add_argument("--rows", type=int, default=1000,
                   help="row-set size; sweep 200 / 1000 / 10000")
    p.add_argument("--matches", type=int, default=6,
                   help="rows per indexed value, so the probe's answer stays "
                        "the same size at every --rows")
    p.add_argument("--ops", type=int, default=2500, help="ops per read arm")
    p.add_argument("--write-ops", type=int, default=400,
                   help="ops per INSERT arm; the insert relations grow by "
                        "this much plus --cpu-write-ops x --cpu-rounds")
    p.add_argument("--cpu-write-ops", type=int, default=400)
    p.add_argument("--ddl-ops", type=int, default=40, help="ops per DDL arm")
    p.add_argument("--block", type=int, default=150)
    p.add_argument("--cpu-rounds", type=int, default=4)
    p.add_argument("--cpu-ops", type=int, default=2500)
    p.add_argument("--cpu-ddl-ops", type=int, default=20)
    p.add_argument("--marks", type=int, default=100,
                   help="transactional DROP TABLEs per side; each leaves 6 "
                        "delete-marked catalog rows that nothing purges")
    p.add_argument("--cold-ops", type=int, default=120,
                   help="ops per cold-catalog arm")
    p.add_argument("--cold-cpu-ops", type=int, default=100)
    p.add_argument("--live-txns", type=int, default=32,
                   help="idle open transactions parked on their own "
                        "connections for the -live arms; 0 skips them")
    p.add_argument("--batch", type=int, default=200)
    p.add_argument("--suffix", default=None)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--json", metavar="PATH")
    p.add_argument("--echo", action="store_true")
    p.add_argument("--no-verify", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="run even on a busy box")
    args = p.parse_args()

    global ECHO
    ECHO = args.echo

    load1, load5 = [float(x) for x in open("/proc/loadavg").read().split()[:2]]
    if (load1 > 1.5 or load5 > 2.0) and not args.force:
        abort(f"host is not quiet: load average {load1} / {load5}. A "
              f"concurrent build has cut this project's throughput 3x with "
              f"nothing in a driver's output to show for it. --force to "
              f"override")
    if os.popen("pgrep -c cc1plus 2>/dev/null").read().strip() not in ("", "0") \
            and not args.force:
        abort("a C++ compile is running on this box")

    suffix = args.suffix or f"{int(time.time())}"
    rng = random.Random(args.seed)
    customers = max(1, args.rows // args.matches)

    sides = [Side(args.label, args.host, args.port, args.server_pid,
                  suffix, args.timeout)]
    if args.ab_port:
        sides.insert(0, Side(args.ab_label, args.host, args.ab_port,
                             args.ab_server_pid, suffix, args.timeout))

    versions = {s.label: s.must("SHOW META").replace("\n", " | ") for s in sides}

    rows_data = make_rows(random.Random(args.seed), args.rows, customers)
    for side in sides:
        build(side, rows_data, args.batch)
    plans = {s.label: check_plan(s) for s in sides}

    meter = Meter()
    started = time.perf_counter()

    arms_read = read_arms(sides, args.ops, args.rows, customers,
                          random.Random(args.seed + 1))
    latency_pass(sides, meter, arms_read, args.block)

    arms_write = write_arms(sides, args.write_ops, customers,
                            random.Random(args.seed + 2))
    latency_pass(sides, meter, arms_write, args.block)

    ddl_pass(sides, meter, args.ddl_ops, "lat", timed=True)
    txn_ddl_pass(sides, meter, args.ddl_ops, "lat")

    # From here the catalog carries delete-marked rows, which is the state
    # DT9's branch exists for. Everything above ran without any.
    mark_pass(sides, meter, args.marks, "lat")
    marks = {s.label: s.marks for s in sides}

    cold_pass(sides, meter, args.cold_ops, args.rows,
              random.Random(args.seed + 5), "", timed=True)
    if args.live_txns:
        for side in sides:
            side.park_idle(args.live_txns)
        cold_pass(sides, meter, args.cold_ops, args.rows,
                  random.Random(args.seed + 6), "-live", timed=True)
        for side in sides:
            side.release_idle()
    latency_elapsed = time.perf_counter() - started

    cpu_started = time.perf_counter()
    cpu_read = read_arms(sides, args.cpu_ops, args.rows, customers,
                         random.Random(args.seed + 3))
    cpu_write = write_arms(sides, args.cpu_write_ops, customers,
                           random.Random(args.seed + 4))
    cpu_pass(sides, meter, cpu_read + cpu_write, args.cpu_rounds)
    ddl_cpu_pass(sides, meter, args.cpu_ddl_ops, args.cpu_rounds, "cpu")
    txn_ddl_cpu_pass(sides, meter, args.cpu_ddl_ops, args.cpu_rounds, "cpu")
    cold_cpu_pass(sides, meter, args.cold_cpu_ops, args.cpu_rounds, args.rows,
                  random.Random(args.seed + 7), "")
    if args.live_txns:
        for side in sides:
            side.park_idle(args.live_txns)
        cold_cpu_pass(sides, meter, args.cold_cpu_ops, args.cpu_rounds,
                      args.rows, random.Random(args.seed + 8), "-live")
        for side in sides:
            side.release_idle()
    cpu_elapsed = time.perf_counter() - cpu_started

    arms = ["ping", "pk-select", "pk-select-again", "idx-probe", "show-tables",
            "ins-idx", "ins-plain", "ddl-create", "ddl-cidx", "ddl-didx",
            "txn-begin", "txn-create", "txn-commit", "txn-rollback",
            "drop-txn", "drop-commit", "cold-pk-select", "cold-ins-idx",
            "cold-pk-select-live", "cold-ins-idx-live"]

    print()
    print(f"catalog_read_ab - {args.rows} rows, {customers} customers, "
          f"{args.matches} rows per indexed value, suffix {suffix}")
    for side in sides:
        print(f"  {side.label}: port {side.port}, pid {side.pid}, "
              f"{side.marks} delete-marked catalog rows left standing")
    print(f"  {args.live_txns} idle transactions parked for the -live arms")
    print(f"  load average at start {load1} / {load5}; latency pass "
          f"{latency_elapsed:.1f} s, cpu pass {cpu_elapsed:.1f} s")
    print()
    table(meter, sides, arms)
    print("server CPU per operation (contiguous windows, "
          f"{args.cpu_rounds} rounds)")
    print()
    cpu_table(meter, sides)

    problems, counts = ([], {})
    if not args.no_verify:
        problems, counts = verify(sides, random.Random(args.seed + 9),
                                  args.rows, customers)
        if problems:
            print("VERIFY FAILED")
            for line in problems:
                print(f"  {line}")
        else:
            print(f"verify: ok - {counts}")

    if args.json:
        payload = {
            "meta": {
                "driver": "catalog_read_ab_benchmark.py",
                "rows": args.rows,
                "customers": customers,
                "matches": args.matches,
                "ops": args.ops,
                "write_ops": args.write_ops,
                "cpu_write_ops": args.cpu_write_ops,
                "ddl_ops": args.ddl_ops,
                "block": args.block,
                "cpu_rounds": args.cpu_rounds,
                "cpu_ops": args.cpu_ops,
                "cpu_ddl_ops": args.cpu_ddl_ops,
                "marks": args.marks,
                "delete_marked_rows": marks,
                "cold_ops": args.cold_ops,
                "cold_cpu_ops": args.cold_cpu_ops,
                "live_txns": args.live_txns,
                "suffix": suffix,
                "seed": args.seed,
                "sides": [{"label": s.label, "port": s.port, "pid": s.pid}
                          for s in sides],
                "show_meta": versions,
                "plans": plans,
                "loadavg_start": [load1, load5],
                "counts": counts,
                "verify_problems": problems,
            },
            "phases": [meter.phases[k].summary() for k in meter.phases],
            "cpu": [{"side": k[0], "arm": k[1], "seconds": v[0], "ops": v[1],
                     "us_per_op": v[0] / v[1] * 1e6 if v[1] else None}
                    for k, v in meter.cpu.items()],
        }
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"  wrote {args.json}")

    for side in sides:
        side.close()
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
