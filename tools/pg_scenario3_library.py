#!/usr/bin/env python3
"""PostgreSQL twin of `scenario3_library.py`.

Same four relations, same row-set sizing, same twelve read shapes, same
arguments drawn from the same seeded generator - so the two runs are
directly comparable and `bench_common` prints tables that line up. The two
join-family shapes (`join-no-literal`, `exists-correlated`) are imported
from the ckdbs driver verbatim - both statements are dialect-identical -
so the twin cannot drift from the shape it is a baseline for.

What it is a baseline *for* is worth stating precisely, because scenario3
asks a question PostgreSQL answers only one way. KDS offers three
structures for a non-primary-key equality - a FilterScan, a Cabin, and a
secondary index - and PostgreSQL offers one, a btree index. So:

  * `--index-mode none` here means **sequential scan**, and is the honest
    baseline for KDS's FilterScan.
  * `--index-mode single|composite|covering|all` declares the equivalent
    PostgreSQL indexes, which PostgreSQL's planner will actually use. That
    is the number KDS's index read path is aiming at once IX10/IX11 land.
  * There is **no PostgreSQL equivalent of a Cabin**, and this driver does
    not invent one. A `--cabin` run has no twin column in the comparison
    table, and saying so is the correct output rather than substituting an
    index and calling it equivalent.

Everything the schema differs in is dialect, not shape: `bigserial primary
key` is how PostgreSQL spells the system-generated identity that KDS
carries in the Keystone word, and `text`/`integer`/`bigint` are the
spellings of KDS's `varchar`/`int32`/`int64`.

The cluster is `tools/pg_setup.sh` on port 15433, left at PostgreSQL's
defaults on purpose: a baseline tuned by hand is not a baseline.

Usage and flags: `bench/docs/README.md`.
"""

import argparse
import datetime
import random
import sys
import time

from bench_common import Phase, report, write_json
from pg_wire import DEFAULT_HOST, PgConnection, PgError
from scenario3_library import (
    BRANCHES, CREATE_ORDER, DAY0, EXISTS_OUTER_K, GENRES, INDEX_MODES,
    JOIN_OUTER_K, LOAN_WINDOW, MEMBER_TYPES, STATUS_OUT, STATUS_OVERDUE,
    STATUS_RETURNED, exists_correlated_stmt, git_stamp,
    join_no_literal_stmt, sizes_for,
)

# The same shape as scenario3_library.SCHEMA, in PostgreSQL's dialect. Kept
# as its own table rather than translated at runtime, because the two
# engines' type spellings do not map one to one and a translator would be a
# second place for the schema to live.
SCHEMA = {
    "users": (
        "id bigserial primary key, member_code text, name text, email text, "
        "joined_day integer, member_type integer, status integer, "
        "branch_id integer, fines_owed bigint"),
    "books": (
        "id bigserial primary key, isbn text, title text, author_id bigint, "
        "publisher_id integer, published_year integer, genre integer, "
        "branch_id integer, copies integer, status integer"),
    "reservations": (
        "id bigserial primary key, user_id bigint, book_id bigint, "
        "placed_day integer, expires_day integer, status integer, "
        "queue_pos integer"),
    "loans": (
        "id bigserial primary key, user_id bigint, book_id bigint, "
        "loaned_day integer, due_day integer, returned_day integer, "
        "status integer, renewals integer"),
}


def abort(message, reply=None):
    print(f"pg_scenario3 aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


class Client:
    def __init__(self, args):
        try:
            self._conn = PgConnection(args.host, args.port, args.user,
                                      args.database, args.password,
                                      timeout=args.timeout,
                                      application_name="pg_scenario3_library.py")
        except (OSError, PgError) as e:
            abort(f"could not connect to "
                  f"{args.host}:{args.port}/{args.database}: {e}\n"
                  f"  start one with: ./tools/pg_setup.sh init")
        self.errors = 0
        self.first_error = None
        if args.synchronous_commit:
            reply = self(f"SET synchronous_commit = {args.synchronous_commit}")
            if reply.startswith("ERR"):
                abort("SET synchronous_commit failed", reply)

    def __call__(self, command):
        tag, rows, _bytes, error = self._conn.query(command)
        if error is not None:
            self._note(command, f"ERR {error}")
            return f"ERR {error}"
        return tag or "OK"

    def rows(self, command):
        result, error = self._conn.fetch(command)
        if error is not None:
            self._note(command, f"ERR {error}")
            return None
        return [[None if v is None else v.decode() for v in row]
                for row in result]

    def timed(self, command, phase):
        t0 = time.perf_counter()
        result = self.rows(command)
        phase.record(time.perf_counter() - t0,
                     "ERR" if result is None else "OK")
        return result

    def _note(self, command, reply):
        self.errors += 1
        if self.first_error is None:
            self.first_error = f"{command}  ->  {reply}"

    def close(self):
        self._conn.close()


def send(client, phase, command):
    t0 = time.perf_counter()
    reply = client(command)
    phase.record(time.perf_counter() - t0, reply)
    return reply


# ---- DDL -----------------------------------------------------------------

def create_tables(client, suffix, phase):
    tables = {}
    for base in CREATE_ORDER:
        name = f"{base}_{suffix}"
        reply = send(client, phase, f"CREATE TABLE {name} ({SCHEMA[base]})")
        if reply.startswith("ERR"):
            abort(f"could not create {name}", reply)
        tables[base] = name
    return tables


def create_indexes(client, mode, tables, suffix, phase):
    """The same index set the ckdbs driver declares, in PostgreSQL's syntax.

    `COVERING (...)` becomes `INCLUDE (...)`, which is the same idea and the
    reason index.md §7 borrowed the concept: non-key payload columns
    stored in the leaf. PostgreSQL *can* answer index-only from one given a
    visible visibility map; KDS explicitly cannot (§7 says there is no
    index-only scan), and that asymmetry is a finding to report rather than
    a difference to hide."""
    names = []
    for name, base, keys, covering in INDEX_MODES[mode]:
        stmt = (f"CREATE INDEX {name}_{suffix} ON {tables[base]} "
                f"({', '.join(keys)})")
        if covering:
            stmt += f" INCLUDE ({', '.join(covering)})"
        reply = send(client, phase, stmt)
        if reply.startswith("ERR"):
            abort(f"could not create index {name}_{suffix}", reply)
        names.append(f"{name}_{suffix}")
    return names


def analyze(client, tables, phase):
    """PostgreSQL will not choose an index it has no statistics for, and a
    freshly loaded table has none until autovacuum gets to it. Running
    ANALYZE explicitly is not tuning - it is removing a race that would
    otherwise decide, run by run, whether the baseline used its index at
    all."""
    for name in tables.values():
        send(client, phase, f"ANALYZE {name}")


# ---- load ----------------------------------------------------------------

def insert_returning(client, phase, statement):
    t0 = time.perf_counter()
    result = client.rows(statement + " RETURNING id")
    phase.record(time.perf_counter() - t0, "ERR" if result is None else "OK")
    return int(result[0][0]) if result else None


def load_users(client, table, count, rng, phase):
    ids = []
    for i in range(count):
        got = insert_returning(
            client, phase,
            f"INSERT INTO {table} "
            f"(member_code, name, email, joined_day, member_type, status, "
            f"branch_id, fines_owed) VALUES "
            f"('M{i:07d}', 'member{i:07d}', 'm{i:07d}@library.test', "
            f"{DAY0 - rng.randint(0, 3650)}, {rng.randrange(MEMBER_TYPES)}, "
            f"0, {rng.randrange(BRANCHES)}, {rng.randrange(0, 5000)})")
        if got is not None:
            ids.append(got)
    return ids


def load_books(client, table, count, rng, phase):
    authors = max(count // 4, 2)
    ids = []
    for i in range(count):
        got = insert_returning(
            client, phase,
            f"INSERT INTO {table} "
            f"(isbn, title, author_id, publisher_id, published_year, genre, "
            f"branch_id, copies, status) VALUES "
            f"('978{i:010d}', 'title{i:07d}', {rng.randrange(authors)}, "
            f"{rng.randrange(64)}, {1950 + rng.randrange(75)}, "
            f"{rng.randrange(GENRES)}, {rng.randrange(BRANCHES)}, "
            f"{rng.randint(1, 6)}, 0)")
        if got is not None:
            ids.append(got)
    return ids


def load_reservations(client, table, count, users, books, rng, phase):
    for _ in range(count):
        placed = DAY0 - rng.randrange(LOAN_WINDOW)
        send(client, phase,
             f"INSERT INTO {table} "
             f"(user_id, book_id, placed_day, expires_day, status, queue_pos) "
             f"VALUES ({rng.choice(users)}, {rng.choice(books)}, {placed}, "
             f"{placed + 14}, {rng.randrange(3)}, {rng.randint(1, 5)})")


def load_loans(client, table, count, users, books, rng, phase):
    for _ in range(count):
        loaned = DAY0 - rng.randrange(LOAN_WINDOW)
        due = loaned + 21
        roll = rng.random()
        if roll < 0.34:
            status = STATUS_OVERDUE if due < DAY0 - 7 else STATUS_OUT
            returned = 0
        else:
            status = STATUS_RETURNED
            returned = min(due + rng.randint(-14, 7), DAY0)
        send(client, phase,
             f"INSERT INTO {table} "
             f"(user_id, book_id, loaned_day, due_day, returned_day, status, "
             f"renewals) VALUES ({rng.choice(users)}, {rng.choice(books)}, "
             f"{loaned}, {due}, {returned}, {status}, {rng.randrange(3)})")


# ---- the read shapes -----------------------------------------------------

def read_phases(client, tables, users, books, args, rng):
    """The same twelve shapes, in the same order, with the same argument
    distribution as the ckdbs driver."""
    ops = args.ops
    phases = []

    p = Phase("pk-user", "WHERE id = ? - index scan on the pk, the control")
    for _ in range(ops):
        client.timed(f"SELECT member_code, member_type, branch_id "
                     f"FROM {tables['users']} "
                     f"WHERE id = {rng.choice(users)}", p)
    phases.append(p)

    p = Phase("loans-by-user", f"WHERE user_id = ? - ~{args.matches} matches")
    for _ in range(ops):
        client.timed(f"SELECT book_id, loaned_day, due_day, status "
                     f"FROM {tables['loans']} "
                     f"WHERE user_id = {rng.choice(users)}", p)
    phases.append(p)

    p = Phase("loans-by-book", f"WHERE book_id = ? - ~{args.matches} matches")
    for _ in range(ops):
        client.timed(f"SELECT user_id, loaned_day, status "
                     f"FROM {tables['loans']} "
                     f"WHERE book_id = {rng.choice(books)}", p)
    phases.append(p)

    p = Phase("resv-by-user", "WHERE user_id = ? on reservations")
    for _ in range(ops):
        client.timed(f"SELECT book_id, placed_day, queue_pos "
                     f"FROM {tables['reservations']} "
                     f"WHERE user_id = {rng.choice(users)}", p)
    phases.append(p)

    p = Phase("books-by-author", "WHERE author_id = ? - middle selectivity")
    authors = max(len(books) // 4, 2)
    for _ in range(ops):
        client.timed(f"SELECT title, published_year, genre "
                     f"FROM {tables['books']} "
                     f"WHERE author_id = {rng.randrange(authors)}", p)
    phases.append(p)

    p = Phase("books-by-genre", f"WHERE genre = ? - 1/{GENRES} of the relation")
    for _ in range(ops):
        client.timed(f"SELECT title, author_id, branch_id "
                     f"FROM {tables['books']} "
                     f"WHERE genre = {rng.randrange(GENRES)}", p)
    phases.append(p)

    p = Phase("loans-by-daterange",
              "WHERE loaned_day BETWEEN ? AND ? - 30 days")
    for _ in range(ops):
        low = DAY0 - rng.randrange(LOAN_WINDOW)
        client.timed(f"SELECT user_id, book_id, due_day "
                     f"FROM {tables['loans']} "
                     f"WHERE loaned_day BETWEEN {low} AND {low + 30}", p)
    phases.append(p)

    p = Phase("overdue", "WHERE status = ? AND due_day BETWEEN ? AND ?")
    for _ in range(ops):
        low = DAY0 - rng.randrange(LOAN_WINDOW)
        client.timed(f"SELECT user_id, book_id, due_day "
                     f"FROM {tables['loans']} "
                     f"WHERE status = {STATUS_OVERDUE} "
                     f"AND due_day BETWEEN {low} AND {low + 60}", p)
    phases.append(p)

    p = Phase("join-loan-user", "loans JOIN users ON user_id = id")
    for _ in range(ops):
        client.timed(f"SELECT l.book_id, l.due_day, u.member_code "
                     f"FROM {tables['loans']} AS l "
                     f"JOIN {tables['users']} AS u ON l.user_id = u.id "
                     f"WHERE u.id = {rng.choice(users)}", p)
    phases.append(p)

    # The two join-family shapes, imported statement builders - fixed
    # literals, so they draw nothing from the seeded generator and the
    # later phases' argument streams stay aligned with the ckdbs run.
    p = Phase("join-no-literal",
              f"JOIN ON l.user_id = u.id, u.id BETWEEN 1 AND "
              f"{JOIN_OUTER_K} - no literal to propagate")
    stmt = join_no_literal_stmt(tables)
    for _ in range(ops):
        client.timed(stmt, p)
    phases.append(p)

    p = Phase("exists-correlated",
              f"EXISTS(loans WHERE user_id = users.id), "
              f"{EXISTS_OUTER_K} outer rows")
    stmt = exists_correlated_stmt(tables)
    for _ in range(ops):
        client.timed(stmt, p)
    phases.append(p)

    p = Phase("count-by-user", "SELECT COUNT(*) ... WHERE user_id = ?")
    for _ in range(ops):
        client.timed(f"SELECT COUNT(*) FROM {tables['loans']} "
                     f"WHERE user_id = {rng.choice(users)}", p)
    phases.append(p)

    return phases


def explain_shapes(client, tables, users):
    """`EXPLAIN` for the equality shapes, so the document can state whether
    PostgreSQL actually used the index rather than assuming a declared index
    is a used one.

    The ckdbs side has the same capability under a different name:
    `ANALYZE <statement>` names each step's access kind (`IndexProbe`,
    `IndexRange`, `FilterScan`, `Scan`) and an index step's `index_scanned`
    / `index_resolved` counts. Both drivers print their plans, so a
    comparison table can say what each engine actually did rather than what
    it was configured to do."""
    out = []
    probes = (
        ("loans-by-user",
         f"SELECT book_id FROM {tables['loans']} WHERE user_id = {users[0]}"),
        ("books-by-genre",
         f"SELECT title FROM {tables['books']} WHERE genre = 3"),
        ("overdue",
         f"SELECT user_id FROM {tables['loans']} "
         f"WHERE status = {STATUS_OVERDUE} AND due_day BETWEEN {DAY0 - 300} "
         f"AND {DAY0}"),
        ("join-no-literal", join_no_literal_stmt(tables)),
        ("exists-correlated", exists_correlated_stmt(tables)),
    )
    for name, stmt in probes:
        rows = client.rows(f"EXPLAIN {stmt}")
        plan = " / ".join(r[0] for r in (rows or []) if r and r[0])
        out.append(f"{name}: {plan[:160]}")
    return out


def main():
    parser = argparse.ArgumentParser(
        description="PostgreSQL twin of scenario3_library.py.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=15433)
    parser.add_argument("--user", default="ec2-user")
    parser.add_argument("--database", default="bench")
    parser.add_argument("--password", default=None)
    parser.add_argument("--suffix", default=None)
    parser.add_argument("--loans", type=int, default=1000)
    parser.add_argument("--matches", type=int, default=5)
    parser.add_argument("--index-mode", default="none",
                        choices=sorted(INDEX_MODES))
    parser.add_argument("--index-when", default="after",
                        choices=("before", "after"))
    parser.add_argument("--ops", type=int, default=200)
    parser.add_argument("--no-analyze", dest="analyze", action="store_false",
                        default=True,
                        help="skip ANALYZE after the load. Leaving it on is "
                             "the default because without statistics "
                             "PostgreSQL may not use the index at all, which "
                             "would make the baseline a coin toss")
    parser.add_argument("--synchronous-commit", default=None,
                        choices=("on", "off", "local", "remote_write"))
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--json", metavar="PATH")
    args = parser.parse_args()

    suffix = args.suffix or datetime.datetime.now().strftime("%H%M%S")
    rng = random.Random(args.seed)
    sizes = sizes_for(args)

    client = Client(args)
    started = datetime.datetime.now(datetime.timezone.utc)

    ddl = Phase("ddl", "CREATE TABLE x4")
    tables = create_tables(client, suffix, ddl)

    index_phase = Phase("create-index", f"--index-mode {args.index_mode}")
    if args.index_when == "before" and args.index_mode != "none":
        create_indexes(client, args.index_mode, tables, suffix, index_phase)

    load = Phase("load", "INSERT x all four relations")
    users = load_users(client, tables["users"], sizes["users"], rng, load)
    books = load_books(client, tables["books"], sizes["books"], rng, load)
    if not users or not books:
        abort("the load produced no users or no books")
    load_reservations(client, tables["reservations"], sizes["reservations"],
                      users, books, rng, load)
    load_loans(client, tables["loans"], sizes["loans"], users, books, rng, load)

    if args.index_when == "after" and args.index_mode != "none":
        create_indexes(client, args.index_mode, tables, suffix, index_phase)

    if args.analyze:
        analyze(client, tables, index_phase)

    phases = [ddl, load]
    if args.index_mode != "none":
        phases.append(index_phase)
    phases.extend(read_phases(client, tables, users, books, args, rng))

    plans = explain_shapes(client, tables, users)

    meta = {
        "engine": "postgresql",
        "scenario": "scenario3-library",
        "columns": sum(len(SCHEMA[b].split(",")) for b in CREATE_ORDER),
        "rows": sizes["loans"],
        "host": args.host,
        "port": args.port,
        "table": ", ".join(tables.values()),
        "connections": 1,
        "sizes": sizes,
        "index_mode": args.index_mode,
        "index_when": args.index_when,
        "cabin": False,
        "matches_per_key": args.matches,
        "ops_per_shape": args.ops,
        "seed": args.seed,
        "analyzed": args.analyze,
        "started_utc": started.isoformat(timespec="seconds"),
        "git": git_stamp(),
        "plans": plans,
    }

    footer = [
        f"index mode {args.index_mode} ({args.index_when} load), "
        f"analyze {'on' if args.analyze else 'off'}",
        "sizes: " + ", ".join(f"{k}={v}" for k, v in sizes.items()),
        "no Cabin equivalent exists here - a --cabin ckdbs run has no twin "
        "column in the comparison",
        "EXPLAIN:",
    ]
    footer.extend(f"    {line}" for line in plans)
    if client.errors:
        footer.append(f"{client.errors} error replies, "
                      f"first: {client.first_error}")

    report(phases, meta, footer)
    if args.json:
        write_json(args.json, meta, phases)
    client.close()
    if client.errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
