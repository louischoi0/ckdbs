#!/usr/bin/env python3
"""The inner build's k-sweep: one statement shape, k as the parameter.

`scenario3_library.py` fixes `JOIN_OUTER_K` at 16 on purpose — its phase
model is ops-of-one-statement — and says in the same comment that the
k-sweep is a harness's job. This is that harness, and it exists because
the statement-local inner build's whole economics is a function of k: the
build is paid once per statement and amortized over the outer rows, so
k = 1 pays it with no payback and k = 16 pays it once for sixteen walks
(`docs/workplan-join-inner-build.md`, "The build constant"). `--shape`
picks the walked join or the stopping correlated `EXISTS`, whose
prefix map is the same economics over a walk its own sink cuts.

It reuses the driver's relations, seeding and statement builder rather
than restating them, so a number here is comparable with a number there.
Run it against a server started by `bench/run_cell.sh`, which is what
gives each cell a fresh data file and records the contention:

    S3ROOT=~/bench-jb DRIVER=./tools/join_ksweep.py \\
        ./bench/run_cell.sh mycell my.conf -- --loans 10000 --ks 1,2,4,16

The A/B lever is `join_build_max_rows` in the config (0 disables the
build outright), which is why two cells of this harness under two configs
of one binary can price the build without a cross-commit comparison —
the placement band that workplan documents cannot confound a config
lever.
"""
import argparse
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import scenario3_library as s3  # noqa: E402
from bench_common import Phase  # noqa: E402


def join_stmt(tables, k):
    """The driver's `join-no-literal` with k lifted out of the module."""
    return (f"SELECT l.book_id, u.member_code "
            f"FROM {tables['users']} AS u JOIN {tables['loans']} AS l "
            f"ON l.user_id = u.id "
            f"WHERE u.id BETWEEN 1 AND {k}")


def exists_stmt(tables, k):
    """The driver's `exists-correlated`, k lifted out the same way.

    A different shape of the same economics: the inner walk **stops** at
    the first qualifying row, so what the build fills is a prefix rather
    than a whole relation (spec §6, workplan JB6), and k decides how much
    of that prefix a later outer row can be answered from.
    """
    return (f"SELECT id FROM {tables['users']} "
            f"WHERE id BETWEEN 1 AND {k} AND EXISTS "
            f"(SELECT l.id FROM {tables['loans']} AS l "
            f"WHERE l.user_id = {tables['users']}.id)")


SHAPES = {"join": join_stmt, "exists": exists_stmt}


def pct(values, q):
    ordered = sorted(values)
    idx = min(len(ordered) - 1, int(q * len(ordered)))
    return ordered[idx] * 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15432)
    ap.add_argument("--loans", type=int, default=10000)
    ap.add_argument("--matches", type=int, default=5)
    ap.add_argument("--ops", type=int, default=40)
    ap.add_argument("--ks", default="1,2,4,16")
    ap.add_argument("--shape", default="join", choices=sorted(SHAPES),
                    help="join = the walked join (JB3-JB5); exists = the "
                         "stopping correlated sub-chain (JB6)")
    ap.add_argument("--suffix", default=None)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--label", default="")
    # run_cell.sh passes --json to every driver it invokes. This harness
    # prints a table rather than writing one, and refusing the flag would
    # make it unusable through the only runner that records a cell's
    # machine state.
    ap.add_argument("--json", default=None, help=argparse.SUPPRESS)
    args = ap.parse_args()

    suffix = args.suffix or f"k{os.getpid()}"
    rng = random.Random(args.seed)
    client = s3.Client(args.host, args.port, args.timeout)
    load = Phase("load")

    sizes = s3.sizes_for(args)
    tables = s3.create_tables(client, suffix, load)
    users = s3.load_users(client, tables["users"], sizes["users"], rng, load)
    books = s3.load_books(client, tables["books"], sizes["books"], rng, load)
    s3.load_loans(client, tables["loans"], sizes["loans"], users, books, rng, load)
    if client.errors:
        print(f"!! {client.errors} errors seeding: {client.first_error}", file=sys.stderr)
        sys.exit(3)

    ks = [int(x) for x in args.ks.split(",")]
    stmt_for = SHAPES[args.shape]
    print(f"# label={args.label} shape={args.shape} loans={sizes['loans']} "
          f"users={sizes['users']} ops={args.ops}")
    print(f"{'k':>4} {'rows':>6} {'p0':>10} {'p25':>10} {'p50':>10} "
          f"{'p95':>10} {'p99':>10} {'stmts/s':>9}")
    for k in ks:
        stmt = stmt_for(tables, k)
        rows = None
        lat = []
        for _ in range(args.ops):
            t0 = time.perf_counter()
            reply = client(stmt)
            lat.append(time.perf_counter() - t0)
            if reply.startswith("ERR"):
                print(f"!! {reply}", file=sys.stderr)
                sys.exit(3)
            if rows is None:
                rows = s3.row_count(reply)
        p50 = pct(lat, 0.50)
        print(f"{k:>4} {rows if rows is not None else -1:>6} "
              f"{pct(lat, 0.0):>10.1f} {pct(lat, 0.25):>10.1f} {p50:>10.1f} "
              f"{pct(lat, 0.95):>10.1f} {pct(lat, 0.99):>10.1f} {1e6 / p50:>9.1f}")

    # The plan and its counters, so a reader can tell a probed statement
    # from a walked one without inferring it from the timings.
    print("# analyze:", " ".join(client(f"ANALYZE {stmt_for(tables, ks[-1])}").split())[:400])
    client.close()


if __name__ == "__main__":
    main()
