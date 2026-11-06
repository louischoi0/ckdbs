#!/usr/bin/env python3
"""The wait-event panel in text (CJ's WE11): active sessions over time by
class, top events, top fingerprints, a fingerprint-to-event drill-down and the
cumulative waits.

Reads `sys.ash` and `sys.wait_stats` over KWP and nothing else; the queries
are `ash_panel.sql`, which the engine's tests also run. KDS Studio is not in
this tree, so this is the panel's data contract and a reference rendering: a
Studio view runs the same three statements.

    ash_panel.py [--host H] [--port P] [--since-seconds N] [--bucket-ms B] [--fingerprint F]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402


def load_queries(path=None):
    """{name: sql} from `ash_panel.sql`'s `-- name:` blocks."""
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "ash_panel.sql")
    out, name, sql = {}, None, []
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("-- name: "):
            name, sql = line[9:], []
        elif line.startswith("--") or name is None:
            continue
        else:
            sql.append(line)
            if line.endswith(";"):
                out[name] = " ".join(sql)[:-1]
                name = None
    return out


def run(conn, sql, **params):
    for key, value in params.items():
        sql = sql.replace(":" + key, str(value))
    fields, rows, _tag, _n = conn.execute(sql)
    return [[kwp.render_value(f, raw) for f, raw in zip(fields, row)] for row in rows]


def tally(samples, fingerprint=None, bucket_us=1_000_000):
    """The panel's tallies from `samples` rows (ts, class, event,
    fingerprint): per time bucket and class, per (event, class), per
    fingerprint and - for `fingerprint` - its (event, class) split. Each core
    samples at its own instant, so one timestamp holds one core's sessions;
    a bucket of one sampling interval holds every core's."""
    by_point, by_event, by_fp, drill = {}, {}, {}, {}
    for ts, cls, event, fp in samples:
        point = int(ts) // bucket_us * bucket_us
        by_point.setdefault(point, {}).setdefault(cls, 0)
        by_point[point][cls] += 1
        by_event[(event, cls)] = by_event.get((event, cls), 0) + 1
        if int(fp) != 0:
            by_fp[int(fp)] = by_fp.get(int(fp), 0) + 1
        if fingerprint is not None and int(fp) == fingerprint:
            drill[(event, cls)] = drill.get((event, cls), 0) + 1
    return by_point, by_event, by_fp, drill


def ranked(counts, limit=10):
    return sorted(counts.items(), key=lambda kv: (-kv[1], str(kv[0])))[:limit]


def stacked(by_point, width=60):
    """Active sessions over time: one line per grid point, a letter per class."""
    classes = sorted({c for p in by_point.values() for c in p})
    # A distinct letter per class: CPU, Commit, CrossCore and Concurrency share
    # their first one.
    letters = {}
    for c in classes:
        pool = [ch.upper() for ch in c if ch.isalnum()] + list("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
        letters[c] = next((ch for ch in pool if ch not in letters.values()), "?")
    out = ["legend: " + ", ".join("%s=%s" % (letters[c], c) for c in classes)]
    peak = max((sum(p.values()) for p in by_point.values()), default=1)
    for ts in sorted(by_point):
        bar = "".join(letters[c] * max(1, round(n * width / peak)) for c, n in sorted(by_point[ts].items()))
        out.append("%12d us | %s %d" % (ts, bar, sum(by_point[ts].values())))
    return out


def table(title, header, rows):
    out = ["", title, "  " + "  ".join(header)]
    out += ["  " + "  ".join(str(c) for c in r) for r in rows] or ["  (none)"]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15432)
    ap.add_argument("--since-seconds", type=float, default=300)
    ap.add_argument("--bucket-ms", type=int, default=1000,
                    help="time bucket; the sampling interval (ash_interval_ms) makes it active sessions")
    ap.add_argument("--fingerprint", type=int, help="drill into one fingerprint (decimal)")
    args = ap.parse_args(argv)
    q = load_queries()
    conn = kwp.Connection(args.host, args.port, client_name="ash-panel")
    newest = run(conn, q["newest_sample"])
    if not newest:
        print("sys.ash holds no samples (sampling off, or nothing active)")
        return 0
    since = max(0, int(newest[0][0]) - int(args.since_seconds * 1e6))
    samples = run(conn, q["samples"], since_us=since)
    bucket_us = max(1, args.bucket_ms * 1000)
    by_point, by_event, by_fp, _ = tally(samples, bucket_us=bucket_us)
    target = args.fingerprint if args.fingerprint is not None else (ranked(by_fp, 1)[0][0] if by_fp else None)
    _, _, _, drill = tally(samples, target, bucket_us)
    lines = ["active sessions over time, by class"] + stacked(by_point)
    lines += table("top events (samples)", ["event", "class", "samples"],
                   [(e, c, n) for (e, c), n in ranked(by_event)])
    lines += table("top fingerprints (samples)", ["fingerprint", "samples"], ranked(by_fp))
    if target is not None:
        lines += table("fingerprint %d by event" % target, ["event", "class", "samples"],
                       [(e, c, n) for (e, c), n in ranked(drill)])
    lines += table("cumulative waits, all cores", ["event", "class", "count", "total_us", "max_us"],
                   run(conn, q["cumulative_waits"]))
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
