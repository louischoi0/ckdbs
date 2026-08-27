#!/usr/bin/env python3
"""Read performance against a secondary index: a library circulation system.

    users           members who borrow
    books           the catalogue
    reservations    a hold placed on a book
    loans           a book actually out - the bulk relation

The question this scenario exists to ask is narrow and, for this engine,
unusually well posed: **what does a non-primary-key equality cost, and what
can be done about it?** KDS offers three answers to that one question and
they sit in three different trust classes, which is why measuring them
against each other says something a single-structure benchmark cannot:

    a FilterScan     walk the whole chain, keep the rows that match
    a Cabin          authoritative *only for values queries have observed*
                     (docs/spec/cabin.md) - a hit needs no relation opened
    a secondary index  authoritative for every value, always maintained
                     (docs/spec/index.md)

PostgreSQL answers it exactly one way, with a btree index, which is what
makes it a clean baseline here rather than a second set of numbers.

---- The two ways to switch the index off ---------------------------------

The index read path is **built** (`docs/workplan-index.md` IX10-IX16), so a
statement with an equality or `BETWEEN` on an indexed column compiles to
`kIndexProbe` / `kIndexRange` and descends. There are two different A/B
levers and they answer different questions:

  * `--index-mode none` declares **no index at all**. The comparison
    against `single` therefore prices the index's *whole* cost - the
    backfill, the per-write maintenance (IX06) and the space - against its
    read benefit.
  * `--server-indexes off` leaves the indexes declared and maintained, and
    turns off only the read path (the `indexes` config key, IX13). Because
    the compiled chain is identical either way, this isolates the *read*
    benefit with every write-side cost still being paid.

Reporting only the first would credit the index for a saving whose cost is
in a phase the table does not show; reporting only the second would hide
that cost entirely. The matrix runs both.

`--assert-index-reads` fails the run if the equality shapes did not improve
under an index, so a regression in the read path cannot be reported as a
flat result.

---- Row-set sizes --------------------------------------------------------

`--loans` is the bulk relation and the row-set axis: **200 / 1,000 /
10,000** are the three sizes the benchmark documentation rules require.

The subtlety that matters for an index benchmark is that a bigger relation
must not also mean a *less selective* predicate, or the three sizes measure
two things at once and neither cleanly. So `users` and `books` are scaled
with `loans` to hold **matches per key** constant (`--matches`, default 5):

    users = loans / matches        books = loans / matches

A `WHERE user_id = ?` therefore returns ~5 rows at every size, out of 200,
1,000 or 10,000. That is the shape that isolates the thing under test: a
scan is O(rows) and an index probe is O(log rows + matches), so holding
matches fixed and growing rows is exactly the axis on which those two
diverge. `books-by-genre` is the deliberate counter-case - genre has fixed
cardinality (16), so its match count *does* grow with the relation, and it
is where an index is expected to pay least.

---- The read shapes ------------------------------------------------------

    pk-user             WHERE id = ?                      Lookup (control:
                        must not move when an index is added)
    loans-by-user       WHERE user_id = ?                 the headline shape
    loans-by-book       WHERE book_id = ?
    resv-by-user        WHERE user_id = ?
    books-by-author     WHERE author_id = ?
    books-by-genre      WHERE genre = ?                   low selectivity
    loans-by-daterange  WHERE loaned_day BETWEEN ? AND ?  a non-pk range
    overdue             WHERE status = ? AND due_day <= ? composite-key case
    join-loan-user      loans JOIN users ON user_id = id  Lookup + Probe
    join-no-literal     users JOIN loans ON l.user_id = u.id, u.id BETWEEN
                        1 AND 16 - no equality to propagate; the inner side
                        is IX17's IndexProbe / CB12's CabinProbe / a walk
    exists-correlated   EXISTS (... WHERE l.user_id = users.id) - the
                        correlated subquery over the same join column
    count-by-user       SELECT COUNT(*) ... WHERE user_id = ?

---- Running it -----------------------------------------------------------

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
# Every relation is BTREE, and that is a hard requirement rather than a
# preference: `docs/spec/index.md` §3 permits a secondary index only on a
# btree-clustered relation, because an index entry names a pk and resolving
# one needs a pk descent. A HEAP relation here would make `--index-mode`
# fail at CREATE INDEX with a refusal naming the storage form.
#
# `id` is never in a column list: it is the Keystone word, system-generated
# (invariant 11), so every INSERT below supplies the columns after it.
SCHEMA = {
    "users": (
        "id int64, member_code varchar, name varchar, email varchar, "
        "joined_day int32, member_type int32, status int32, branch_id int32, "
        "fines_owed int64", "BTREE"),
    "books": (
        "id int64, isbn varchar, title varchar, author_id int64, "
        "publisher_id int32, published_year int32, genre int32, "
        "branch_id int32, copies int32, status int32", "BTREE"),
    "reservations": (
        "id int64, user_id int64, book_id int64, placed_day int32, "
        "expires_day int32, status int32, queue_pos int32", "BTREE"),
    "loans": (
        "id int64, user_id int64, book_id int64, loaned_day int32, "
        "due_day int32, returned_day int32, status int32, renewals int32",
        "BTREE"),
}

CREATE_ORDER = ("users", "books", "reservations", "loans")

# ---- the indexes ---------------------------------------------------------
#
# Each mode is a list of (index_suffix, relation, key columns, covering
# columns). The name carries the run suffix too, because an index name is
# unique **instance-wide** (index.md §10) - two runs sharing a data
# file would collide on a bare `loans_user_idx`.
#
# `none` is the baseline and is deliberately an empty list rather than a
# missing key, so every mode goes through the same code.
INDEX_MODES = {
    "none": (),
    # One single-column index per hot equality column. This is the mode the
    # headline comparison uses.
    "single": (
        ("loans_user", "loans", ("user_id",), ()),
        ("loans_book", "loans", ("book_id",), ()),
        ("resv_user", "reservations", ("user_id",), ()),
        ("books_author", "books", ("author_id",), ()),
        ("books_genre", "books", ("genre",), ()),
    ),
    # Multi-column keys, for the two shapes that filter on two columns.
    # `overdue` is (status, due_day): a prefix probe on status followed by a
    # range on due_day is the composite case index.md §5 encodes for.
    "composite": (
        ("loans_status_due", "loans", ("status", "due_day"), ()),
        ("books_branch_genre", "books", ("branch_id", "genre"), ()),
    ),
    # COVERING (index.md §7). Note §7 is explicit that there is no
    # index-only scan: covering columns save a *decode*, not the pk descent.
    # Measuring how little that is worth is the point of including it.
    "covering": (
        ("loans_user_cov", "loans", ("user_id",),
         ("book_id", "due_day", "status")),
    ),
}

# `all` is every mode's indexes at once - the write-cost worst case, and the
# only mode where a relation carries more than one index.
INDEX_MODES["all"] = (INDEX_MODES["single"] + INDEX_MODES["composite"]
                      + INDEX_MODES["covering"])

# The Cabin is the other accelerator for a non-pk equality, and the reason
# this scenario can say something about *which* structure to reach for. One
# column only: docs/spec/cabin.md C3 keeps a Cabin single-column.
CABIN_RELATION = "loans"
CABIN_COLUMN = "user_id"
CABIN_TYPE = "int64"

# The two join-family shapes folded in from bench/results-scenario3-library.md
# §9b/§7b's session harnesses (§9b.7's task). Fixed literals on purpose:
# they draw nothing from the seeded generator, so their insertion leaves
# every other phase's argument stream untouched, and ASSIGNED ids issue from
# 1 per relation (invariant 11), so `BETWEEN 1 AND k` names the first k
# users in any data file, fresh or shared. k is fixed at 16 because the
# phase model is ops-of-one-statement; the k-sweep stays a harness's job.
JOIN_OUTER_K = 16       # outer rows for join-no-literal
EXISTS_OUTER_K = 20     # outer rows for exists-correlated

GENRES = 16
BRANCHES = 8
MEMBER_TYPES = 4
DAY0 = 20000            # an arbitrary epoch-day base, as scenario1/2 use
LOAN_WINDOW = 720       # loans are spread over this many days
STATUS_OUT, STATUS_RETURNED, STATUS_OVERDUE = 0, 1, 2

INSERTED_ID = re.compile(r"\bid=(\d+)")
ROW_COUNT = re.compile(r"\((\d+) rows?\)")


def abort(message, reply=None):
    print(f"scenario3 aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def connect(host, port, timeout):
    try:
        return ServerConnection(host, port, timeout=timeout)
    except OSError as e:
        abort(f"could not connect to {host}:{port}: {e}\n"
              f"  start one with: ./build-release/kds_server ~/library.db "
              f"--port {port}")


ECHO = False


class Client:
    """One connection and the one-command-one-reply callable everything else
    is written against. Counts errors, so a caller that does not inspect
    every reply still cannot report a clean run over a failing one."""

    def __init__(self, host, port, timeout):
        self._conn = connect(host, port, timeout)
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            print(f"[s3] {command}  ->  {reply[:96]}", file=sys.stderr,
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


def send(exec_, phase, command):
    t0 = time.perf_counter()
    reply = exec_(command)
    phase.record(time.perf_counter() - t0, reply)
    return reply


def inserted_id(reply):
    """The Keystone id the server issued, or None if the insert failed.

    Read back rather than derived from a counter: ids are unique and
    monotonic but **not gapless** (invariant 11), and a failed insert burns
    one - so a run that hits an error must not go on addressing rows by
    ordinal."""
    got = INSERTED_ID.search(reply)
    return int(got.group(1)) if got else None


def select_rows(reply):
    """The data rows of a SELECT reply, as lists of field strings.

    The wire form is CSV with a **header line first** - `count(*)\\n200` for
    a fold, `user_id,book_id\\n2,19\\n...` for a projection - so the header is
    dropped and the rest split on commas. This driver's generated values
    contain no commas by construction (member codes, ISBNs and titles are
    alphanumeric), which is what makes a split legitimate instead of a
    parser.

    An error reply and a genuinely empty result must not look alike here:
    the caller checks `ERR` itself, and this returns [] for both, which is
    why every read phase counts errors through the Phase rather than by
    inspecting the row list."""
    if reply.startswith("ERR"):
        return []
    lines = [line for line in reply.splitlines() if line.strip()]
    if len(lines) < 2:
        return []
    return [[f.strip() for f in line.split(",")] for line in lines[1:]]


def row_count(reply):
    return len(select_rows(reply))


# ---- DDL -----------------------------------------------------------------

def schema_for(base, cabin):
    columns, clustered = SCHEMA[base]
    if cabin and base == CABIN_RELATION:
        columns = columns.replace(f"{CABIN_COLUMN} {CABIN_TYPE}",
                                  f"{CABIN_COLUMN} {CABIN_TYPE} CABIN", 1)
    return columns, clustered


def create_tables(exec_, suffix, phase, cabin=False):
    created = {}
    for base in CREATE_ORDER:
        columns, clustered = schema_for(base, cabin)
        name = f"{base}_{suffix}"
        reply = send(exec_, phase,
                     f"CREATE TABLE {name} ({columns}) {clustered}")
        if reply.startswith("ERR"):
            explain_ddl_failure(base, reply, cabin)
            abort(f"could not create {name}", reply)
        created[base] = name
    return created


def explain_ddl_failure(base, reply, cabin):
    """Turns the refusals this schema can actually provoke into an error
    naming the flag responsible, rather than a syntax error pointing into
    the middle of a column definition."""
    upper = reply.upper()
    if cabin and base == CABIN_RELATION and "CABIN" in upper:
        abort(f"--cabin: this server does not understand the column cabin "
              f"policy.\n  `{CABIN_RELATION}.{CABIN_COLUMN} {CABIN_TYPE} "
              f"CABIN` needs a build with docs/spec/cabin.md in it; re-run "
              f"without --cabin, or rebuild the server.", reply)
    if "no room" in reply or "reserved catalog page range" in reply:
        abort(f"could not create {base}: the catalog is out of column space.\n"
              f"  Catalog relations chain into a reserved range and nothing "
              f"reclaims a row, because there is no DROP TABLE.\n"
              f"  Restart the server on a fresh data file.", reply)


def index_statements(mode, tables, suffix):
    """The `CREATE INDEX` statements for `mode`, already formatted."""
    out = []
    for name, base, keys, covering in INDEX_MODES[mode]:
        stmt = (f"CREATE INDEX {name}_{suffix} ON {tables[base]} "
                f"({', '.join(keys)})")
        if covering:
            stmt += f" COVERING ({', '.join(covering)})"
        out.append((f"{name}_{suffix}", stmt))
    return out


def create_indexes(exec_, mode, tables, suffix, phase):
    """Declares the mode's indexes. Returns their names.

    Every failure aborts rather than degrading to an unindexed run: a
    benchmark whose index silently did not exist would report the baseline
    twice and call the second one a result."""
    names = []
    for name, stmt in index_statements(mode, tables, suffix):
        reply = send(exec_, phase, stmt)
        if reply.startswith("ERR"):
            explain_index_failure(stmt, reply)
            abort(f"could not create index {name}", reply)
        names.append(name)
    return names


def explain_index_failure(stmt, reply):
    lowered = reply.lower()
    if "unsupported" in lowered or "expected" in lowered:
        abort(f"this server does not understand CREATE INDEX.\n"
              f"  `{stmt}`\n"
              f"  Secondary indexes need a build with docs/spec/index.md in "
              f"it (IX05 for the grammar, IX09 for the backfill). Re-run "
              f"with --index-mode none, or rebuild the server.", reply)
    if "heap" in lowered:
        abort(f"CREATE INDEX refused a heap relation.\n"
              f"  `{stmt}`\n"
              f"  index.md §3 permits an index only on a btree-"
              f"clustered relation. This scenario declares all four BTREE, "
              f"so seeing this means SCHEMA was edited.", reply)
    if "ever held a row" in lowered or "populated" in lowered:
        abort(f"CREATE INDEX refused a populated relation.\n"
              f"  `{stmt}`\n"
              f"  That refusal is lifted by IX09 (the backfill). This build "
              f"predates it: re-run with --index-when before, which declares "
              f"the index on an empty relation and lets the write hook "
              f"maintain it.", reply)


def join_no_literal_stmt(tables):
    """The no-literal join (results §9b): `BETWEEN` gives the join its outer
    rows and no equality any rewrite could push onto `loans`, so the inner
    side is whatever structure the join column has — an index (IX17), a
    Cabin (CB12), or a per-outer-row walk. Dialect-identical in PostgreSQL;
    the twin imports this builder rather than restating it."""
    return (f"SELECT l.book_id, u.member_code "
            f"FROM {tables['users']} AS u JOIN {tables['loans']} AS l "
            f"ON l.user_id = u.id "
            f"WHERE u.id BETWEEN 1 AND {JOIN_OUTER_K}")


def exists_correlated_stmt(tables):
    """The correlated EXISTS over the same join column (results §7b/§9b.4).
    The correlated reference is spelled with the relation name, as the
    measured harnesses spelled it."""
    return (f"SELECT id FROM {tables['users']} "
            f"WHERE id BETWEEN 1 AND {EXISTS_OUTER_K} AND EXISTS "
            f"(SELECT l.id FROM {tables['loans']} AS l "
            f"WHERE l.user_id = {tables['users']}.id)")


ANALYZE_STEP = re.compile(r"^step \d+ (\w+) ", re.M)


def analyze_shapes(exec_, tables, users, books):
    """`ANALYZE <statement>` for the shapes an index is supposed to serve.

    This is KDS's EXPLAIN, and it is what makes `--assert-index-reads` a
    check on the *plan* rather than a guess from latency: the reply names
    the access kind per step (`IndexProbe`, `IndexRange`, `FilterScan`,
    `Lookup`) and, for an index step, `index_scanned` / `index_resolved`.
    A latency-based assertion would pass a plan that regressed to a scan on
    a small enough relation."""
    probes = (
        ("loans-by-user",
         f"SELECT book_id FROM {tables['loans']} WHERE user_id = {users[0]}"),
        ("loans-by-book",
         f"SELECT user_id FROM {tables['loans']} WHERE book_id = {books[0]}"),
        ("resv-by-user",
         f"SELECT book_id FROM {tables['reservations']} "
         f"WHERE user_id = {users[0]}"),
        ("books-by-author",
         f"SELECT title FROM {tables['books']} WHERE author_id = 0"),
        ("books-by-genre",
         f"SELECT title FROM {tables['books']} WHERE genre = 3"),
        ("loans-by-daterange",
         f"SELECT user_id FROM {tables['loans']} "
         f"WHERE loaned_day BETWEEN {DAY0 - 300} AND {DAY0 - 270}"),
        ("overdue",
         f"SELECT user_id FROM {tables['loans']} "
         f"WHERE status = {STATUS_OVERDUE} "
         f"AND due_day BETWEEN {DAY0 - 300} AND {DAY0}"),
        ("join-no-literal", join_no_literal_stmt(tables)),
        ("exists-correlated", exists_correlated_stmt(tables)),
    )
    out = {}
    for name, stmt in probes:
        reply = exec_(f"ANALYZE {stmt}")
        # ANALYZE prints each step twice - once in the plan, once again in
        # the per-step statistics line - so the same kind appears twice for
        # a one-step chain. Dedupe in order rather than with a set, because
        # a two-step chain's order is the execution order and is the thing
        # worth reading.
        kinds = []
        for kind in ANALYZE_STEP.findall(reply):
            if not kinds or kinds[-1] != kind:
                kinds.append(kind)
        # A multi-step chain survives the consecutive dedupe as the whole
        # sequence twice (plan section, then statistics section:
        # Range, Scan, Range, Scan). One listing of the chain is the plan;
        # fold the exact repetition.
        half = len(kinds) // 2
        if kinds[:half] == kinds[half:]:
            kinds = kinds[:half]
        out[name] = {
            "kinds": kinds,
            "line": " ".join(reply.split("\n")[0:1]),
            "raw": reply,
        }
    return out


# The shapes whose predicate column `--index-mode single` indexes. Only
# these are expected to reach an index; the others are here as the control
# that says a plan did not change for a reason unrelated to the index.
# For the last two, the indexed step is the *inner* one — IX17's correlated
# probe on loans.user_id, keyed per outer row — so `--assert-index-reads`
# now also fails a run in which IX17 regressed to the walk.
INDEXED_BY_SINGLE = ("loans-by-user", "loans-by-book", "resv-by-user",
                     "books-by-author", "books-by-genre",
                     "join-no-literal", "exists-correlated")
INDEXED_BY_COMPOSITE = ("overdue",)


def show_indexes(exec_):
    """`SHOW INDEXES`, as printed. Carries height and entry counts walked
    from the tree, which is the only way to tell a declared index from a
    populated one - the catalog can say an index exists and never what is
    in it (index.md §10)."""
    return exec_("SHOW INDEXES")


# ---- sizing --------------------------------------------------------------

def sizes_for(args):
    """Row counts, with matches-per-key held constant across the sweep.

    This is the whole reason the three row-set sizes are comparable. If
    `users` were fixed while `loans` grew, `WHERE user_id = ?` would return
    5 rows at 200 loans and 250 at 10,000 - so the shape's cost would grow
    for two reasons at once and the measurement could not separate "the
    relation got bigger" from "the answer got bigger"."""
    loans = max(args.loans, 20)
    users = max(loans // max(args.matches, 1), 4)
    books = max(loans // max(args.matches, 1), 4)
    reservations = max(loans // 2, 4)
    return {"users": users, "books": books,
            "reservations": reservations, "loans": loans}


# ---- load ----------------------------------------------------------------

def load_users(exec_, table, count, rng, phase):
    ids = []
    for i in range(count):
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"('M{i:07d}', 'member{i:07d}', "
                     f"'m{i:07d}@library.test', "
                     f"{DAY0 - rng.randint(0, 3650)}, "
                     f"{rng.randrange(MEMBER_TYPES)}, 0, "
                     f"{rng.randrange(BRANCHES)}, "
                     f"{rng.randrange(0, 5000)})")
        got = inserted_id(reply)
        if got is not None:
            ids.append(got)
    return ids


def load_books(exec_, table, count, rng, phase):
    """Authors are deliberately fewer than books, so `books-by-author` has a
    match count above 1 and below the genre shape's - a middle selectivity
    between the two extremes the other shapes cover."""
    authors = max(count // 4, 2)
    ids = []
    for i in range(count):
        reply = send(exec_, phase,
                     f"INSERT INTO {table} VALUES "
                     f"('978{i:010d}', 'title{i:07d}', "
                     f"{rng.randrange(authors)}, "
                     f"{rng.randrange(64)}, "
                     f"{1950 + rng.randrange(75)}, "
                     f"{rng.randrange(GENRES)}, "
                     f"{rng.randrange(BRANCHES)}, "
                     f"{rng.randint(1, 6)}, 0)")
        got = inserted_id(reply)
        if got is not None:
            ids.append(got)
    return ids


def load_reservations(exec_, table, count, users, books, rng, phase):
    for i in range(count):
        placed = DAY0 - rng.randrange(LOAN_WINDOW)
        send(exec_, phase,
             f"INSERT INTO {table} VALUES "
             f"({rng.choice(users)}, {rng.choice(books)}, "
             f"{placed}, {placed + 14}, {rng.randrange(3)}, "
             f"{rng.randint(1, 5)})")


def load_loans(exec_, table, count, users, books, rng, phase):
    """The bulk relation, and the one `--loans` sizes.

    A third of the loans are left out (`returned_day = 0`) and a slice of
    those is overdue, so the `overdue` shape has a real, non-degenerate
    answer at every size rather than matching everything or nothing."""
    for i in range(count):
        loaned = DAY0 - rng.randrange(LOAN_WINDOW)
        due = loaned + 21
        roll = rng.random()
        if roll < 0.34:
            status = STATUS_OVERDUE if due < DAY0 - 7 else STATUS_OUT
            returned = 0
        else:
            status = STATUS_RETURNED
            returned = min(due + rng.randint(-14, 7), DAY0)
        send(exec_, phase,
             f"INSERT INTO {table} VALUES "
             f"({rng.choice(users)}, {rng.choice(books)}, "
             f"{loaned}, {due}, {returned}, {status}, "
             f"{rng.randrange(3)})")


# ---- the read shapes -----------------------------------------------------

def read_phases(client, tables, users, books, args, rng):
    """Every read shape, `--ops` times each, against one connection.

    Arguments are drawn fresh per operation from the loaded id lists rather
    than from a fixed hot key, because a single repeated argument would be
    served by the probe memo and by a Waystone trail and would measure
    neither the scan nor the index."""
    ops = args.ops
    phases = []

    # The control. A pk lookup descends the clustered tree and touches no
    # secondary structure, so it must not move when an index is added. It
    # is here to prove that rather than to be assumed.
    p = Phase("pk-user", "WHERE id = ? - Lookup, the control")
    for _ in range(ops):
        client.timed(f"SELECT member_code, member_type, branch_id "
                     f"FROM {tables['users']} "
                     f"WHERE id = {rng.choice(users)}", p)
    phases.append(p)

    # The headline. A non-pk equality: FilterScan today, kIndexProbe once
    # IX10/IX11 land, and a Cabin probe under --cabin.
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

    # The counter-case: genre cardinality is fixed at 16, so this shape's
    # match count grows with the relation. An index is expected to pay
    # least here, and at 10,000 rows may not pay at all.
    p = Phase("books-by-genre", f"WHERE genre = ? - 1/{GENRES} of the relation")
    for _ in range(ops):
        client.timed(f"SELECT title, author_id, branch_id "
                     f"FROM {tables['books']} "
                     f"WHERE genre = {rng.randrange(GENRES)}", p)
    phases.append(p)

    # A non-pk range. `kRange` is pk-only on this engine (CLAUDE.md), so
    # this is a FilterScan today and is what kIndexRange would serve.
    p = Phase("loans-by-daterange", "WHERE loaned_day BETWEEN ? AND ? - 30 days")
    for _ in range(ops):
        low = DAY0 - rng.randrange(LOAN_WINDOW)
        client.timed(f"SELECT user_id, book_id, due_day "
                     f"FROM {tables['loans']} "
                     f"WHERE loaned_day BETWEEN {low} AND {low + 30}", p)
    phases.append(p)

    # Two columns, which is what the composite mode's key is for.
    p = Phase("overdue", "WHERE status = ? AND due_day BETWEEN ? AND ?")
    for _ in range(ops):
        low = DAY0 - rng.randrange(LOAN_WINDOW)
        client.timed(f"SELECT user_id, book_id, due_day "
                     f"FROM {tables['loans']} "
                     f"WHERE status = {STATUS_OVERDUE} "
                     f"AND due_day BETWEEN {low} AND {low + 60}", p)
    phases.append(p)

    # A join chain: a pk Lookup on loans feeding a pk Probe into users.
    # Written order is execution order (docs/spec/parser-v2.md), so this is a
    # Lookup then a Probe and never reordered.
    p = Phase("join-loan-user", "loans JOIN users ON user_id = id")
    for _ in range(ops):
        client.timed(f"SELECT l.book_id, l.due_day, u.member_code "
                     f"FROM {tables['loans']} AS l "
                     f"JOIN {tables['users']} AS u ON l.user_id = u.id "
                     f"WHERE u.id = {rng.choice(users)}", p)
    phases.append(p)

    # The no-literal join - the shape and its three inner-side answers are
    # join_no_literal_stmt()'s docstring.
    p = Phase("join-no-literal",
              f"JOIN ON user_id, u.id BETWEEN 1 AND {JOIN_OUTER_K} - "
              f"no literal to propagate")
    stmt = join_no_literal_stmt(tables)
    for _ in range(ops):
        client.timed(stmt, p)
    phases.append(p)

    # Under --cabin the early ops pay CB14's per-key observation charge,
    # so the mean honestly mixes the warm-up; p50 is the served cost.
    p = Phase("exists-correlated",
              f"EXISTS(loans.user_id = users.id), {EXISTS_OUTER_K} outer "
              f"rows - under --cabin the mean mixes the observation charge")
    stmt = exists_correlated_stmt(tables)
    for _ in range(ops):
        client.timed(stmt, p)
    phases.append(p)

    # The fold over an indexed filter. AG1 puts the fold outside the
    # executor and the compiled chain is byte-identical to the same
    # statement without the aggregate, so this shape's cost above
    # `loans-by-user` is the fold and nothing else.
    p = Phase("count-by-user", "SELECT COUNT(*) ... WHERE user_id = ?")
    for _ in range(ops):
        client.timed(f"SELECT COUNT(*) FROM {tables['loans']} "
                     f"WHERE user_id = {rng.choice(users)}", p)
    phases.append(p)

    return phases


# ---- verification --------------------------------------------------------

def verify(client, tables, sizes, users, sample, rng):
    """Five invariants. A throughput number over a workload that lost rows
    measures nothing, and an index that lost rows is exactly the failure
    mode this scenario is built to catch.

    1. Each relation holds the row count the load claims.
    2. A `WHERE user_id = ?` answer equals the same rows found by a full
       scan filtered client-side. This is the one that would catch an
       index serving an incomplete set - the failure cabin.md §5 calls
       invisible without a baseline, and index.md §1 inherits.
    3. Every loan's user_id names a user that exists.
    4. The no-literal join's reply equals the expectation computed from two
       full scans client-side, compared **row for row in order**: outer
       rows ascend the pk range, and each outer row's matches arrive in
       loans pk order, which is also what a full scan of a BTREE relation
       returns. An index probe resolves entries in (key, pk) order and a
       Cabin serves its entry set in observation order - both collapse to
       the same sequence here - so a structure serving an extra, missing
       or reordered set fails this check, not just a smaller one. One
       honest limit: the projection carries no key column, so a row
       displaced across keys is caught only when its book_id differs at
       that position - the price of keeping §9b.1's measured statement
       verbatim.
    5. The correlated EXISTS's id list, likewise exact and ordered.
    """
    problems = []

    for base, expected in sizes.items():
        reply = client(f"SELECT COUNT(*) FROM {tables[base]}")
        rows = select_rows(reply)
        got = int(rows[0][0]) if rows and rows[0] and rows[0][0].isdigit() else -1
        if got != expected:
            problems.append(f"{base}: COUNT(*) = {got}, loaded {expected}")

    # The full scan, once, as the ground truth for check 2.
    truth = {}
    reply = client(f"SELECT user_id, book_id FROM {tables['loans']}")
    for row in select_rows(reply):
        if len(row) >= 2 and row[0].lstrip("-").isdigit():
            truth.setdefault(int(row[0]), []).append(row[1])

    for uid in rng.sample(users, min(sample, len(users))):
        reply = client(f"SELECT book_id FROM {tables['loans']} "
                       f"WHERE user_id = {uid}")
        got = sorted(r[0] for r in select_rows(reply) if r)
        want = sorted(truth.get(uid, []))
        if got != want:
            problems.append(
                f"loans WHERE user_id = {uid}: {len(got)} rows, "
                f"scan says {len(want)}")

    known = set(users)
    for uid in list(truth)[:sample]:
        if uid not in known:
            problems.append(f"loans.user_id = {uid} names no user")

    # Checks 4 and 5: the two join-family shapes, against expectations
    # built from `truth` plus one users scan. Exact-ordered comparison -
    # `select_rows` keeps the reply's row order and field spelling, so this
    # is byte-level equality of the data rows.
    members = {}
    for row in select_rows(
            client(f"SELECT id, member_code FROM {tables['users']}")):
        if len(row) >= 2 and row[0].isdigit():
            members[int(row[0])] = row[1]

    def exact_match(label, got, want, unit):
        if got == want:
            return
        same = (" (same count - wrong content or wrong order)"
                if len(got) == len(want) else "")
        problems.append(f"{label}: {len(got)} {unit}, "
                        f"expectation {len(want)}{same}")

    want = []
    for uid in range(1, JOIN_OUTER_K + 1):
        if uid in members:
            want.extend([book, members[uid]] for book in truth.get(uid, []))
    exact_match("join-no-literal",
                select_rows(client(join_no_literal_stmt(tables))), want, "rows")

    want = [[str(uid)] for uid in range(1, EXISTS_OUTER_K + 1)
            if uid in members and truth.get(uid)]
    exact_match("exists-correlated",
                select_rows(client(exists_correlated_stmt(tables))), want, "ids")

    return problems


# ---- reporting -----------------------------------------------------------

def server_meta(client):
    reply = client("SHOW META")
    return reply.replace("\n", "; ")[:200]


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


def main():
    parser = argparse.ArgumentParser(
        description="Library circulation: read performance against a "
                    "secondary index.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--suffix", default=None,
                        help="relation-name suffix, so runs can share a data "
                             "file (default: a timestamp)")
    parser.add_argument("--loans", type=int, default=1000,
                        help="the bulk relation, and the row-set axis. The "
                             "documented sweep is 200 / 1000 / 10000 "
                             "(default: 1000)")
    parser.add_argument("--matches", type=int, default=5,
                        help="rows per key for the equality shapes; users "
                             "and books are scaled to hold it constant "
                             "across the sweep (default: 5)")
    parser.add_argument("--index-mode", default="none",
                        choices=sorted(INDEX_MODES),
                        help="which secondary indexes to declare "
                             "(default: none)")
    parser.add_argument("--index-when", default="after",
                        choices=("before", "after"),
                        help="`after` declares the indexes on the loaded "
                             "relations, exercising the IX09 backfill and "
                             "timing it; `before` declares them empty and "
                             "lets the IX06 write hook maintain them, which "
                             "moves the cost into the load phase "
                             "(default: after)")
    parser.add_argument("--cabin", action="store_true",
                        help=f"declare a Cabin on "
                             f"{CABIN_RELATION}.{CABIN_COLUMN} - the other "
                             f"accelerator for a non-pk equality")
    parser.add_argument("--ops", type=int, default=200,
                        help="operations per read shape (default: 200)")
    parser.add_argument("--verify", type=int, default=25, metavar="N",
                        help="check the five invariants over a sample of N; "
                             "0 disables (default: 25)")
    parser.add_argument("--assert-index-reads", action="store_true",
                        help="fail the run if the shapes this --index-mode "
                             "indexes did not compile to IndexProbe / "
                             "IndexRange. Checks the plan through ANALYZE, "
                             "not the latency, so a regression to a scan "
                             "cannot pass on a small relation.")
    parser.add_argument("--server-indexes", default="unknown",
                        choices=("on", "off", "unknown"),
                        help="**a label, not a switch.** The `indexes` "
                             "config key is read at server start and there "
                             "is no runtime SET for it, so the caller starts "
                             "the server with `indexes = off` and records "
                             "that here. It lands in the JSON so a matrix "
                             "cell cannot lose which engine it ran against.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--echo", action="store_true")
    parser.add_argument("--json", metavar="PATH")
    args = parser.parse_args()

    global ECHO
    ECHO = args.echo

    suffix = args.suffix or datetime.datetime.now().strftime("%H%M%S")
    rng = random.Random(args.seed)
    sizes = sizes_for(args)

    client = Client(args.host, args.port, args.timeout)
    started = datetime.datetime.now(datetime.timezone.utc)

    ddl = Phase("ddl", "CREATE TABLE x4")
    tables = create_tables(client, suffix, ddl, cabin=args.cabin)

    index_phase = Phase("create-index", f"--index-mode {args.index_mode}")
    if args.index_when == "before" and args.index_mode != "none":
        create_indexes(client, args.index_mode, tables, suffix, index_phase)

    load = Phase("load", "INSERT x all four relations")
    users = load_users(client, tables["users"], sizes["users"], rng, load)
    books = load_books(client, tables["books"], sizes["books"], rng, load)
    if not users or not books:
        abort("the load produced no users or no books; nothing below can run")
    load_reservations(client, tables["reservations"], sizes["reservations"],
                      users, books, rng, load)
    load_loans(client, tables["loans"], sizes["loans"], users, books, rng, load)

    if args.index_when == "after" and args.index_mode != "none":
        create_indexes(client, args.index_mode, tables, suffix, index_phase)

    phases = [ddl, load]
    if args.index_mode != "none":
        phases.append(index_phase)
    phases.extend(read_phases(client, tables, users, books, args, rng))

    plans = analyze_shapes(client, tables, users, books)

    problems = []
    if args.verify:
        problems = verify(client, tables, sizes, users, args.verify, rng)

    meta = {
        "engine": "ckdbs",
        "scenario": "scenario3-library",
        "columns": sum(len(SCHEMA[b][0].split(",")) for b in CREATE_ORDER),
        "rows": sizes["loans"],
        "host": args.host,
        "port": args.port,
        "table": ", ".join(tables.values()),
        "connections": 1,
        "sizes": sizes,
        "index_mode": args.index_mode,
        "index_when": args.index_when,
        "server_indexes": args.server_indexes,
        "cabin": args.cabin,
        "matches_per_key": args.matches,
        "ops_per_shape": args.ops,
        "seed": args.seed,
        "started_utc": started.isoformat(timespec="seconds"),
        "git": git_stamp(),
        "server_meta": server_meta(client),
        "plans": {k: v["kinds"] for k, v in plans.items()},
        "verify_problems": problems,
    }

    footer = [
        f"index mode {args.index_mode} ({args.index_when} load), "
        f"cabin {'on' if args.cabin else 'off'}, "
        f"server indexes {args.server_indexes}",
        "sizes: " + ", ".join(f"{k}={v}" for k, v in sizes.items()),
        "ANALYZE (the access kind each shape compiled to):",
    ]
    for name, got in plans.items():
        footer.append(f"    {name}: {', '.join(got['kinds']) or '-'}")
    if args.index_mode != "none":
        footer.append("SHOW INDEXES:")
        for line in show_indexes(client).splitlines():
            footer.append(f"    {line}")
    if problems:
        footer.append(f"VERIFY FAILED - {len(problems)} problem(s):")
        for line in problems[:10]:
            footer.append(f"    {line}")
    if client.errors:
        footer.append(f"{client.errors} error replies, "
                      f"first: {client.first_error}")

    report(phases, meta, footer)
    if args.json:
        write_json(args.json, meta, phases)

    client.close()

    if problems:
        sys.exit(2)
    if args.assert_index_reads and args.index_mode != "none":
        expected = []
        if args.index_mode in ("single", "all"):
            expected.extend(INDEXED_BY_SINGLE)
        if args.index_mode in ("composite", "all"):
            expected.extend(INDEXED_BY_COMPOSITE)
        if args.index_mode == "covering":
            expected.append("loans-by-user")
        missed = [name for name in expected
                  if not any(k.startswith("Index")
                             for k in plans.get(name, {}).get("kinds", ()))]
        if missed:
            abort("--assert-index-reads: these shapes have an index on "
                  "their predicate column and did not compile to an index "
                  "step: " + ", ".join(missed) +
                  "\n  ANALYZE says: " +
                  "; ".join(f"{n}={plans[n]['kinds']}" for n in missed))
    if client.errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
