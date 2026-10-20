#!/usr/bin/env python3
"""Time to first byte and to the first S_ROW_BATCH of a KWP/1 statement (work order CE).

One statement, sent as PARSE + BIND + EXECUTE + CLOSE + SYNC in one write (the benchmark path of
`kwp.Connection.query`), then the reply read raw. The clock starts immediately before `sendall` and
each event is stamped with the time of the `recv` that completed it:

    first_byte   the first recv that returned data (any frame)
    row_desc     the recv that completed S_ROW_DESC
    first_batch  the recv that completed the first S_ROW_BATCH
    last_batch   the recv that completed the last S_ROW_BATCH
    complete     the recv that completed S_COMPLETE (or S_ERROR)
    ready        the recv that completed S_READY (the reply is over)

A reply that leaves in one write (a buffered reply: today's engine) has every event at the same
instant bar the client's own parse time; a streamed reply has first_batch well below ready. The
difference `ready - first_batch` is how much of the answer the client could already hold.

No cell is decoded and no row is counted by value: only the frame headers and the S_ROW_BATCH row
count (its first two payload bytes) are read, so the probe adds almost nothing to the clock.

  ttfb_probe.py --port P [--host H] [--reps N] [--max-seconds S] [--warmup W] [--json OUT]
                (--sql LABEL=STATEMENT ...)
                [--daily-stats-sizes 1,100,1000,10000,60480] [--daily-stats-filter 3]

  --sql LABEL=STATEMENT    a statement to time, repeatable
  --daily-stats-sizes L    shorthand for `SELECT * FROM daily_stats WHERE id BETWEEN 1 AND n` per n
                           (the row-cost sweep's table, `tools`-side loader in the CE-S0 archive)
  --daily-stats-filter K   shorthand for `SELECT * FROM daily_stats WHERE symbol_id = K`
  --reps N                 timed repetitions per statement, default 50 (stops early at --max-seconds,
                           never below 10)
  --max-seconds S          time budget per statement, default 8
  --warmup W               untimed executions before the timed ones, default 3
  --json OUT               write every repetition's events (microseconds from send) to OUT

Pin the process when comparing runs (`taskset -c 6 python3 ...`) and run it against a quiet host.
Prints one table: per statement, rows, batches, bytes, and p0 p25 p50 p95 p99 of each event in
microseconds. The close of CE re-runs this probe unchanged against the streaming build.
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402

_HDR = struct.Struct("<IBBH")
_U16 = struct.Struct("<H")
BUF = 8 << 20          # one frame is at most 64 KiB of batch plus a header; kMaxFrame is 16 MiB


def pct(xs, p):
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


class Probe:
    def __init__(self, host, port):
        self.conn = kwp.Connection(host, port, timeout=300)
        self.sock = self.conn.sock
        self.buf = bytearray(BUF)
        self.mv = memoryview(self.buf)

    def once(self, sql):
        """One statement. Returns a dict of events in microseconds from send, plus shape counters."""
        sock, buf, mv = self.sock, self.buf, self.mv
        msg = kwp.frame(kwp.C_PARSE, kwp._Writer().s("").text(sql).take()) + kwp._QUERY_TAIL
        ev = {}
        rows = batches = total = recvs = 0
        first_types = []
        have = p = 0
        pc = time.perf_counter_ns
        t0 = pc()
        sock.sendall(msg)
        while True:
            if p:
                buf[0:have - p] = buf[p:have]
                have -= p
                p = 0
            n = sock.recv_into(mv[have:])
            now = pc()
            if not n:
                raise ConnectionError("server closed the connection")
            recvs += 1
            total += n
            have += n
            if "first_byte" not in ev:
                ev["first_byte"] = now - t0
            while have - p >= 8:
                (length, ftype, _, _) = _HDR.unpack_from(buf, p)
                size = 4 + length
                if size > BUF:
                    raise RuntimeError("frame larger than the probe's buffer: %d" % size)
                if have - p < size:
                    break
                if len(first_types) < 6:
                    first_types.append(ftype)
                if ftype == kwp.S_ROW_DESC:
                    ev.setdefault("row_desc", now - t0)
                elif ftype == kwp.S_ROW_BATCH:
                    ev.setdefault("first_batch", now - t0)
                    ev["last_batch"] = now - t0
                    batches += 1
                    rows += _U16.unpack_from(buf, p + 8)[0]
                elif ftype in (kwp.S_COMPLETE, kwp.S_ERROR):
                    ev.setdefault("complete", now - t0)
                    if ftype == kwp.S_ERROR:
                        ev["error"] = 1
                elif ftype == kwp.S_READY:
                    ev["ready"] = now - t0
                    out = {k: v / 1000.0 for k, v in ev.items() if k != "error"}
                    out.update(rows=rows, batches=batches, bytes=total, recvs=recvs,
                               first_types=first_types, error=ev.get("error", 0))
                    return out
                p += size
            if p == have:
                p = have = 0


EVENTS = ("first_byte", "row_desc", "first_batch", "last_batch", "complete", "ready")


def run_one(probe, label, sql, reps, max_seconds, warmup):
    for _ in range(warmup):
        probe.once(sql)
    out = []
    t_end = time.perf_counter() + max_seconds
    while len(out) < reps and (len(out) < 10 or time.perf_counter() < t_end):
        r = probe.once(sql)
        if r["error"]:
            raise RuntimeError("statement failed: " + label)
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--sql", action="append", default=[], metavar="LABEL=STATEMENT")
    ap.add_argument("--daily-stats-sizes", default="")
    ap.add_argument("--daily-stats-filter", type=int, default=0)
    ap.add_argument("--reps", type=int, default=50)
    ap.add_argument("--max-seconds", type=float, default=8.0)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--json")
    a = ap.parse_args()
    shapes = []
    for s in a.sql:
        label, _, sql = s.partition("=")
        shapes.append((label, sql))
    for n in [int(x) for x in a.daily_stats_sizes.split(",") if x]:
        shapes.append((f"range-{n}", f"SELECT * FROM daily_stats WHERE id BETWEEN 1 AND {n}"))
    if a.daily_stats_filter:
        shapes.append((f"filter-symbol-{a.daily_stats_filter}",
                       f"SELECT * FROM daily_stats WHERE symbol_id = {a.daily_stats_filter}"))
    if not shapes:
        ap.error("no statement given")
    probe = Probe(a.host, a.port)
    allres = {}
    print(f"{'statement':22s} {'reps':>4s} {'rows':>6s} {'batch':>5s} {'bytes':>9s} {'recv':>5s} | event      "
          f"{'p0':>10s} {'p25':>10s} {'p50':>10s} {'p95':>10s} {'p99':>10s}  (us from send)")
    for label, sql in shapes:
        res = run_one(probe, label, sql, a.reps, a.max_seconds, a.warmup)
        allres[label] = {"sql": sql, "reps": res}
        r0 = res[len(res) // 2]
        for i, e in enumerate(EVENTS):
            xs = [r[e] for r in res]
            head = (f"{label:22s} {len(res):4d} {r0['rows']:6d} {r0['batches']:5d} {r0['bytes']:9d} {r0['recvs']:5d}"
                    if i == 0 else " " * 56)
            print(f"{head} | {e:10s} " + " ".join(f"{pct(xs, p):10.1f}" for p in (0, 25, 50, 95, 99)))
        sys.stdout.flush()
    if a.json:
        with open(a.json, "w") as f:
            json.dump(allres, f)


if __name__ == "__main__":
    main()
