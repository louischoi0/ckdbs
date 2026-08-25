#!/usr/bin/env python3
"""Timing and reporting harness shared by the ckdbs and PostgreSQL benchmarks.

Two drivers use it:

    benchmark.py      ckdbs, over its newline text protocol
    pg_benchmark.py   PostgreSQL, over the v3 wire protocol

They run the same four phases (insert / point-select / full-scan / update)
against the same 5-column row shape, print the same table and write the same
JSON keys, so two runs are directly diffable. Everything here is
backend-agnostic on purpose: a backend contributes only a callable that takes
one already-formatted command string and returns one reply string, where a
reply beginning with `ERR` counts as an error.

What the numbers include is identical on both sides: a per-request wall clock
around one send + one receive from a single connection, so every latency
carries Python's own socket cost. That cost is the floor this harness can
resolve - treat sub-10us differences as noise, and read the two engines'
absolute numbers as "what a client pays", not "what the engine costs".
"""

import json
import statistics
import time

# Name column width: wide enough for the PostgreSQL driver's variant-tagged
# phase names ("point-select[noidx]"), and shared so both tools' tables line
# up when pasted next to each other.
NAME_WIDTH = 22


def nearest_rank(ordered, p):
    """The nearest-rank percentile of an ascending sequence: ceil(p/100 * n) - 1,
    clamped, so p=50 on 4 samples picks index 1. One home for the rule -
    Phase.percentile and the drivers' own tables both read it here."""
    rank = max(1, -(-int(p) * len(ordered) // 100)) - 1
    return ordered[rank]


class Phase:
    """One timed phase: its per-request latencies in seconds."""

    def __init__(self, name, detail=""):
        self.name = name
        self.detail = detail
        self.latencies = []
        self.trace = None
        self.errors = 0
        self.first_error = None
        self.elapsed = 0.0

    def record(self, latency, reply):
        # The arrival time as well as the duration, when a caller asked for
        # the trace. Percentiles say how bad the tail is and never *when* -
        # and "when" is what separates a periodic stall (maintenance, a
        # timer, the kernel) from a proportional one (every Nth row grows a
        # page). Off unless enabled: two floats per statement is memory a
        # long run does not need to spend.
        if self.trace is not None:
            self.trace.append((time.time(), latency))
        self.latencies.append(latency)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = reply

    @property
    def ops(self):
        return len(self.latencies)

    @property
    def qps(self):
        return self.ops / self.elapsed if self.elapsed > 0 else 0.0

    def percentile(self, p):
        """Nearest-rank percentile over the sorted latencies, in seconds."""
        if not self.latencies:
            return 0.0
        return nearest_rank(sorted(self.latencies), p)

    def summary(self):
        return {
            "phase": self.name,
            "detail": self.detail,
            "ops": self.ops,
            "errors": self.errors,
            "first_error": self.first_error,
            "elapsed_s": round(self.elapsed, 6),
            "qps": round(self.qps, 1),
            "mean_us": round(statistics.fmean(self.latencies) * 1e6, 1) if self.latencies else 0.0,
            # p0 and p25 are here because a benchmark document is required to
            # carry them (bench/docs/README.md, rule 6): p0 is the best case
            # the path can reach, which is what says how much of the mean is
            # fixed cost, and p25 says whether the body of the distribution is
            # tight or long. p50/p99 alone hide both.
            "p0_us": round(min(self.latencies) * 1e6, 1) if self.latencies else 0.0,
            "p25_us": round(self.percentile(25) * 1e6, 1),
            "p50_us": round(self.percentile(50) * 1e6, 1),
            "p95_us": round(self.percentile(95) * 1e6, 1),
            "p99_us": round(self.percentile(99) * 1e6, 1),
            "max_us": round(max(self.latencies) * 1e6, 1) if self.latencies else 0.0,
        }


def run_phase(execute, name, commands, detail=""):
    """Sends every command in `commands` back to back, timing each one.

    `execute` is the backend's one-command-one-reply callable. `commands` is
    an iterable of already-formatted command strings; building them is kept
    out of the timed region so string formatting is not counted as server
    time - which is why callers pass a generator, not a list.
    """
    phase = Phase(name, detail)
    started = time.perf_counter()
    for command in commands:
        t0 = time.perf_counter()
        reply = execute(command)
        phase.record(time.perf_counter() - t0, reply)
    phase.elapsed = time.perf_counter() - started
    return phase


def report(phases, meta, footer=()):
    """Prints the standard table. `meta` needs engine/columns/rows/host/port/
    table; `footer` is the backend's own caveats, one line each.

    `meta['clustered']` is optional and ckdbs-only: which storage the
    relation uses (heap chain vs clustered B+ tree). Printed on the header
    line when present because it changes what the point-select and update
    phases do, so a table without it beside them is unreadable.

    `meta['connections']` is optional and defaults to 1, which is what the
    two single-connection drivers want. scenario0_stockmarket.py drives several
    processes at once, and a header claiming one connection over an
    aggregate throughput number would misread by exactly that factor.
    """
    print()
    storage = f", {meta['clustered']}-clustered" if meta.get("clustered") else ""
    connections = meta.get("connections", 1)
    print(f"{meta['engine']} benchmark - {meta['columns']} columns, "
          f"{meta['rows']} rows resident{storage}, "
          f"{connections} connection{'' if connections == 1 else 's'}")
    print(f"server {meta['host']}:{meta['port']}  table {meta['table']}")
    print()
    header = (f"{'phase':<{NAME_WIDTH}}{'ops':>8}{'qps':>10}{'mean us':>10}"
              f"{'p0':>9}{'p25':>9}{'p50':>9}{'p95':>9}{'p99':>9}{'max':>10}{'err':>6}")
    print(header)
    print("-" * len(header))
    for phase in phases:
        s = phase.summary()
        print(f"{s['phase']:<{NAME_WIDTH}}{s['ops']:>8}{s['qps']:>10,.0f}{s['mean_us']:>10.1f}"
              f"{s['p0_us']:>9.1f}{s['p25_us']:>9.1f}{s['p50_us']:>9.1f}"
              f"{s['p95_us']:>9.1f}{s['p99_us']:>9.1f}{s['max_us']:>10.1f}"
              f"{s['errors']:>6}")
    print()
    for phase in phases:
        if phase.detail:
            print(f"  {phase.name}: {phase.detail}")
        if phase.errors:
            print(f"  {phase.name}: {phase.errors} error replies, first: {phase.first_error}")
    print()
    for line in footer:
        print(f"  {line}")


def write_json(path, meta, phases):
    with open(path, "w") as f:
        json.dump({"meta": meta, "phases": [p.summary() for p in phases]}, f, indent=2)
    print(f"  wrote {path}")
