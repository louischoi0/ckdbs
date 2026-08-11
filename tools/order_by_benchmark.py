#!/usr/bin/env python3
"""The output sort, priced: what ORDER BY costs, and what it costs when elided.

`docs/workplan-order-by.md` OB4/OB5. Three questions, answered against one
server on one data file within seconds of each other, because they are only
comparable that way:

  1. **What does the feature cost a statement that does not use it?** The
     dispatcher's row rendering was refactored into one shared `render`
     lambda that appends into a reused `std::string` and then streams it,
     replacing N per-value `os << ...` insertions. That is the only change on
     the unsorted hot path, so the `plain` arm below run against a binary
     built from the base commit and against one built from the feature is the
     whole before/after. `star` is the same statement written `SELECT *`,
     which renders from the relation's schema and therefore needs a
     `TableAccess` the projection form does not; the feature resolves it once
     per statement where the base resolved it once per row, so the gap
     between `star`'s delta and `plain`'s is what that hoist is worth.
  2. **Is `ORDER BY <pk> ASC` really free?** The compiler elides it - one
     ascending key on the driving relation's pk names the order the chain
     already emits, so `chain.sort_keys` is cleared and no sort is built. The
     `pk-order` arm must therefore be indistinguishable from `plain`. This
     driver does not take that on trust: it reads `sorted=` out of `ANALYZE`
     and refuses to label an arm elided when the server reports a sort.
  3. **What does a real sort cost?** `nonpk` orders by a random int64 column,
     `nonpk-str` by a varchar one (a different normalized-key shape), and
     `nonpk-lim` adds `LIMIT 20`, which turns the buffer into a top-N heap
     holding `offset + limit` rows. `plain-lim` is its unsorted twin - and
     the honest comparison for it, because an unsorted `LIMIT 20` *stops the
     walk*, where a sorted one cannot.

---- What makes the comparison fair ---------------------------------------

**One relation, one data file, one server.** Every arm reads the same rows.
Nothing is created or dropped between arms.

**Interleaved in blocks, not run in sequence.** A round runs one block of
every arm before the next round starts any arm's second block, so a machine
that gets busier partway through costs every arm equally instead of inventing
a result. The block, rather than the single operation, is the interleaving
unit because server CPU is sampled around it and `/proc/<pid>/stat` resolves
one jiffy.

**Equal work, not equal time.** Every arm gets the same fixed operation count
at a given row-set size.

**The noise floor comes from inside the run.** `plain` is measured twice -
once as `plain`, once as `plain-again` - and the two are the same
configuration by construction. Nothing smaller than that gap is a finding.
`pk-point` is a second control: a single-row clustered descent that no sort
can touch, so it must not move across arms or across binaries.

**Server CPU beside client latency.** `--server-pid` accumulates
utime+stime across each arm's blocks and divides by the operation count.
Client latency on this harness carries Python's socket cost, which for a
10,000-row reply is most of the number; the CPU column is what the engine
actually spent.

---- Row-set sizes --------------------------------------------------------

`--rows` is the axis: **200 / 1000 / 10000**, one server and one fresh data
file per size (catalog rows are never reclaimed and undo never purges, so a
second size on one file is not a repeat of the first). A sort is O(n log n)
in the rows it holds and the render change is O(n) in the values emitted, so
one cardinality could not separate them.

Flags and exact invocations: `bench/docs/README.md`.
"""

import argparse
import datetime
import json
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bench_common import Phase, report, write_json
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection

COLUMNS = "id int64, val int64, grp int32, amount int64, tag varchar"
CLUSTERED = "BTREE"
PROJECTION = "id, val, grp, amount, tag"

ROW_COUNT = re.compile(r"\brows=(\d+)")
SORTED = re.compile(r"\bsorted=(\d+)")
EXAMINED = re.compile(r"\bexamined=(\d+)")
PAGES = re.compile(r"\bpages=(\d+)")


def abort(message, reply=None):
    print(f"order_by_benchmark aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def server_cpu_seconds(pid):
    """utime+stime of the server process, in seconds (SC_CLK_TCK jiffies)."""
    with open(f"/proc/{pid}/stat") as f:
        after_comm = f.read().rsplit(") ", 1)[1].split()
    return (int(after_comm[11]) + int(after_comm[12])) / os.sysconf("SC_CLK_TCK")


class Client:
    def __init__(self, host, port, timeout):
        self._conn = ServerConnection(host, port, timeout=timeout)

    def __call__(self, command):
        return self._conn.send_command(command)

    def timed(self, command, phase):
        t0 = time.perf_counter()
        reply = self._conn.send_command(command)
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def close(self):
        self._conn.close()


def make_rows(rows, seed):
    """The row list. `val` is a random int64 in a domain as large as the
    relation, so the sort key is neither already-ordered nor tie-heavy - an
    ascending `val` would let the comparator win on its first branch every
    time and price a sort nobody runs."""
    rng = random.Random(seed)
    out = []
    for i in range(rows):
        out.append((
            rng.randrange(rows * 10),        # val: the int64 sort key
            rng.randrange(16),               # grp
            rng.randrange(1_000_000),        # amount
            f"K{rng.randrange(rows * 10):09d}",  # tag: the varchar sort key
        ))
    return out


def insert_values(row):
    val, grp, amount, tag = row
    return f"{val}, {grp}, {amount}, '{tag}'"


def load(client, rel, rows, phase, batch):
    if batch > 1:
        client("BEGIN")
    pending = 0
    for row in rows:
        reply = client.timed(f"INSERT INTO {rel} VALUES ({insert_values(row)})", phase)
        if reply.startswith("ERR"):
            abort(f"load failed at row {pending}", reply)
        pending += 1
        if batch > 1 and pending % batch == 0:
            client("COMMIT")
            client("BEGIN")
    if batch > 1:
        client("COMMIT")


# ---- the arms -------------------------------------------------------------
#
# (name, detail, statement builder). The builder takes the relation name and
# a per-operation integer so a point lookup can vary its argument; the scan
# arms ignore it, because a scan has no argument to vary and re-running one
# statement is what "the same work every operation" means for them.

def arms(limit_rows, sorted_arms=True):
    """The arm list: `(name, detail, build(rel, k))`.

    The builder takes the relation name rather than closing over it, because
    the two sides of a before/after run are two servers with two relations
    and must be driven by one arm list - a second list is a second place for
    the two sides to differ.

    `sorted_arms=False` is `--base`: a server built before the output sort
    refuses every non-pk `ORDER BY` with `Unsupported`, so those arms are not
    dropped for tidiness - they cannot be run at all. What remains is exactly
    the set both binaries can answer, which is what the before/after
    comparison is allowed to use.
    """
    def sel(rel):
        return f"SELECT {PROJECTION} FROM {rel}"

    out = [
        ("ping", "PING - client and socket floor, no engine work",
         lambda rel, k: "PING"),
        ("plain", "no ORDER BY - the unsorted full scan",
         lambda rel, k: sel(rel)),
        # `star` is `plain`'s twin in every way but one: the dispatcher
        # renders it from the relation's schema rather than from the chain's
        # projection types, which means it needs a `catalog_.InitTableAccess`.
        # Whether that lookup is per row or per statement is invisible to the
        # client and worth tens of nanoseconds a row, so the two arms are run
        # side by side and the difference between their before/after deltas
        # is the whole of what hoisting it buys.
        ("star", "SELECT * - the same five columns, rendered from the schema",
         lambda rel, k: f"SELECT * FROM {rel}"),
        ("pk-order", "ORDER BY id ASC - elided at compile, must equal `plain`",
         lambda rel, k: f"{sel(rel)} ORDER BY id ASC"),
    ]
    if sorted_arms:
        out += [
            ("nonpk", "ORDER BY val - a real sort over every row",
             lambda rel, k: f"{sel(rel)} ORDER BY val"),
            ("nonpk-desc", "ORDER BY val DESC - the same sort, descending",
             lambda rel, k: f"{sel(rel)} ORDER BY val DESC"),
            ("nonpk-str", "ORDER BY tag - a varchar key, a different normalization",
             lambda rel, k: f"{sel(rel)} ORDER BY tag"),
            ("nonpk-lim", f"ORDER BY val LIMIT {limit_rows} - the top-N heap",
             lambda rel, k: f"{sel(rel)} ORDER BY val LIMIT {limit_rows}"),
        ]
    # ---- the ANALYZE arms: the same read path with the render and the
    # reply removed -------------------------------------------------------
    #
    # `RunAnalyze` compiles the same chain, executes the same steps through
    # the same sink, arms the same sorter and drains it through the same
    # quota - and then answers with one line of plan text instead of the
    # rows. It is therefore the *only* way on this harness to price the walk
    # apart from the formatting: `plain` minus `an-plain` is what rendering
    # and shipping the rows cost, and `an-nonpk-lim` minus `an-plain` is what
    # normalizing a sort key and testing it against the heap cost, neither of
    # which is separable from the other by timing whole statements.
    #
    # It is an instrument, not a workload: nobody's OLTP traffic is ANALYZE.
    # The one respect in which it is not the real run is the one it is used
    # for - it renders nothing - and that is stated wherever it is quoted.
    out += [
        ("an-plain", "ANALYZE, no ORDER BY - the walk with no render, no reply",
         lambda rel, k: f"ANALYZE {sel(rel)}"),
    ]
    if sorted_arms:
        out += [
            ("an-nonpk", "ANALYZE ORDER BY val - walk + normalize + buffer + sort",
             lambda rel, k: f"ANALYZE {sel(rel)} ORDER BY val"),
            ("an-nonpk-lim", f"ANALYZE ORDER BY val LIMIT {limit_rows} - walk + "
                             "normalize + heap test, nothing rendered",
             lambda rel, k: f"ANALYZE {sel(rel)} ORDER BY val LIMIT {limit_rows}"),
        ]
    out += [
        ("an-plain-lim", f"ANALYZE LIMIT {limit_rows} - the ANALYZE floor: a "
                         "round trip and a walk that stops at 20",
         lambda rel, k: f"ANALYZE {sel(rel)} LIMIT {limit_rows}"),
        ("plain-lim", f"LIMIT {limit_rows}, no ORDER BY - the quota stops the walk",
         lambda rel, k: f"{sel(rel)} LIMIT {limit_rows}"),
        ("pk-point", "WHERE id = ? - the control, one clustered descent",
         lambda rel, k: f"{sel(rel)} WHERE id = {k}"),
        ("plain-again", "no ORDER BY, repeated - the in-run noise floor",
         lambda rel, k: sel(rel)),
    ]
    return out


def analyze_facts(client, rel, limit_rows, sorted_arms=True):
    """`ANALYZE` for every scan arm: what the server says it did, in its own
    words. This is what stops an arm being labelled elided because the driver
    assumed it: `sorted=` appears only when a sort ran."""
    out = {}
    for name, _, build in arms(limit_rows, sorted_arms):
        if name == "ping":
            continue
        stmt = build(rel, 1)
        # The `an-*` arms are already an ANALYZE of an arm listed above them;
        # prefixing a second one is a syntax error, and the facts would be
        # a duplicate of the row they measure.
        if stmt.startswith("ANALYZE "):
            continue
        reply = client(f"ANALYZE {stmt}")
        if reply.startswith("ERR"):
            abort(f"ANALYZE failed for arm `{name}`", reply)
        srt = SORTED.search(reply)
        ex = EXAMINED.search(reply)
        pg = PAGES.search(reply)
        rc = ROW_COUNT.search(reply)
        out[name] = {
            "sorted": int(srt.group(1)) if srt else None,
            "examined": int(ex.group(1)) if ex else None,
            "pages": int(pg.group(1)) if pg else None,
            "rows": int(rc.group(1)) if rc else None,
            "reply": reply,
        }
    return out


def verify(client, rel, rows, limit_rows, sorted_arms=True):
    """Every arm's reply, checked for the property that arm claims.

    A throughput number over a statement that answered wrongly is a
    measurement of nothing, and an *order* is the one thing this feature
    exists to produce - so the check is on the order, not only the count.
    """
    base = f"SELECT {PROJECTION} FROM {rel}"

    def fetch(stmt):
        reply = client(stmt)
        if reply.startswith("ERR"):
            abort(f"verify: `{stmt}`", reply)
        body = reply.split("\\n")[1:]
        return [line.split(",") for line in body if line]

    plain = fetch(base)
    if len(plain) != rows:
        abort(f"verify: plain scan returned {len(plain)} rows, expected {rows}")

    pk_order = fetch(f"{base} ORDER BY id ASC")
    if pk_order != plain:
        abort("verify: ORDER BY id ASC did not equal the unsorted reply "
              "row for row - the elision changed the answer")

    # `SELECT *` names the same five columns in the same order, so it must
    # answer identically. The check exists because the dispatcher resolves
    # the star's `TableAccess` once per statement rather than per row: a
    # hoist that resolved the wrong relation, or held a pointer across a
    # catalog change, would show here and nowhere else.
    star = fetch(f"SELECT * FROM {rel}")
    if star != plain:
        abort("verify: SELECT * did not equal the explicit projection row "
              "for row - the hoisted TableAccess changed the answer")
    if not sorted_arms:
        return {"rows": rows, "checks": 3}

    by_val = fetch(f"{base} ORDER BY val")
    keys = [int(r[1]) for r in by_val]
    if keys != sorted(keys):
        abort("verify: ORDER BY val is not ascending")
    if sorted(tuple(r) for r in by_val) != sorted(tuple(r) for r in plain):
        abort("verify: ORDER BY val returned a different row multiset")

    desc = fetch(f"{base} ORDER BY val DESC")
    if [int(r[1]) for r in desc] != sorted(keys, reverse=True):
        abort("verify: ORDER BY val DESC is not descending")

    by_tag = fetch(f"{base} ORDER BY tag")
    tags = [r[4] for r in by_tag]
    if tags != sorted(tags):
        abort("verify: ORDER BY tag is not ascending")

    lim = fetch(f"{base} ORDER BY val LIMIT {limit_rows}")
    if len(lim) != min(limit_rows, rows):
        abort(f"verify: LIMIT {limit_rows} returned {len(lim)} rows")
    if lim != by_val[:limit_rows]:
        abort("verify: the top-N heap's rows are not the first rows of the "
              "full sorted order")
    return {"rows": rows, "checks": 8}


class Side:
    """One server under test: its connection, its relation, its CPU meter.

    Two of these is the before/after comparison. They are two *processes* on
    two data files, because two binaries cannot share one - so the only way
    to keep a busy machine from inventing a result is to interleave the
    blocks between them, which `run_pass` does.
    """

    def __init__(self, label, host, port, pid, timeout, has_sort):
        self.label = label
        self.port = port
        self.pid = pid
        self.has_sort = has_sort
        self.client = Client(host, port, timeout)
        self.rel = None
        self.facts = None
        self.verified = None


def run_pass(sides, spec, rounds, block, max_id):
    """Runs `spec`'s arms on every side, interleaved at block granularity.

    Round r runs, for each arm and then for each side, one block of `block`
    operations. Every side therefore reaches the same point in the run at the
    same time, and a machine that gets busier partway through costs them
    equally. Returns `{(arm, side_label): Phase}` and the CPU seconds each
    pair accumulated.
    """
    phases = {(n, s.label): Phase(f"{n}[{s.label}]", d)
              for n, d, _ in spec for s in sides}
    cpu = {(n, s.label): 0.0 for n, _, _ in spec for s in sides}
    for r in range(rounds):
        for name, _, build in spec:
            for side in sides:
                c0 = server_cpu_seconds(side.pid) if side.pid else 0.0
                for k in range(block):
                    arg = 1 + ((r * block + k) * 7919) % max_id
                    reply = side.client.timed(build(side.rel, arg),
                                              phases[(name, side.label)])
                    if reply.startswith("ERR"):
                        abort(f"arm `{name}` on `{side.label}` failed", reply)
                if side.pid:
                    cpu[(name, side.label)] += server_cpu_seconds(side.pid) - c0
    # `elapsed` is the time spent inside the arm, not the wall clock of the
    # whole interleaved run - an arm's share of a run that also drove nine
    # other arms would otherwise report a ninth of its real rate.
    for phase in phases.values():
        phase.elapsed = sum(phase.latencies)
    return phases, cpu


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--ops", type=int, default=0,
                    help="operations per arm; 0 picks a per-size default that "
                         "keeps each arm's server CPU well above /proc's jiffy")
    ap.add_argument("--rounds", type=int, default=10,
                    help="interleaving rounds; ops are split evenly across them")
    ap.add_argument("--limit", type=int, default=20, help="the LIMIT arms' n")
    ap.add_argument("--batch", type=int, default=200, help="load rows per txn")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--suffix", default="")
    ap.add_argument("--server-pid", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--json", default="")
    ap.add_argument("--label", default="", help="free-text tag for the JSON, "
                    "e.g. the commit the server binary was built from")
    ap.add_argument("--no-verify", dest="verify", action="store_false")
    ap.add_argument("--base", action="store_true",
                    help="the server on --port predates the output sort: run "
                         "only the arms it can answer (no non-pk ORDER BY)")
    ap.add_argument("--ab-port", type=int, default=0,
                    help="a second server, measured against --port's on the "
                         "arms both can answer, interleaved block by block")
    ap.add_argument("--ab-label", default="base")
    ap.add_argument("--ab-server-pid", type=int, default=0)
    ap.add_argument("--ab-base", action="store_true", default=True,
                    help="the --ab-port server predates the output sort "
                         "(the default; that is what the second side is for)")
    ap.add_argument("--pre-port", type=int, default=0,
                    help="a third server that DOES have the sort but predates "
                         "some change to it - it therefore answers every arm, "
                         "and pass 2 runs the full arm set across it and "
                         "--port's interleaved. This is what keeps a "
                         "'the fix is worth X' claim inside one run instead "
                         "of comparing today's numbers to a file's")
    ap.add_argument("--pre-label", default="pre-fix")
    ap.add_argument("--pre-server-pid", type=int, default=0)
    args = ap.parse_args()

    ops = args.ops or {200: 6000, 1000: 2000}.get(args.rows, 400)
    block = max(1, ops // args.rounds)
    ops = block * args.rounds

    suffix = args.suffix or f"{int(time.time())}_{random.randrange(1 << 16):04x}"
    rows = make_rows(args.rows, args.seed)

    primary = Side(args.label or "kds", args.host, args.port, args.server_pid,
                   args.timeout, has_sort=not args.base)
    sides = [primary]
    if args.pre_port:
        sides.append(Side(args.pre_label, args.host, args.pre_port,
                          args.pre_server_pid, args.timeout, has_sort=True))
    if args.ab_port:
        sides.append(Side(args.ab_label, args.host, args.ab_port,
                          args.ab_server_pid, args.timeout,
                          has_sort=not args.ab_base))

    setup = Phase("setup", "CREATE TABLE + load, per side")
    for side in sides:
        side.rel = f"ob_{suffix}"
        reply = side.client.timed(
            f"CREATE TABLE {side.rel} ({COLUMNS}) {CLUSTERED}", setup)
        if reply.startswith("ERR"):
            abort(f"could not create {side.rel} on `{side.label}`", reply)
        load(side.client, side.rel, rows, setup, args.batch)

        side.facts = analyze_facts(side.client, side.rel, args.limit,
                                   side.has_sort)
        # The elision is a claim about the server, so it is read off the
        # server rather than assumed from a flag.
        if side.facts["pk-order"]["sorted"] is not None:
            abort(f"on `{side.label}`, ORDER BY id ASC was NOT elided - the "
                  f"server reports sorted={side.facts['pk-order']['sorted']}. "
                  "The `pk-order` arm would be mislabelled.")
        if side.facts["plain"]["sorted"] is not None:
            abort(f"on `{side.label}`, a statement with no ORDER BY reports a sort")
        if side.has_sort:
            if side.facts["nonpk"]["sorted"] != args.rows:
                abort(f"ORDER BY val held {side.facts['nonpk']['sorted']} rows, "
                      f"expected {args.rows}")
            want_held = min(args.limit, args.rows)
            if side.facts["nonpk-lim"]["sorted"] != want_held:
                abort(f"the top-N heap held {side.facts['nonpk-lim']['sorted']} "
                      f"rows, expected {want_held}")
        if args.verify:
            side.verified = verify(side.client, side.rel, args.rows,
                                   args.limit, side.has_sort)

    max_id = args.rows  # ASSIGNED ids start at 1 and ascend
    started = time.time()

    # Pass 1, only when there are two sides: the arms both binaries answer,
    # interleaved between them. This is the before/after.
    shared_spec = arms(args.limit, sorted_arms=False) if len(sides) > 1 else []
    ab_phases, ab_cpu = ({}, {})
    if shared_spec:
        ab_phases, ab_cpu = run_pass(sides, shared_spec, args.rounds, block, max_id)

    # Pass 2: every arm, on every side that can answer them all. With no
    # `--pre-port` that is the primary alone and this is what the feature
    # costs; with one it is the primary and the older sort-carrying binary
    # interleaved, and it is additionally what the change between them cost.
    feat_spec = arms(args.limit, sorted_arms=primary.has_sort)
    feat_sides = [s for s in sides if s.has_sort]
    feat_phases, feat_cpu = run_pass(feat_sides, feat_spec, args.rounds, block, max_id)
    elapsed = time.time() - started

    meta = {
        "engine": "ckdbs",
        "label": args.label,
        "columns": COLUMNS,
        "rows": args.rows,
        "host": args.host,
        "port": args.port,
        "table": primary.rel,
        "clustered": CLUSTERED.lower(),
        "ops_per_arm": ops,
        "rounds": args.rounds,
        "block": block,
        "limit": args.limit,
        "seed": args.seed,
        "sides": [{"label": s.label, "port": s.port, "pid": s.pid,
                   "has_sort": s.has_sort} for s in sides],
        "elapsed_s": round(elapsed, 3),
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }

    def emit(title, spec, phases, cpu, side_list):
        print()
        print(f"==== {title} ====")
        ordered = [phases[(n, s.label)] for n, _, _ in spec for s in side_list]
        report([setup] + ordered, meta)
        if any(s.pid for s in side_list):
            print(f"{'arm':<24} {'cpu_us/op':>10}")
            for n, _, _ in spec:
                for s in side_list:
                    if not s.pid:
                        continue
                    print(f"{n + '[' + s.label + ']':<24} "
                          f"{cpu[(n, s.label)] / ops * 1e6:>10.1f}")

    if shared_spec:
        emit("pass 1: before/after, arms every binary answers",
             shared_spec, ab_phases, ab_cpu, sides)
    emit("pass 2: every arm, on every side that has the sort", feat_spec,
         feat_phases, feat_cpu, feat_sides)

    print()
    print("ANALYZE, per arm and side:")
    for side in sides:
        for name, f in side.facts.items():
            print(f"  [{side.label}] {name:<14} rows={f['rows']} "
                  f"examined={f['examined']} pages={f['pages']} "
                  f"sorted={f['sorted']}")

    if args.json:
        payload = {
            "meta": meta,
            "setup": setup.summary(),
            "ab": {f"{n}[{s.label}]": ab_phases[(n, s.label)].summary()
                   for n, _, _ in shared_spec for s in sides},
            "ab_cpu_us_per_op": {f"{n}[{s.label}]": round(ab_cpu[(n, s.label)] / ops * 1e6, 2)
                                 for n, _, _ in shared_spec for s in sides},
            "feature": {f"{n}[{s.label}]": feat_phases[(n, s.label)].summary()
                        for n, _, _ in feat_spec for s in feat_sides},
            "feature_cpu_us_per_op_all": {
                f"{n}[{s.label}]": round(feat_cpu[(n, s.label)] / ops * 1e6, 2)
                for n, _, _ in feat_spec for s in feat_sides if s.pid},
            "feature_primary_cpu_us_per_op": {
                n: round(feat_cpu[(n, primary.label)] / ops * 1e6, 2)
                for n, _, _ in feat_spec} if primary.pid else None,
            "analyze": {s.label: s.facts for s in sides},
            "verify": {s.label: s.verified for s in sides},
        }
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
