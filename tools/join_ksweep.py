#!/usr/bin/env python3
"""The walked join's k-sweep: one statement shape, k as the parameter.

`scenario3_library.py` fixes `JOIN_OUTER_K` at 16 on purpose — its phase
model is ops-of-one-statement — and says in the same comment that the
k-sweep is a harness's job. This is that harness, and it exists because
the statement-local inner build's whole economics is a function of k: the
build is paid once per statement and amortized over the outer rows, so
k = 1 pays it with no payback and k = 16 pays it once for sixteen walks
(`docs/workplan-join-inner-build.md`, "The build constant").

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
    print(f"# label={args.label} loans={sizes['loans']} users={sizes['users']} "
          f"ops={args.ops}")
    print(f"{'k':>4} {'rows':>6} {'p0':>10} {'p25':>10} {'p50':>10} "
          f"{'p90':>10} {'stmts/s':>9}")
    for k in ks:
        stmt = join_stmt(tables, k)
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
              f"{pct(lat, 0.90):>10.1f} {1e6 / p50:>9.1f}")

    # The plan and its counters, so a reader can tell a probed statement
    # from a walked one without inferring it from the timings.
    print("# analyze:", " ".join(client(f"ANALYZE {join_stmt(tables, ks[-1])}").split())[:400])
    client.close()


if __name__ == "__main__":
    main()
