#!/usr/bin/env python3
"""What a fold costs (docs/spec/aggregate.md, workplan AG10).

The measurement this bench exists to make is a *difference*, not a
throughput. AG1 places aggregation outside the executor and compiles the
same chain either way, so the only question the numbers can answer is: what
does folding a row cost on top of producing it? Every scenario therefore
runs an aggregated statement against its **unaggregated twin** - the same
chain, the same rows, the same access kind - and reports the gap.

Two scenarios, which are the two shapes a fold meets:

    grouped-scan    a walk that produces many rows and collapses them to a
                    few. The fold pays per input row and the client pays
                    for almost nothing coming back, which is where a fold
                    is supposed to win outright: the unaggregated twin has
                    to serialise every row onto the wire.
    grouped-probe   a keyed chain that produces a handful of rows. The fold
                    pays for the group machinery on a statement that had
                    little to fold, which is where its overhead shows
                    undiluted - and the case AG11's defaults must not make
                    worse.

Also swept: the group **cardinality**, because that is what decides whether
the fold's map is one bucket or many, and it is the input the AG11 caps are
about.

Run against a server whose data file is on a **block device**. The Cabin
bench learned this the expensive way (bench/results-cabin.md): on tmpfs
fsync is free, the scan becomes the bottleneck, and a feature that touches
scans measures as a much larger win than it is. Results recorded from a
tmpfs run are not results.
"""

import argparse
import socket
import statistics
import sys
import time

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 15432


class Conn:
    """One connection, one line in / one line out."""

    def __init__(self, host, port, timeout):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._buf = b""

    def send(self, line):
        self._sock.sendall(line.encode("utf-8") + b"\n")
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self._buf += chunk
        out, self._buf = self._buf.split(b"\n", 1)
        return out.decode("utf-8", errors="replace")

    def must(self, line):
        reply = self.send(line)
        if reply.startswith("ERR"):
            raise RuntimeError("%s -> %s" % (line, reply))
        return reply

    def close(self):
        self._sock.close()


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    at = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[at]


class Result:
    def __init__(self, name, latencies, rows_back):
        self.name = name
        self.latencies = latencies
        self.rows_back = rows_back

    @property
    def mean(self):
        return statistics.fmean(self.latencies) if self.latencies else 0.0

    @property
    def p50(self):
        return percentile(self.latencies, 0.50)

    @property
    def p99(self):
        return percentile(self.latencies, 0.99)


def time_statement(conn, sql, reps):
    """Runs `sql` `reps` times and returns the per-execution latencies.

    One warm-up execution first, discarded: the first run of a statement
    pays for a parse into a cold catalog cache and, if recording is on, is
    the execution that only *counts* (recording is n=2). Neither is what
    this bench is about.
    """
    conn.must(sql)
    latencies = []
    rows_back = 0
    for _ in range(reps):
        start = time.perf_counter()
        reply = conn.send(sql)
        latencies.append(time.perf_counter() - start)
        if reply.startswith("ERR"):
            raise RuntimeError("%s -> %s" % (sql, reply))
        rows_back = reply.count("\\n")
    return Result(sql, latencies, rows_back)


def load(conn, table, rows, cardinality, clustered, suffix):
    """Builds one relation of `rows` rows over `cardinality` distinct keys."""
    name = "%s%s" % (table, suffix)
    conn.send("DROP TABLE %s" % name)  # may not exist; the reply is ignored
    conn.must("CREATE TABLE %s (id int64, bucket int64, qty int64, sym varchar) %s"
              % (name, clustered))
    for i in range(rows):
        conn.must("INSERT INTO %s VALUES (%d, %d, 's%d')"
                  % (name, i % cardinality, (i * 7) % 1000, i % 16))
    return name


def report(title, pairs):
    print()
    print(title)
    print("  %-46s %9s %9s %9s %7s" % ("statement", "mean_us", "p50_us", "p99_us", "rows"))
    for folded, plain in pairs:
        for r in (plain, folded):
            print("  %-46s %9.1f %9.1f %9.1f %7d"
                  % (r.name[:46], r.mean * 1e6, r.p50 * 1e6, r.p99 * 1e6, r.rows_back))
        delta = (folded.mean / plain.mean - 1.0) * 100.0 if plain.mean else 0.0
        print("  %-46s %9s %9s %9s %7s" % ("  -> fold vs plain: %+.1f%% mean" % delta,
                                           "", "", "", ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--suffix", default="",
                        help="appended to every table name, so repeated runs "
                             "against one data file do not collide")
    parser.add_argument("--rows", type=int, default=20000,
                        help="rows in the scanned relation (default: 20000)")
    parser.add_argument("--reps", type=int, default=30,
                        help="timed executions per statement (default: 30)")
    parser.add_argument("--cardinalities", default="2,64,1024",
                        help="group counts to sweep in the grouped-scan "
                             "scenario (default: 2,64,1024)")
    parser.add_argument("--skip-load", action="store_true",
                        help="reuse the relations a previous run built")
    args = parser.parse_args()

    conn = Conn(args.host, args.port, args.timeout)
    cardinalities = [int(c) for c in args.cardinalities.split(",") if c]

    try:
        # ---- Scenario 1: grouped scan -------------------------------------
        #
        # One relation per cardinality, because the group count is the
        # variable and the row count must not move with it.
        scan_pairs = []
        for card in cardinalities:
            table = "aggscan%d" % card
            name = ("%s%s" % (table, args.suffix)) if args.skip_load else load(
                conn, table, args.rows, card, "HEAP", args.suffix)
            folded = time_statement(
                conn, "SELECT bucket, COUNT(*), SUM(qty) FROM %s GROUP BY bucket" % name,
                args.reps)
            plain = time_statement(conn, "SELECT bucket, qty FROM %s" % name, args.reps)
            scan_pairs.append((folded, plain))
        report("grouped-scan  (%d rows, one relation per group count)" % args.rows, scan_pairs)

        # The global form: the same walk with no key encoded and no map
        # probed, which is what isolates the map's share of the fold.
        name = "aggscan%d%s" % (cardinalities[-1], args.suffix)
        global_pairs = [(
            time_statement(conn, "SELECT COUNT(*), SUM(qty) FROM %s" % name, args.reps),
            time_statement(conn, "SELECT qty FROM %s" % name, args.reps),
        )]
        report("global fold   (no GROUP BY: no key, no hash, no map)", global_pairs)

        # DISTINCT over the same walk, which adds a set insert per new value
        # per group - the one part of the fold that allocates.
        distinct_pairs = [(
            time_statement(conn, "SELECT COUNT(DISTINCT sym) FROM %s" % name, args.reps),
            time_statement(conn, "SELECT COUNT(sym) FROM %s" % name, args.reps),
        )]
        report("distinct      (16 distinct values over the same walk)", distinct_pairs)

        # ---- Scenario 2: grouped probe ------------------------------------
        #
        # A btree relation and a pk equality, so the chain descends instead
        # of walking and the fold's overhead is undiluted by scan cost.
        probe = ("aggprobe%s" % args.suffix) if args.skip_load else load(
            conn, "aggprobe", min(args.rows, 5000), 64, "BTREE", args.suffix)
        probe_pairs = [
            (time_statement(conn, "SELECT COUNT(*) FROM %s WHERE id = 42" % probe, args.reps),
             time_statement(conn, "SELECT qty FROM %s WHERE id = 42" % probe, args.reps)),
            (time_statement(conn,
                            "SELECT bucket, COUNT(*) FROM %s WHERE id BETWEEN 100 AND 200 "
                            "GROUP BY bucket" % probe, args.reps),
             time_statement(conn,
                            "SELECT bucket, qty FROM %s WHERE id BETWEEN 100 AND 200" % probe,
                            args.reps)),
        ]
        report("grouped-probe (btree, keyed chain)", probe_pairs)

        print()
        print("note: latencies include the Python client's own socket cost, which is the "
              "floor this harness can resolve.")
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
