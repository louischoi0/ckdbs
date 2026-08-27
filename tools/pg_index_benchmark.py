#!/usr/bin/env python3
"""PostgreSQL twin of `index_benchmark.py`.

Same six relations, same row generator, same seven read shapes, same
arguments drawn from the same seeded stream, same phase names - so the two
tables line up and a comparison is a comparison rather than two runs. Every
shape definition, the row generator and the sizing arithmetic are **imported**
from `index_benchmark.py` rather than restated, which is what stops the two
sides drifting into measuring different questions.

What differs, and it is dialect rather than shape:

  * `bigserial primary key` is how PostgreSQL spells the system-generated
    identity KDS carries in its Keystone word (invariant 11).
  * `text` / `integer` / `bigint` spell KDS's `varchar` / `int32` / `int64`.
  * `CREATE INDEX ... INCLUDE (status)` is PostgreSQL's `COVERING (status)`.

What differs and is **not** dialect, and is the most interesting column of
the comparison: PostgreSQL's `INCLUDE` can produce a real **index-only
scan**, because a visibility map exists to answer "is every row on this heap
page visible to everyone?" without reading the page. `index.md` §7 says
KDS cannot have one and says exactly why - there is no visibility witness
outside the tuple. So this twin runs `EXPLAIN (ANALYZE, BUFFERS)` on the
covering shapes and records which scan node PostgreSQL chose; a run where it
chose Index Only Scan is the measurement of what that missing witness is
worth.

`VACUUM ANALYZE` runs after the load and **is not tuning**: without
statistics PostgreSQL may not choose its index at all, which would make the
baseline a coin toss, and without the vacuum the visibility map is empty and
an index-only scan cannot happen even where it is the right plan. Both are
what a PostgreSQL installation does to itself. The cluster stays at default
settings otherwise (`tools/pg_setup.sh`, port 15433).

Usage and flags: `bench/docs/README.md`.
"""

import argparse
import datetime
import random
import sys
import time

from bench_common import Phase, report, write_json
from pg_wire import DEFAULT_HOST, PgConnection, PgError
from index_benchmark import (
    READ_ORDER, READ_RELS, REGIONS, SHAPES, WRITE_ORDER, WRITE_RELS,
    git_stamp, insert_values, make_rows, rotate, shape_args, sizes_for,
)

# The same shape as index_benchmark.COLUMNS, in PostgreSQL's dialect. Kept as
# its own string rather than translated at runtime: the two engines' type
# spellings do not map one to one, and a translator would be a second place
# for the schema to live.
COLUMNS = ("id bigserial primary key, cust_id bigint, region integer, "
           "status integer, amount bigint, ref text")


def abort(message, detail=None):
    print(f"pg_index_benchmark aborted: {message}", file=sys.stderr)
    if detail:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


ECHO = False


class Client:
    def __init__(self, conn):
        self._conn = conn
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = self._conn.send_command(command)
        if ECHO:
            print(f"[pgix] {command}  ->  {reply[:110]}", file=sys.stderr,
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

    def fetch(self, sql):
        rows, error = self._conn.fetch(sql)
        if error:
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{sql}  ->  {error}"
        return rows


# ---- DDL -----------------------------------------------------------------

def table_names(suffix):
    return {tag: f"ord_{tag}_{suffix}"
            for tag in READ_ORDER + WRITE_ORDER}


def create_tables(client, names, phase, keep):
    for tag in READ_ORDER + WRITE_ORDER:
        if not keep:
            client(f"DROP TABLE IF EXISTS {names[tag]}")
        reply = client.timed(f"CREATE TABLE {names[tag]} ({COLUMNS})", phase)
        if reply.startswith("ERR"):
            abort(f"could not create {names[tag]}", reply)


def index_defs(tag, names, suffix):
    """The same index set as the ckdbs driver's, spelled with INCLUDE."""
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
            stmt += f" INCLUDE ({', '.join(covering)})"
        out.append((name, stmt))
    return out


def create_indexes(client, tags, names, suffix, phase):
    made = []
    for tag in tags:
        for name, stmt in index_defs(tag, names, suffix):
            reply = client.timed(stmt, phase)
            if reply.startswith("ERR"):
                abort(f"could not create index {name}", reply)
            made.append(name)
    return made


def load(client, names, tags, rows, phase, batch):
    pending = 0
    columns = "(cust_id, region, status, amount, ref)"
    if batch > 1:
        client("BEGIN")
    for row in rows:
        values = insert_values(row)
        for tag in tags:
            client.timed(f"INSERT INTO {names[tag]} {columns} "
                         f"VALUES ({values})", phase)
        pending += 1
        if batch > 1 and pending >= batch:
            client("COMMIT")
            client("BEGIN")
            pending = 0
    if batch > 1:
        client("COMMIT")


# ---- phases --------------------------------------------------------------

def ping_phase(client, ops):
    """The twin of the ckdbs driver's `PING` phase: the client and socket
    round trip with no engine work behind it. `SELECT 1` is PostgreSQL's
    cheapest complete statement and is what the two drivers' floors have to
    be compared through - the two clients are different code over different
    protocols, so an absolute latency comparison that does not subtract them
    is comparing two Python programs."""
    phase = Phase("ping", "client + socket round trip, no engine work")
    for _ in range(ops):
        client.timed("SELECT 1", phase)
    return phase


def read_phases(client, names, sizes, args, rng):
    phases = {}
    for shape, detail, _ in SHAPES:
        for tag in READ_ORDER:
            phases[(shape, tag)] = Phase(f"{shape}[{tag}]", detail)
    for _ in range(args.ops):
        drawn = shape_args(sizes, rng)
        for shape, _, build in SHAPES:
            for tag in rotate(READ_ORDER, rng.randrange(len(READ_ORDER))):
                client.timed(build(names[tag], drawn), phases[(shape, tag)])
    return phases


def write_phases(client, names, sizes, args, rng):
    phases = {}
    columns = "(cust_id, region, status, amount, ref)"
    for tag in WRITE_ORDER:
        n = len(WRITE_RELS[tag])
        phases[("insert", tag)] = Phase(
            f"insert[{tag}]", f"INSERT with {n} index{'' if n == 1 else 'es'}")
    rows = make_rows(sizes, args.seed + 7)[:args.write_ops]
    for row in rows:
        values = insert_values(row)
        for tag in rotate(WRITE_ORDER, rng.randrange(len(WRITE_ORDER))):
            client.timed(f"INSERT INTO {names[tag]} {columns} "
                         f"VALUES ({values})", phases[("insert", tag)])

    for tag in WRITE_ORDER:
        n = len(WRITE_RELS[tag])
        phases[("upd-key", tag)] = Phase(
            f"upd-key[{tag}]", f"SET region = ? - moves an indexed key on "
                               f"{'1 of ' + str(n) if n else 'none of 0'} "
                               f"index(es)")
        phases[("upd-nonkey", tag)] = Phase(
            f"upd-nonkey[{tag}]",
            "SET amount = ? - touches no index column")
    count = min(args.update_ops, len(rows)) if rows else 0
    for _ in range(count):
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


def explain_shapes(client, names, sizes, seed):
    """`EXPLAIN (ANALYZE)` per (shape, relation), reduced to the scan node.

    The node name is the number that matters here: `Index Only Scan` is the
    plan KDS structurally cannot produce (`index.md` §7), and seeing
    which shapes PostgreSQL serves that way is what prices the missing
    visibility witness rather than guessing at it."""
    rng = random.Random(seed)
    drawn = shape_args(sizes, rng)
    out = {}
    for shape, _, build in SHAPES:
        for tag in READ_ORDER:
            sql = f"EXPLAIN (ANALYZE, BUFFERS) {build(names[tag], drawn)}"
            rows = client.fetch(sql)
            plan = [r[0].decode() for r in rows if r and r[0] is not None]
            node = ""
            for line in plan:
                stripped = line.strip().lstrip("->").strip()
                if stripped.startswith(("Seq Scan", "Index Scan",
                                        "Index Only Scan", "Bitmap",
                                        "Aggregate")):
                    if not node or "Scan" in stripped:
                        node = stripped.split("(")[0].strip()
                    if "Scan" in stripped:
                        break
            heap = ""
            for line in plan:
                if "Heap Fetches" in line:
                    heap = line.strip()
            out[f"{shape}[{tag}]"] = (node + ("  " + heap if heap else "")
                                      ).strip()
    return out


def verify(client, names, sizes, args, rng):
    """The same equivalence check the ckdbs driver makes, so a failure means
    the same thing on both sides: an indexed relation must answer a shape
    exactly as the unindexed one does."""
    problems = []
    for tag in READ_ORDER:
        rows = client.fetch(f"SELECT COUNT(*) FROM {names[tag]}")
        got = int(rows[0][0]) if rows and rows[0][0] is not None else -1
        if got != sizes["rows"]:
            problems.append(f"{tag}: COUNT(*) = {got}, loaded {sizes['rows']}")

    for _ in range(args.verify):
        drawn = shape_args(sizes, rng)
        for shape, _, build in SHAPES:
            base = client.fetch(build(names["none"], drawn))
            for tag in ("idx", "cov"):
                got = client.fetch(build(names[tag], drawn))
                if got != base:
                    problems.append(
                        f"{shape}: {tag} returned {len(got)} rows, the "
                        f"unindexed scan returned {len(base)}")
    return problems


def main():
    parser = argparse.ArgumentParser(
        description="PostgreSQL twin of index_benchmark.py.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=15433)
    parser.add_argument("--user", default=None)
    parser.add_argument("--database", default="bench")
    parser.add_argument("--suffix", default=None)
    parser.add_argument("--rows", type=int, default=1000)
    parser.add_argument("--matches", type=int, default=6)
    parser.add_argument("--ops", type=int, default=200)
    parser.add_argument("--write-ops", type=int, default=400)
    parser.add_argument("--update-ops", type=int, default=200)
    parser.add_argument("--batch", type=int, default=200)
    parser.add_argument("--verify", type=int, default=20, metavar="N")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--json", metavar="PATH")
    parser.add_argument("--echo", action="store_true")
    parser.add_argument("--no-writes", action="store_true")
    parser.add_argument("--synchronous-commit", default=None,
                        choices=("on", "off"),
                        help="leave unset to keep the cluster's default; "
                             "setting it is a durability change and is "
                             "recorded in the run's metadata")
    parser.add_argument("--no-analyze", action="store_true",
                        help="skip VACUUM ANALYZE. Off by default because "
                             "without statistics PostgreSQL may not choose "
                             "its index at all, which makes the baseline a "
                             "coin toss rather than a baseline")
    parser.add_argument("--keep", action="store_true",
                        help="do not DROP the relations first")
    args = parser.parse_args()

    global ECHO
    ECHO = args.echo

    suffix = args.suffix or datetime.datetime.now().strftime("%H%M%S")
    sizes = sizes_for(args)
    rng = random.Random(args.seed + 1000)
    try:
        conn = PgConnection(host=args.host, port=args.port, user=args.user,
                            database=args.database, timeout=args.timeout)
    except (OSError, PgError) as e:
        abort(f"could not connect to {args.host}:{args.port}/{args.database}",
              f"{e}\n  bring the cluster up with: ./tools/pg_setup.sh init")
    client = Client(conn)
    started = datetime.datetime.now(datetime.timezone.utc)

    if args.synchronous_commit:
        client(f"SET synchronous_commit = {args.synchronous_commit}")
    sync_commit = conn.scalar("SHOW synchronous_commit")
    version = conn.scalar("SELECT version()")

    names = table_names(suffix)
    ddl = Phase("ddl", "CREATE TABLE x6")
    create_tables(client, names, ddl, args.keep)

    idx_before = Phase("create-index[write]", "on empty write relations")
    create_indexes(client, WRITE_ORDER, names, suffix, idx_before)

    loadp = Phase("load", "INSERT into the three read relations, batched")
    rows = make_rows(sizes, args.seed)
    load(client, names, READ_ORDER, rows, loadp, args.batch)

    idx_after = Phase("create-index[read]", "on loaded relations")
    create_indexes(client, ("idx", "cov"), names, suffix, idx_after)

    if not args.no_analyze:
        for tag in READ_ORDER + WRITE_ORDER:
            client(f"VACUUM ANALYZE {names[tag]}")

    plans = explain_shapes(client, names, sizes, args.seed + 3)

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
        "engine": "postgresql",
        "scenario": "index-benchmark",
        "columns": len(COLUMNS.split(",")),
        "rows": sizes["rows"],
        "host": args.host,
        "port": args.port,
        "database": args.database,
        "table": ", ".join(names[t] for t in READ_ORDER),
        "connections": 1,
        "sizes": sizes,
        "matches_per_key": args.matches,
        "ops_per_shape": args.ops,
        "write_ops": 0 if args.no_writes else args.write_ops,
        "update_ops": 0 if args.no_writes else args.update_ops,
        "seed": args.seed,
        "started_utc": started.isoformat(timespec="seconds"),
        "git": git_stamp(),
        "server_version": version.decode() if version else "",
        "synchronous_commit": sync_commit.decode() if sync_commit else "",
        "analyzed": not args.no_analyze,
        "plans": plans,
        "verify_problems": problems,
    }

    footer = [
        f"synchronous_commit = {meta['synchronous_commit']}, "
        f"VACUUM ANALYZE {'on' if not args.no_analyze else 'skipped'}",
        f"sizes: rows={sizes['rows']}, customers={sizes['customers']}",
        "EXPLAIN ANALYZE, one draw per shape - the scan node chosen:",
    ]
    for key in sorted(plans):
        footer.append(f"    {key:<22} {plans[key]}")
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
    conn.close()
    if problems:
        sys.exit(2)
    if client.errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
