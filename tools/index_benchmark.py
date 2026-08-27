#!/usr/bin/env python3
"""Secondary indexes, priced: the read win, the covering claim, the write cost.

`docs/workplan-index.md` IX14 asks four questions and this driver answers all
four in one process, because they are only comparable if they run against one
server on one data file within a few seconds of each other:

  1. a selective non-pk equality, indexed against the walk that replaces it;
  2. the same statement with and without `COVERING`, to price
     `docs/spec/index.md` §7's claim that covering buys the **avoided
     descents** and nothing else - it is explicitly not an index-only scan;
  3. INSERT with 0, 1 and 2 indexes, and an UPDATE that moves an indexed key
     (which appends) against one that touches only a non-indexed column
     (which must append nothing);
  4. the same shapes on PostgreSQL - `pg_index_benchmark.py`, which imports
     the schema, the row generator and the shape list from this file so the
     two cannot drift into measuring different questions.

---- What makes the comparison fair ---------------------------------------

**Three read relations with byte-identical contents.** `ord_none` carries no
index, `ord_idx` one on `cust_id`, `ord_cov` the same key with
`COVERING (status)`. The load generates the row list once and inserts it into
all three, so a probe returns the same rows in the same order from each - and
`--verify` checks exactly that, comparing every shape's reply from the two
indexed relations against the unindexed one **including row order**, which is
what `index.md` IX8a's pk-order rule stands or falls on.

**Every shape is driven with the same argument on all three relations, in the
same operation.** The three are interleaved rather than run in sequence, so a
machine that gets busier partway through costs all three equally instead of
inventing a result. The same rule drives the write phases.

**Equal work, not equal time.** Every phase is a fixed operation count.

**The noise floor comes from inside the run.** `eq` is measured twice on
`ord_idx` - once as `eq` and once as `eq-again` - and the two are the same
configuration by construction. A delta smaller than that gap is not a finding.
The pk lookup is a second control: it descends the clustered tree and touches
no secondary structure, so it must not move when an index is added.

---- The `indexes` switch -------------------------------------------------

`indexes = off` in the server config makes an index step take the walk it
would have taken had the index not existed, **with the compiled chain
unchanged** (`index.md` §12.3). That is the sharpest A/B available:
identical plan, identical rows, different work. It is a startup key with no
runtime `SET`, so the two sides are two server processes over two freshly
loaded data files - the load is seeded, so their contents match.

The driver does not take the operator's word for which mode it is in: it
reads the `index_scanned=` counter out of `ANALYZE` after the load and
reports the mode it actually observed. `--expect-indexes on|off` turns a
mismatch into a failed run rather than a mislabelled table.

---- Row-set sizes --------------------------------------------------------

`--rows` is the axis: **200 / 1000 / 10000**. Customers scale with it
(`rows / --matches`), so `WHERE cust_id = ?` answers with the same ~6 rows at
every size and the relation alone grows. That is the axis on which a walk
(O(rows)) and a descent (O(log rows + matches)) diverge; letting the answer
grow too would move two variables at once.

The range shapes hold their answer constant the same way: the span is a fixed
number of customers, so `BETWEEN` scans ~60 entries at 200 rows and at 10,000.

Flags, invocations and the PostgreSQL twin: `bench/docs/README.md`.
"""

import argparse
import datetime
import random
import re
import subprocess
import sys
import time

from bench_common import Phase, report, write_json
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection, format_reply

# ---- the schema ----------------------------------------------------------
#
# One relation shape, used six times. Every relation is BTREE and that is a
# hard requirement: index.md §3 permits a secondary index only on a
# btree-clustered relation, because an entry's payload is a pk and resolving
# one costs a descent there and a chain scan on a heap.
#
# `id` never appears in an INSERT column list - it is the Keystone word,
# system-generated (invariant 11).
#
#   cust_id   the indexed equality/range column on the read relations
#   region    the indexed column on the write relations - what a key-moving
#             UPDATE moves
#   status    the COVERING column, and the residual the entry-side filter
#             gets to decide
#   amount    indexed by nothing anywhere - what a non-key UPDATE touches
#   ref       one varchar, so a row is the width a real OLTP row is and a
#             page holds a realistic number of them
COLUMNS = ("id int64, cust_id int64, region int32, status int32, "
           "amount int64, ref varchar")
CLUSTERED = "BTREE"

# The three read relations: same contents, different index.
READ_RELS = {
    "none": (),
    "idx": (("cust_id",), ()),
    "cov": (("cust_id",), ("status",)),
}
READ_ORDER = ("none", "idx", "cov")

# The three write relations: same contents, 0 / 1 / 2 indexes. `w2`'s second
# index is on `cust_id`, which no UPDATE below touches - so a key-moving
# UPDATE on `w2` appends to one of its two indexes, which is what prices
# index.md §2's "an UPDATE that touches no key column must not append".
WRITE_RELS = {
    "w0": (),
    "w1": ((("region",), ()),),
    "w2": ((("region",), ()), (("cust_id",), ())),
}
WRITE_ORDER = ("w0", "w1", "w2")

REGIONS = 8
STATUSES = 6
# Customers spanned by a BETWEEN. Fixed rather than proportional, so the
# range's answer stays ~RANGE_SPAN x --matches rows at every row-set size.
RANGE_SPAN = 10

INSERTED_ID = re.compile(r"\bid=(\d+)")
ROW_COUNT = re.compile(r"\((\d+) rows?\)")
INDEX_SCANNED = re.compile(r"index_scanned=(\d+)")
INDEX_FILTERED = re.compile(r"index_filtered=(\d+)")
INDEX_RESOLVED = re.compile(r"index_resolved=(\d+)")
EXAMINED = re.compile(r"examined=(\d+)")


def abort(message, reply=None):
    print(f"index_benchmark aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


ECHO = False


class Client:
    """One connection and the one-command-one-reply callable everything is
    written against. It counts errors itself, so a caller that does not
    inspect every reply still cannot report a clean run over a failing one."""

    def __init__(self, host, port, timeout):
        try:
            self._conn = ServerConnection(host, port, timeout=timeout)
        except OSError as e:
            abort(f"could not connect to {host}:{port}: {e}\n"
                  f"  start one with: ./build-release/kds_server "
                  f"~/bench-index/idx.db --port {port}")
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            print(f"[ix] {command}  ->  {reply[:110]}", file=sys.stderr,
                  flush=True)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def timed(self, command, phase):
        t0 = time.perf_counter()
        reply = self(command)
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def close(self):
        self._conn.close()


def select_rows(reply):
    """The data rows of a SELECT reply, as lists of field strings.

    The newline protocol answers a SELECT with a comma-separated header line
    and then one line per row, and answers an empty result with the header
    alone - so the first line is dropped unconditionally and a one-line reply
    has no rows. An error and a genuinely empty result look alike here on
    purpose: the caller checks `ERR` through the Phase, never by inspecting
    this list."""
    if reply.startswith("ERR"):
        return []
    lines = [ln for ln in reply.splitlines() if ln.strip()]
    if len(lines) <= 1:
        return []
    return [[f.strip() for f in ln.split(",")] for ln in lines[1:]]


def result_key(reply):
    """A reply reduced to what two engines' answers must agree on: the data
    rows, **in order**. Order is not incidental here - an index step collects
    pks in index-key order and a scan emits them in pk order, so comparing
    unordered sets would be blind to exactly the bug IX8a exists to prevent."""
    return [tuple(r) for r in select_rows(reply)]


# ---- the data ------------------------------------------------------------

def sizes_for(args):
    """Row count and customer count, with matches-per-key held constant.

    Holding it constant is what makes the three sizes comparable: a bigger
    relation must not also mean a less selective predicate, or a shape's cost
    grows for two reasons at once."""
    rows = max(args.rows, 20)
    customers = max(rows // max(args.matches, 1), 4)
    return {"rows": rows, "customers": customers}


def make_rows(sizes, seed):
    """The row list, generated once and inserted into every relation.

    Returned as a list rather than streamed, because the three read relations
    must hold identical contents for `--verify`'s equivalence check to mean
    anything, and re-deriving them from a re-seeded generator per relation is
    a second place for them to disagree."""
    rng = random.Random(seed)
    out = []
    for i in range(sizes["rows"]):
        out.append((
            rng.randrange(sizes["customers"]),     # cust_id
            rng.randrange(REGIONS),                # region
            rng.randrange(STATUSES),               # status
            rng.randrange(1_000_000),              # amount
            f"REF{i:08d}",                         # ref
        ))
    return out


def insert_values(row):
    """The VALUES list for one row, in both engines' shared column order."""
    cust, region, status, amount, ref = row
    return f"{cust}, {region}, {status}, {amount}, '{ref}'"


# ---- DDL -----------------------------------------------------------------

def table_names(suffix):
    names = {}
    for tag in READ_ORDER:
        names[tag] = f"ord_{tag}_{suffix}"
    for tag in WRITE_ORDER:
        names[tag] = f"ord_{tag}_{suffix}"
    return names


def create_tables(client, names, phase):
    for tag in READ_ORDER + WRITE_ORDER:
        reply = client.timed(
            f"CREATE TABLE {names[tag]} ({COLUMNS}) {CLUSTERED}", phase)
        if reply.startswith("ERR"):
            if "no room" in reply or "catalog" in reply.lower():
                abort(f"could not create {names[tag]}: the catalog is out of "
                      f"column space. Nothing reclaims a catalog row and "
                      f"there is no DROP TABLE - start on a fresh data file.",
                      reply)
            abort(f"could not create {names[tag]}", reply)


def index_defs(tag, names, suffix):
    """`(name, statement)` per index on relation `tag`.

    An index name is unique **instance-wide** (index.md §10), so the run
    suffix is in the name as well as in the relation's."""
    spec = READ_RELS.get(tag)
    if spec is None:
        defs = WRITE_RELS[tag]
    elif spec:
        defs = (spec,)
    else:
        defs = ()
    out = []
    for keys, covering in defs:
        name = f"ix_{tag}_{'_'.join(keys)}_{suffix}"
        stmt = f"CREATE INDEX {name} ON {names[tag]} ({', '.join(keys)})"
        if covering:
            stmt += f" COVERING ({', '.join(covering)})"
        out.append((name, stmt))
    return out


def create_indexes(client, tags, names, suffix, phase):
    """Declares the indexes. A failure aborts rather than degrading to an
    unindexed run: a benchmark whose index silently did not exist reports the
    baseline twice and calls the second one a result."""
    made = []
    for tag in tags:
        for name, stmt in index_defs(tag, names, suffix):
            reply = client.timed(stmt, phase)
            if reply.startswith("ERR"):
                lowered = reply.lower()
                if "unsupported" in lowered or "expected" in lowered:
                    abort(f"this server does not understand CREATE INDEX.\n"
                          f"  `{stmt}`\n  Secondary indexes need a build with "
                          f"docs/spec/index.md in it.", reply)
                if "heap" in lowered:
                    abort(f"CREATE INDEX refused a heap relation - SCHEMA was "
                          f"edited; index.md §3 requires BTREE.", reply)
                abort(f"could not create index {name}", reply)
            made.append(name)
    return made


# ---- load ----------------------------------------------------------------

def load(client, names, tags, rows, phase, batch):
    """Inserts `rows` into every relation in `tags`, interleaved.

    Interleaved, because these relations are compared against each other:
    loading one fully and then the next would give the second a data file
    that already holds the first, and there is no page reclamation to undo
    that. Batched in transactions because the load is setup, not a
    measurement - the measured insert phase is autocommit.
    """
    pending = 0
    if batch > 1:
        client("BEGIN")
    for row in rows:
        values = insert_values(row)
        for tag in tags:
            client.timed(f"INSERT INTO {names[tag]} VALUES ({values})", phase)
        pending += 1
        if batch > 1 and pending >= batch:
            client("COMMIT")
            client("BEGIN")
            pending = 0
    if batch > 1:
        client("COMMIT")


# ---- the read shapes -----------------------------------------------------
#
# Each entry is (name, detail, builder). The builder takes a relation name
# and one already-drawn argument tuple and returns a statement, so every
# relation in a round sees the *same* argument - which is what makes the
# three columns of the result table a comparison rather than three runs.

def shape_args(sizes, rng):
    """One argument draw, shared by every relation in the round."""
    customers = sizes["customers"]
    return {
        "cust": rng.randrange(customers),
        "status": rng.randrange(STATUSES),
        "lo": rng.randrange(max(customers - RANGE_SPAN, 1)),
        "pk": rng.randrange(1, sizes["rows"] + 1),
    }


SHAPES = (
    ("pk", "WHERE id = ? - the control: a clustered-tree Lookup, "
           "which no secondary index may move",
     lambda t, a: f"SELECT cust_id, status, amount FROM {t} "
                  f"WHERE id = {a['pk']}"),
    ("eq", "WHERE cust_id = ? - the headline selective equality",
     lambda t, a: f"SELECT id, region, status, amount FROM {t} "
                  f"WHERE cust_id = {a['cust']}"),
    ("eq-again", "WHERE cust_id = ? again - the in-run noise floor",
     lambda t, a: f"SELECT id, region, status, amount FROM {t} "
                  f"WHERE cust_id = {a['cust']}"),
    ("eq-covered", "WHERE cust_id = ? AND status = ? - a residual the "
                   "covered column can decide",
     lambda t, a: f"SELECT id, region, amount FROM {t} "
                  f"WHERE cust_id = {a['cust']} AND status = {a['status']}"),
    ("range", "WHERE cust_id BETWEEN ? AND ? - a non-pk range, nothing for "
              "a covered column to filter",
     lambda t, a: f"SELECT id, region, status FROM {t} WHERE cust_id "
                  f"BETWEEN {a['lo']} AND {a['lo'] + RANGE_SPAN}"),
    ("range-covered", "the same range AND status = ? - where COVERING has "
                      "the most to drop",
     lambda t, a: f"SELECT id, region, amount FROM {t} WHERE cust_id "
                  f"BETWEEN {a['lo']} AND {a['lo'] + RANGE_SPAN} "
                  f"AND status = {a['status']}"),
    ("count-eq", "SELECT COUNT(*) ... WHERE cust_id = ? - not servable from "
                 "the index either (index.md §7)",
     lambda t, a: f"SELECT COUNT(*) FROM {t} WHERE cust_id = {a['cust']}"),
)


def ping_phase(client, ops):
    """`PING`, `--ops` times: the client and socket round trip with no engine
    work behind it.

    Every latency in this file is a sum, and this is the term that is not the
    engine - one Python send, one receive, and a dispatcher that answers
    without opening a relation. Subtracting it from a shape's p0 is what says
    how much of a 130 us pk lookup the engine is actually responsible for. It
    is also a control that cannot be affected by any index, so a run where it
    moves between configurations is a run where the machine moved."""
    phase = Phase("ping", "client + socket round trip, no engine work")
    for _ in range(ops):
        client.timed("PING", phase)
    return phase


def read_phases(client, names, sizes, args, rng):
    """Every shape against every read relation, interleaved per operation."""
    phases = {}
    for shape, detail, _ in SHAPES:
        for tag in READ_ORDER:
            phases[(shape, tag)] = Phase(f"{shape}[{tag}]", detail)

    for _ in range(args.ops):
        drawn = shape_args(sizes, rng)
        for shape, _, build in SHAPES:
            # The relation order rotates so no relation is permanently the
            # one that pays for a cold cache line at the top of a round.
            for tag in rotate(READ_ORDER, rng.randrange(len(READ_ORDER))):
                client.timed(build(names[tag], drawn), phases[(shape, tag)])
    return phases


def rotate(seq, n):
    return seq[n:] + seq[:n]


# ---- the write shapes ----------------------------------------------------

def write_phases(client, names, sizes, args, rng):
    """INSERT with 0/1/2 indexes, then two UPDATE shapes on each.

    The insert is autocommit, one durability point per row, because that is
    what a client pays and what the durability wait in the write-up is a
    share of. The three relations are interleaved per row.
    """
    phases = {}
    for tag in WRITE_ORDER:
        n = len(WRITE_RELS[tag])
        phases[("insert", tag)] = Phase(
            f"insert[{tag}]", f"INSERT with {n} index{'' if n == 1 else 'es'}")
    rows = make_rows(sizes, args.seed + 7)[:args.write_ops]
    for row in rows:
        values = insert_values(row)
        for tag in rotate(WRITE_ORDER, rng.randrange(len(WRITE_ORDER))):
            client.timed(f"INSERT INTO {names[tag]} VALUES ({values})",
                         phases[("insert", tag)])

    # The UPDATE half. Both shapes address the row by pk, so their read half
    # is one clustered descent and the only thing that differs between them
    # is what the write hook does with the index.
    for tag in WRITE_ORDER:
        n = len(WRITE_RELS[tag])
        phases[("upd-key", tag)] = Phase(
            f"upd-key[{tag}]",
            f"SET region = ? - moves an indexed key on "
            f"{'1 of ' + str(n) if n else 'none of 0'} index(es)")
        phases[("upd-nonkey", tag)] = Phase(
            f"upd-nonkey[{tag}]",
            "SET amount = ? - touches no index column: must append nothing")

    count = min(args.update_ops, len(rows)) if rows else 0
    for i in range(count):
        pk = rng.randrange(1, len(rows) + 1)
        region = rng.randrange(REGIONS)
        amount = rng.randrange(1_000_000)
        for tag in rotate(WRITE_ORDER, rng.randrange(len(WRITE_ORDER))):
            client.timed(f"UPDATE {names[tag]} SET region = {region} "
                         f"WHERE id = {pk}", phases[("upd-key", tag)])
        for tag in rotate(WRITE_ORDER, rng.randrange(len(WRITE_ORDER))):
            client.timed(f"UPDATE {names[tag]} SET amount = {amount} "
                         f"WHERE id = {pk}", phases[("upd-nonkey", tag)])
    return phases


# ---- ANALYZE -------------------------------------------------------------

def analyze_shapes(client, names, sizes, seed):
    """One `ANALYZE` per (shape, read relation), for the counters no latency
    can carry: examined, matched, and the index's three numbers.

    `index_filtered` is the only honest price for a COVERING clause
    (index.md §7): it counts base descents the covered columns avoided,
    and nothing else. Zero means the clause bought exactly the write cost it
    added."""
    rng = random.Random(seed)
    drawn = shape_args(sizes, rng)
    out = {}
    for shape, _, build in SHAPES:
        for tag in READ_ORDER:
            reply = client(f"ANALYZE {build(names[tag], drawn)}")
            line = ""
            for text in reply.splitlines():
                if text.strip().startswith("step 0") and "opens=" in text:
                    line = text.strip()
            out[f"{shape}[{tag}]"] = line
    return out


def observed_index_mode(analysis):
    """Which side of the `indexes` switch the server is actually on.

    Read out of the counters rather than taken on trust: the compiled chain
    is identical either way by design (index.md §12.3), so the plan
    cannot answer this and only the work done can. An index step that
    scanned no index entries walked instead."""
    for key, line in analysis.items():
        if key.endswith("[idx]") and ("Index" in line):
            if INDEX_SCANNED.search(line):
                return "on"
            return "off"
    return "unknown"


# ---- verification --------------------------------------------------------

def verify(client, names, sizes, args, rng):
    """The checks that decide whether any number above is worth reporting.

    1. Every relation holds the rows it was given.
    2. **Every read shape's reply from `ord_idx` and `ord_cov` equals
       `ord_none`'s, row for row and in order.** The three hold identical
       contents, so this is the equivalence that says the index served a
       complete set in the right order - the failure cabin.md §5 calls
       invisible without a baseline, which index.md §1 inherits, plus
       IX8a's pk-order rule which only an ordered comparison can see.
    3. The same across the write relations after their updates, which is the
       write hook's half: if a key-moving UPDATE failed to append, `w1` and
       `w2` return fewer rows for the new region than `w0`'s walk.
    """
    problems = []

    for tag in READ_ORDER:
        reply = client(f"SELECT COUNT(*) FROM {names[tag]}")
        rows = select_rows(reply)
        got = int(rows[0][0]) if rows and rows[0][0].lstrip("-").isdigit() else -1
        if got != sizes["rows"]:
            problems.append(f"{tag}: COUNT(*) = {got}, loaded {sizes['rows']}")

    for _ in range(args.verify):
        drawn = shape_args(sizes, rng)
        for shape, _, build in SHAPES:
            base = result_key(client(build(names["none"], drawn)))
            for tag in ("idx", "cov"):
                got = result_key(client(build(names[tag], drawn)))
                if got != base:
                    problems.append(
                        f"{shape}: {tag} returned {len(got)} rows, the "
                        f"unindexed walk returned {len(base)}"
                        + ("" if len(got) != len(base) else
                           " (same count, different rows or order)"))

    # The write relations, after the update phase. Same contents by
    # construction, so an index that lost an append shows up here.
    for _ in range(args.verify):
        region = rng.randrange(REGIONS)
        base = result_key(client(f"SELECT id, region FROM {names['w0']} "
                                 f"WHERE region = {region}"))
        for tag in ("w1", "w2"):
            got = result_key(client(f"SELECT id, region FROM {names[tag]} "
                                    f"WHERE region = {region}"))
            if got != base:
                problems.append(
                    f"write hook: {tag} WHERE region = {region} returned "
                    f"{len(got)} rows, the unindexed walk returned "
                    f"{len(base)}")
    return problems


# ---- reporting -----------------------------------------------------------

def git_stamp():
    def run(cmd):
        try:
            return subprocess.run(cmd, cwd=sys.path[0] or ".", shell=True,
                                  capture_output=True, text=True,
                                  timeout=10).stdout.strip()
        except Exception:
            return ""
    return {
        "branch": run("git rev-parse --abbrev-ref HEAD"),
        "commit": run("git rev-parse --short HEAD"),
        "dirty": bool(run("git status --porcelain")),
        "committed_at": run("git log -1 --format=%cI"),
    }


def build_parser(description):
    """The flags both this driver and its PostgreSQL twin take, so the two
    are invoked identically apart from connection details."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--suffix", default=None,
                        help="relation-name suffix (default: a timestamp)")
    parser.add_argument("--rows", type=int, default=1000,
                        help="rows in each read relation, and the row-set "
                             "axis. The documented sweep is 200 / 1000 / "
                             "10000 (default: 1000)")
    parser.add_argument("--matches", type=int, default=6,
                        help="rows per cust_id; customers are scaled to hold "
                             "it constant across the sweep (default: 6)")
    parser.add_argument("--ops", type=int, default=200,
                        help="operations per read shape per relation "
                             "(default: 200)")
    parser.add_argument("--write-ops", type=int, default=400,
                        help="INSERTs per write relation (default: 400)")
    parser.add_argument("--update-ops", type=int, default=200,
                        help="UPDATEs per shape per write relation "
                             "(default: 200)")
    parser.add_argument("--batch", type=int, default=200,
                        help="rows per transaction during the load; the "
                             "measured insert phase is always autocommit "
                             "(default: 200)")
    parser.add_argument("--verify", type=int, default=20, metavar="N",
                        help="argument draws for the equivalence checks; "
                             "0 disables (default: 20)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--json", metavar="PATH")
    parser.add_argument("--echo", action="store_true")
    return parser


def main():
    parser = build_parser("Secondary indexes: the read win, the covering "
                          "claim and the write cost, in one run.")
    parser.add_argument("--expect-indexes", choices=("on", "off"),
                        help="fail the run if the server's `indexes` setting "
                             "is not this. Read from ANALYZE's counters, not "
                             "from the config - the compiled chain is "
                             "identical either way by design.")
    parser.add_argument("--no-writes", action="store_true",
                        help="skip the INSERT/UPDATE phases")
    args = parser.parse_args()

    global ECHO
    ECHO = args.echo

    suffix = args.suffix or datetime.datetime.now().strftime("%H%M%S")
    sizes = sizes_for(args)
    rng = random.Random(args.seed + 1000)
    client = Client(args.host, args.port, args.timeout)
    started = datetime.datetime.now(datetime.timezone.utc)

    names = table_names(suffix)
    ddl = Phase("ddl", "CREATE TABLE x6")
    create_tables(client, names, ddl)

    # The write relations get their indexes on an empty relation, so the
    # IX06 write hook maintains every row the measured insert phase adds.
    # The read relations get theirs after the load, which is the IX09
    # backfill and is timed separately.
    idx_before = Phase("create-index[write]", "on empty write relations")
    create_indexes(client, WRITE_ORDER, names, suffix, idx_before)

    loadp = Phase("load", "INSERT into the three read relations, batched")
    rows = make_rows(sizes, args.seed)
    load(client, names, READ_ORDER, rows, loadp, args.batch)

    idx_after = Phase("create-index[read]", "the IX09 backfill, on loaded "
                                            "relations")
    create_indexes(client, ("idx", "cov"), names, suffix, idx_after)

    analysis = analyze_shapes(client, names, sizes, args.seed + 3)
    mode = observed_index_mode(analysis)
    if args.expect_indexes and mode != args.expect_indexes:
        abort(f"--expect-indexes {args.expect_indexes}, but the server "
              f"behaved as `indexes = {mode}`.\n"
              f"  Read from ANALYZE's index_scanned counter; the compiled "
              f"chain is identical either way, so only the work says which.")

    ordered = [ddl, idx_before, loadp, idx_after,
               ping_phase(client, args.ops)]
    reads = read_phases(client, names, sizes, args, rng)
    for shape, _, _ in SHAPES:
        for tag in READ_ORDER:
            ordered.append(reads[(shape, tag)])

    if not args.no_writes:
        writes = write_phases(client, names, sizes, args, rng)
        for kind in ("insert", "upd-key", "upd-nonkey"):
            for tag in WRITE_ORDER:
                ordered.append(writes[(kind, tag)])

    problems = []
    if args.verify:
        problems = verify(client, names, sizes, args, rng)

    meta = {
        "engine": "ckdbs",
        "scenario": "index-benchmark",
        "columns": len(COLUMNS.split(",")),
        "rows": sizes["rows"],
        "host": args.host,
        "port": args.port,
        "table": ", ".join(names[t] for t in READ_ORDER),
        "clustered": "btree",
        "connections": 1,
        "sizes": sizes,
        "matches_per_key": args.matches,
        "range_span_customers": RANGE_SPAN,
        "ops_per_shape": args.ops,
        "write_ops": 0 if args.no_writes else args.write_ops,
        "update_ops": 0 if args.no_writes else args.update_ops,
        "indexes_observed": mode,
        "seed": args.seed,
        "started_utc": started.isoformat(timespec="seconds"),
        "git": git_stamp(),
        "server_meta": client("SHOW META").replace("\n", "; ")[:200],
        "analyze": analysis,
        "show_indexes": client("SHOW INDEXES"),
        "verify_problems": problems,
    }

    footer = [
        f"indexes observed {mode} (from ANALYZE's counters, not the config)",
        f"sizes: rows={sizes['rows']}, customers={sizes['customers']}, "
        f"~{args.matches} rows per cust_id, range spans {RANGE_SPAN} "
        f"customers",
        "ANALYZE, one draw per shape:",
    ]
    for key in sorted(analysis):
        footer.append(f"    {key:<22} {analysis[key]}")
    footer.append("SHOW INDEXES:")
    for line in meta["show_indexes"].splitlines():
        footer.append(f"    {line}")
    if problems:
        footer.append(f"VERIFY FAILED - {len(problems)} problem(s):")
        for line in problems[:10]:
            footer.append(f"    {line}")
    if client.errors:
        footer.append(f"{client.errors} error replies, "
                      f"first: {client.first_error}")

    report(ordered, meta, footer)
    if args.json:
        write_json(args.json, meta, ordered)
    client.close()

    if problems:
        sys.exit(2)
    if client.errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
