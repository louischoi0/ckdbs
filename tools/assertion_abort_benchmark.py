#!/usr/bin/env python3
"""What a ROLLBACK costs when the transaction reserved against an assertion.

`assertion_benchmark.py` prices the *commit* half of the assertion protocol:
every statement it measures is autocommitted, so `AssertionEnforcer::CommitTxn`
runs on every one of them and `AbortTxn` runs on none. This driver is the
abort half, and it exists because that half acquired a page write:
`AbortTxn` now does a page read-modify-write plus a `StampPageLsn` per aborted
reservation, to set `kEntryOrphaned` on the entry the reservation wrote
(`docs/spec/assertion.md` §7, AS6b). The claim made for that change is that it
is "what commit was already paying to clear `kEntryReserved`". This driver is
what tests the claim, because the two paths batch differently:

    CommitTxn   groups the pending reservations by (assertion, page), so one
                page fetch, one `BoundCabinPage::Open`, one WAL record and one
                `StampPageLsn` serve every entry that landed on that page.
    AbortTxn    walks the reservations one at a time, so the fetch, the open,
                the record and the stamp are all *per reservation*.

A transaction that reserves K times therefore pays K page fetches and K stamps
to abort against 1 and 1 to commit, whenever those K entries share a page --
which they normally do, because entries are appended in order. `--reservations`
is the knob that exposes exactly that: if abort and commit really cost the
same, their gap must not grow with K.

The relations, all BTREE, all loaded with identical rows in identical order:

    abt_none    no assertion - the control. Its ROLLBACK unwinds the undo
                trail and nothing else, so it prices everything about a
                rollback that is not the assertion protocol.
    abt_twin    no assertion - the in-run noise floor. `none` vs `twin` is the
                same configuration by construction, so a gap on any other row
                smaller than this one is not a finding.
    abt_cnt     CHECK COUNT(*) <= N GROUP BY (grp) - one reservation per
                inserted row.
    abt_multi   COUNT and SUM - two reservations per inserted row, which is
                what separates a per-reservation cost from a per-transaction
                one without changing the statement count.

The measured arms, per relation:

    ping        SHOW META. The client + socket round-trip floor; every other
                number here is above it and only the excess is the engine.
    begin       BEGIN. A control that touches no assertion state at all: it
                must not differ between the relations or between the binaries.
    insert      the K INSERTs inside the transaction, timed individually.
                This is the reserve path, and `assertion_benchmark.py`
                already owns it; it is here so that a change in the abort
                number can be attributed rather than guessed.
    rollback    ROLLBACK. **The measured unit.** One statement, unwinding K
                (or 2K) reservations.
    commit      COMMIT, over an identically shaped transaction. The
                symmetry test: abort-minus-commit at each K is the answer to
                whether the two halves of the protocol now cost the same.

---- How it measures -------------------------------------------------------

Transactions are interleaved across the relations - and across the two
binaries, when `--ab-port` names a second server - in blocks of `--block`
transactions, so a machine that gets busier partway through costs every arm
equally instead of inventing a result. With `--server-pid` / `--ab-server-pid`
the server's own CPU (`/proc/<pid>/stat`, utime+stime) is sampled around each
block and reported as **server CPU per transaction**, which is the meter
`docs/inflight/in-progress/workplan-aggregate-perf.md` requires for a per-statement fixed cost:
client latency carries the Python socket stack, which is most of a small
statement.

Every relation in a round sees the same argument draw, so the columns of the
table are one comparison rather than four runs.

`--verify` (on by default) closes the loop, because a throughput number over a
rollback that did not roll back is a measurement of nothing. After the
measured arms it checks, on every side: that `SELECT COUNT(*)` equals
`rows + committed*K` on every relation (the rolled-back rows are gone and the
committed ones are not), that the four relations agree row for row on the
assertion's own aggregate (`GROUP BY grp: COUNT(*), SUM(amount)`), and - the
one that actually tests this change - that the asserted relations' live
aggregate matches the unasserted control's, which is what `Unapply` must have
restored on every abort.

There is no PostgreSQL twin, for the same reason `assertion_benchmark.py` has
none: PostgreSQL parses `CREATE ASSERTION` in no released version, and the
nearest emulation (a constraint trigger) measures a different mechanism.

Run against **Release** servers on a **block device** data file, one fresh
data file and one fresh server per side (`bench/docs/README.md`). A server
built from a commit on either side of the WAL segment format bump cannot mount
the other side's file at all, so sharing one is not merely bad practice here.
"""

import argparse
import os
import random
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_common

AMOUNT_MAX = 1000
TAGS = ("none", "twin", "cnt", "multi")
ASSERTED = {"cnt": ("count",), "multi": ("count", "sum")}


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


class ServerCpu:
    """Samples one process's CPU seconds from /proc/<pid>/stat (utime+stime)."""

    def __init__(self, pid):
        self._path = "/proc/%d/stat" % pid
        self._tick = os.sysconf("SC_CLK_TCK")
        self()

    def __call__(self):
        with open(self._path) as f:
            line = f.read()
        fields = line.rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / self._tick


class Side:
    """One server: its connection, its relation names, its CPU sampler."""

    def __init__(self, label, host, port, pid, suffix, timeout):
        self.label = label
        self.port = port
        self.conn = Conn(host, port, timeout)
        self.cpu = ServerCpu(pid) if pid else None
        self.names = {tag: "abt_%s%s" % (tag, suffix) for tag in TAGS}
        self.assertions = []
        self.enforcing = None


def abort(message):
    print("assertion_abort_benchmark aborted: %s" % message, file=sys.stderr)
    return 2


# ---- setup ---------------------------------------------------------------

def create_and_load(side, rows, groups, batch):
    """Identical relations, identical rows, loaded interleaved across tags."""
    for tag in TAGS:
        side.conn.must("CREATE TABLE %s (id int64, grp int64, amount int64, "
                       "note varchar) BTREE" % side.names[tag])
    pending = 0
    side.conn.must("BEGIN")
    for i in range(rows):
        values = "%d, %d, 'r%d'" % (i % groups, (i * 7) % AMOUNT_MAX, i)
        for tag in TAGS:
            side.conn.must("INSERT INTO %s VALUES (%s)" % (side.names[tag], values))
        pending += 1
        if pending >= batch:
            side.conn.must("COMMIT")
            side.conn.must("BEGIN")
            pending = 0
    side.conn.must("COMMIT")


def declare(side, suffix, count_ceiling, sum_ceiling):
    """CREATE ASSERTION on the loaded relations. Setup, not a measured arm."""
    defs = {
        "cnt": [("abcnt%s" % suffix, "CHECK COUNT(*) <= %d" % count_ceiling)],
        "multi": [("abmc%s" % suffix, "CHECK COUNT(*) <= %d" % count_ceiling),
                  ("abms%s" % suffix, "CHECK SUM(amount) <= %d" % sum_ceiling)],
    }
    for tag, items in defs.items():
        for name, check in items:
            side.conn.must("CREATE ASSERTION %s ON %s GROUP BY (grp) %s"
                           % (name, side.names[tag], check))
            side.assertions.append(name)


def observed_enforcing(side):
    reply = side.conn.must("SHOW ASSERTIONS")
    state = {}
    for line in reply.split("\\n")[1:]:
        name = re.search(r"\bname=(\S+)", line)
        enf = re.search(r"\benforcing=(\d)", line)
        if name and enf:
            state[name.group(1)] = enf.group(1)
    missing = [n for n in side.assertions if n not in state]
    if missing:
        raise RuntimeError("SHOW ASSERTIONS did not list: %s" % ", ".join(missing))
    values = {state[n] for n in side.assertions}
    if values == {"1"}:
        return "on"
    if values == {"0"}:
        return "off"
    return "mixed"


# ---- the measured loop ---------------------------------------------------

class Meter:
    """Phases keyed by (side label, tag, arm), plus per-(side, tag, ending) CPU."""

    def __init__(self):
        self.phases = {}
        self.cpu_s = {}
        self.cpu_txns = {}

    def phase(self, side, tag, arm):
        key = (side.label, tag, arm)
        if key not in self.phases:
            self.phases[key] = bench_common.Phase("%s/%s/%s" % (side.label, arm, tag))
        return self.phases[key]

    def add_cpu(self, side, tag, ending, seconds, txns):
        key = (side.label, tag, ending)
        self.cpu_s[key] = self.cpu_s.get(key, 0.0) + seconds
        self.cpu_txns[key] = self.cpu_txns.get(key, 0) + txns


def draw(rng, txns, reservations, groups):
    return [["%d, %d, 'x'" % (rng.randrange(groups), rng.randrange(AMOUNT_MAX))
             for _ in range(reservations)] for _ in range(txns)]


def one_txn(side, meter, tag, ending, inserts):
    """BEGIN, K INSERTs, then COMMIT or ROLLBACK. Every statement timed."""
    name = side.names[tag]
    stmts = ["INSERT INTO %s VALUES (%s)" % (name, row) for row in inserts]

    t0 = time.perf_counter()
    reply = side.conn.send("BEGIN")
    meter.phase(side, tag, "begin").record(time.perf_counter() - t0, reply)

    ins = meter.phase(side, tag, "insert")
    for stmt in stmts:
        t0 = time.perf_counter()
        reply = side.conn.send(stmt)
        ins.record(time.perf_counter() - t0, reply)

    t0 = time.perf_counter()
    reply = side.conn.send(ending)
    meter.phase(side, tag, ending.lower()).record(time.perf_counter() - t0, reply)


def one_txn_untimed(side, tag, ending, inserts):
    """The same transaction with no per-statement clock, for the CPU pass."""
    name = side.names[tag]
    stmts = ["INSERT INTO %s VALUES (%s)" % (name, row) for row in inserts]
    side.conn.must("BEGIN")
    for stmt in stmts:
        side.conn.must(stmt)
    side.conn.must(ending)


def latency_pass(sides, meter, txns, block, reservations, groups, rng):
    """Blocks of `block` transactions, cycling (side, tag, ending).

    Wall clock only. Server CPU is *not* sampled here: a block of ten small
    transactions burns well under one 10 ms scheduler tick, so a per-block
    delta of /proc's jiffy counter is mostly quantisation, and summing many
    of them accumulates that error rather than cancelling it. The CPU meter
    is its own pass below, with one pair of reads around a window long
    enough that a tick is a rounding error.
    """
    draws = draw(rng, txns, reservations, groups)
    for start in range(0, txns, block):
        chunk = draws[start:start + block]
        for ending in ("ROLLBACK", "COMMIT"):
            for side in sides:
                for tag in TAGS:
                    for rows_for_txn in chunk:
                        one_txn(side, meter, tag, ending, rows_for_txn)

    # The round-trip floor, once per side, off the timed path above.
    for side in sides:
        p = meter.phase(side, "none", "ping")
        began = time.perf_counter()
        for _ in range(500):
            t0 = time.perf_counter()
            reply = side.conn.send("SHOW META")
            p.record(time.perf_counter() - t0, reply)
        p.elapsed = time.perf_counter() - began


def ordinary_pass(sides, meter, ops, block, groups, rng, rows):
    """The four autocommitted statement shapes, interleaved across binaries.

    This is the regression half of the driver and it is here rather than in
    `assertion_benchmark.py` for one reason: that driver owns its server, so an
    A/B across two binaries is two sequential runs, and two sequential runs of
    it on this box disagree with themselves by up to 26% at p50 - a whole-run
    shift, not a phase-level one. Interleaving the two binaries block by block
    inside one run puts both on the same side of any such shift, which is what
    makes a 1 us claim about an unchanged path sayable at all.

    Runs first, while the relations are still at their loaded size and no
    transaction has committed anything into them.
    """
    ins = ["%d, %d, 'a'" % (rng.randrange(groups), rng.randrange(AMOUNT_MAX))
           for _ in range(ops)]
    upd = [(rng.randrange(AMOUNT_MAX), rng.randrange(1, rows + 1))
           for _ in range(ops)]
    sel = [rng.randrange(1, rows + 1) for _ in range(ops)]
    # Distinct ids: a second DELETE of one row answers DELETED 0, which is a
    # different statement's work.
    dele = rng.sample(range(1, rows + 1), ops)

    arms = (
        ("ac-insert", ["INSERT INTO %%s VALUES (%s)" % v for v in ins]),
        ("ac-update", ["UPDATE %%s SET amount = %d WHERE id = %d" % v for v in upd]),
        ("ac-select", ["SELECT amount FROM %%s WHERE id = %d" % v for v in sel]),
        ("ac-delete", ["DELETE FROM %%s WHERE id = %d" % v for v in dele]),
    )
    for arm, commands in arms:
        for start in range(0, ops, block):
            chunk = commands[start:start + block]
            for side in sides:
                for tag in TAGS:
                    stmts = [c % side.names[tag] for c in chunk]
                    phase = meter.phase(side, tag, arm)
                    for stmt in stmts:
                        t0 = time.perf_counter()
                        reply = side.conn.send(stmt)
                        phase.record(time.perf_counter() - t0, reply)


def cpu_pass(sides, meter, rounds, txns, reservations, groups, rng):
    """Server CPU per transaction, one contiguous window per arm per round.

    The window is the unit because /proc/<pid>/stat advances in whole
    scheduler ticks: two reads around a window of `txns` transactions cost at
    most one tick of error, and `rounds` of them re-interleave the arms so a
    machine that drifts costs all of them equally. Latencies are not recorded
    - this pass exists to attribute CPU, and the wall clock already has its
    own pass.
    """
    for _ in range(rounds):
        draws = draw(rng, txns, reservations, groups)
        for ending in ("ROLLBACK", "COMMIT"):
            for side in sides:
                for tag in TAGS:
                    # Run on every side whether or not its CPU can be sampled:
                    # the two data files have to stay identical, so an
                    # unsampled side must still do the work.
                    before = side.cpu() if side.cpu else None
                    for rows_for_txn in draws:
                        one_txn_untimed(side, tag, ending, rows_for_txn)
                    if side.cpu:
                        meter.add_cpu(side, tag, ending.lower(),
                                      side.cpu() - before, len(draws))


# ---- verification --------------------------------------------------------

def aggregate(side, tag):
    reply = side.conn.must("SELECT grp, COUNT(*), SUM(amount) FROM %s GROUP BY grp"
                           % side.names[tag])
    return sorted(reply.split("\\n")[1:])


def count(side, tag):
    reply = side.conn.must("SELECT COUNT(*) FROM %s" % side.names[tag])
    m = re.search(r"(\d+)", reply.split("\\n")[-1])
    return int(m.group(1)) if m else -1


def verify(sides, rows, txns, reservations, ordinary_ops=0):
    """The rolled-back rows are gone, the committed ones are not, and every
    relation agrees with the unasserted control on the assertion's own
    aggregate. That last one is what `Unapply` has to have restored."""
    # The ordinary pass's INSERT and DELETE arms run the same op count over
    # distinct preloaded pks, so together they net to zero rows - and a count
    # that lands anywhere else is how a half-run pass would show up here.
    del ordinary_ops
    expected = rows + txns * reservations
    problems = []
    for side in sides:
        base = None
        for tag in TAGS:
            got = count(side, tag)
            if got != expected:
                problems.append("%s/%s: COUNT(*)=%d, expected %d"
                                % (side.label, tag, got, expected))
            agg = aggregate(side, tag)
            if base is None:
                base = agg
            elif agg != base:
                problems.append("%s/%s: GROUP BY aggregate differs from %s/none"
                                % (side.label, tag, side.label))
    return problems


# ---- reporting -----------------------------------------------------------

def cpu_per_txn(meter, label, tag, ending):
    key = (label, tag, ending)
    if key in meter.cpu_s and meter.cpu_txns.get(key):
        return meter.cpu_s[key] / meter.cpu_txns[key] * 1e6
    return None


def table(meter, sides, arms):
    print()
    header = ("%-10s %-8s %-9s %7s %9s %9s %9s %9s %9s %9s"
              % ("side", "arm", "relation", "ops", "mean us", "p0", "p25", "p50",
                 "p95", "p99"))
    print(header)
    print("-" * len(header))
    for arm in arms:
        for side in sides:
            for tag in TAGS:
                key = (side.label, tag, arm)
                if key not in meter.phases:
                    continue
                s = meter.phases[key].summary()
                print("%-10s %-8s %-9s %7d %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f"
                      % (side.label, arm, tag, s["ops"], s["mean_us"], s["p0_us"],
                         s["p25_us"], s["p50_us"], s["p95_us"], s["p99_us"]))
    print()


def cpu_table(meter, sides):
    if not meter.cpu_s:
        print("server CPU: not sampled (pass --server-pid / --ab-server-pid)")
        print()
        return
    print("server CPU per transaction (us), one contiguous window per arm per "
          "round. The whole transaction is in each number - BEGIN, the K "
          "INSERTs, and the ending - so `rollback - commit` on one relation is "
          "the ending's own cost with everything else subtracted out.")
    head = "%-10s %-9s %12s %12s %14s" % (
        "side", "relation", "rollback", "commit", "roll - commit")
    print(head)
    print("-" * len(head))
    for side in sides:
        for tag in TAGS:
            r = cpu_per_txn(meter, side.label, tag, "rollback")
            c = cpu_per_txn(meter, side.label, tag, "commit")
            if r is None or c is None:
                continue
            print("%-10s %-9s %12.1f %12.1f %14.1f" % (side.label, tag, r, c, r - c))
    print()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=15432)
    p.add_argument("--label", default="new")
    p.add_argument("--server-pid", type=int, default=0)
    p.add_argument("--ab-port", type=int, default=0,
                   help="a second server, on a second binary and a second data "
                        "file; its arms interleave block by block with --port's")
    p.add_argument("--ab-label", default="base")
    p.add_argument("--ab-server-pid", type=int, default=0)
    p.add_argument("--rows", type=int, default=1000,
                   help="rows preloaded into every relation (sweep 200/1000/10000)")
    p.add_argument("--txns", type=int, default=200,
                   help="measured transactions per relation per ending")
    p.add_argument("--reservations", type=int, default=10,
                   help="INSERTs per transaction; with --multi that is 2x the "
                        "reservations. This is the per-reservation-cost knob")
    p.add_argument("--groups", type=int, default=64)
    p.add_argument("--block", type=int, default=10,
                   help="transactions per interleaved block, latency pass")
    p.add_argument("--cpu-rounds", type=int, default=4,
                   help="rounds of the server-CPU pass; 0 skips it")
    p.add_argument("--cpu-txns", type=int, default=150,
                   help="transactions per arm per CPU round - long enough that "
                        "one 10 ms scheduler tick is a rounding error")
    p.add_argument("--ordinary-ops", type=int, default=0,
                   help="autocommitted INSERT/UPDATE/SELECT/DELETE statements "
                        "per relation per side, interleaved across binaries. "
                        "Must be <= --rows (the delete arm needs distinct pks). "
                        "0 skips the pass")
    p.add_argument("--load-batch", type=int, default=200)
    p.add_argument("--suffix", default="")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--expect-enforcing", default="on", choices=("on", "off", "any"))
    p.add_argument("--no-verify", dest="verify", action="store_false")
    p.add_argument("--json", default=None)
    args = p.parse_args()

    load1, load5, _ = os.getloadavg()
    rng = random.Random(args.seed)

    # The ceiling has to cover every row both passes will commit, or the
    # workload would trip the bound it declared and measure the refusal path.
    committed = args.txns + args.cpu_rounds * args.cpu_txns
    total_rows = args.rows + committed * args.reservations
    count_ceiling = 2 * total_rows
    sum_ceiling = 2 * total_rows * AMOUNT_MAX

    sides = [Side(args.label, args.host, args.port, args.server_pid,
                  args.suffix, args.timeout)]
    if args.ab_port:
        sides.append(Side(args.ab_label, args.host, args.ab_port,
                          args.ab_server_pid, args.suffix, args.timeout))

    setup_started = time.perf_counter()
    for side in sides:
        create_and_load(side, args.rows, args.groups, args.load_batch)
        declare(side, args.suffix, count_ceiling, sum_ceiling)
        side.enforcing = observed_enforcing(side)
        if args.expect_enforcing != "any" and side.enforcing != args.expect_enforcing:
            return abort("side %s reports enforcing=%s, expected %s - refusing to "
                         "measure a mislabelled engine"
                         % (side.label, side.enforcing, args.expect_enforcing))
    setup_s = time.perf_counter() - setup_started

    if args.ordinary_ops > args.rows:
        return abort("--ordinary-ops (%d) must be <= --rows (%d): the delete arm "
                     "needs that many distinct pks"
                     % (args.ordinary_ops, args.rows))

    meter = Meter()
    ordinary_started = time.perf_counter()
    if args.ordinary_ops:
        ordinary_pass(sides, meter, args.ordinary_ops, args.block, args.groups,
                      rng, args.rows)
    ordinary_elapsed = time.perf_counter() - ordinary_started
    started = time.perf_counter()
    latency_pass(sides, meter, args.txns, args.block, args.reservations,
                 args.groups, rng)
    elapsed = time.perf_counter() - started
    cpu_started = time.perf_counter()
    cpu_txns_done = 0
    if args.cpu_rounds:
        cpu_pass(sides, meter, args.cpu_rounds, args.cpu_txns, args.reservations,
                 args.groups, rng)
        cpu_txns_done = args.cpu_rounds * args.cpu_txns
    cpu_elapsed = time.perf_counter() - cpu_started
    for phase in meter.phases.values():
        if phase.elapsed == 0.0:
            phase.elapsed = elapsed

    errors = sum(ph.errors for ph in meter.phases.values())
    if errors:
        first = next(ph for ph in meter.phases.values() if ph.errors)
        return abort("%d error replies (first in %s: %s) - this run is not a "
                     "measurement" % (errors, first.name, first.first_error))

    problems = []
    if args.verify:
        problems = verify(sides, args.rows, args.txns + cpu_txns_done,
                          args.reservations, args.ordinary_ops)

    load1b, load5b, _ = os.getloadavg()
    print()
    print("assertion abort/commit - %d rows preloaded, %d txns per relation per "
          "ending, %d INSERTs per txn, %d groups"
          % (args.rows, args.txns, args.reservations, args.groups))
    for side in sides:
        print("  side %-6s port %d  enforcing=%s  server cpu %s"
              % (side.label, side.port, side.enforcing,
                 "sampled" if side.cpu else "not sampled"))
    print("  load average %.2f/%.2f (1m) %.2f/%.2f (5m) before/after; setup %.1f s, "
          "ordinary pass %.1f s, latency pass %.1f s, cpu pass %.1f s "
          "(%d rounds x %d txns per arm)"
          % (load1, load1b, load5, load5b, setup_s, ordinary_elapsed, elapsed,
             cpu_elapsed, args.cpu_rounds if cpu_txns_done else 0, args.cpu_txns))

    table(meter, sides, ("ping", "ac-insert", "ac-update", "ac-select",
                         "ac-delete", "begin", "insert", "rollback", "commit"))
    cpu_table(meter, sides)

    # The two comparisons this driver exists to make, printed rather than left
    # to the reader: abort against its own control, and abort against commit.
    print("deltas (p50 us): rollback vs the unasserted control, and rollback vs "
          "commit on the same relation")
    head = "%-10s %-9s %12s %12s %12s %12s" % (
        "side", "relation", "rollback p50", "vs none", "commit p50", "roll-commit")
    print(head)
    print("-" * len(head))
    for side in sides:
        base = meter.phases.get((side.label, "none", "rollback"))
        base_p50 = base.summary()["p50_us"] if base else 0.0
        for tag in TAGS:
            r = meter.phases.get((side.label, tag, "rollback"))
            c = meter.phases.get((side.label, tag, "commit"))
            if not r or not c:
                continue
            rp = r.summary()["p50_us"]
            cp = c.summary()["p50_us"]
            print("%-10s %-9s %12.1f %12.1f %12.1f %12.1f"
                  % (side.label, tag, rp, rp - base_p50, cp, rp - cp))
    print()

    if problems:
        for line in problems:
            print("VERIFY FAILED: %s" % line, file=sys.stderr)
        return 1
    if args.verify:
        print("verify: every relation holds %d rows and every asserted relation's "
              "GROUP BY aggregate equals the unasserted control's"
              % (args.rows + (args.txns + cpu_txns_done) * args.reservations))

    if args.json:
        meta = {
            "engine": "ckdbs",
            "driver": "assertion_abort_benchmark.py",
            "rows": args.rows,
            "txns": args.txns,
            "reservations": args.reservations,
            "groups": args.groups,
            "block": args.block,
            "cpu_rounds": args.cpu_rounds,
            "cpu_txns": args.cpu_txns,
            "ordinary_ops": args.ordinary_ops,
            "seed": args.seed,
            "sides": [{"label": s.label, "port": s.port, "enforcing": s.enforcing}
                      for s in sides],
            "loadavg_before": [load1, load5],
            "loadavg_after": [load1b, load5b],
            "verify": "passed" if args.verify and not problems else
                      ("skipped" if not args.verify else "failed"),
        }
        rows_out = []
        for (label, tag, arm), phase in sorted(meter.phases.items()):
            s = phase.summary()
            s["side"] = label
            s["relation"] = tag
            s["arm"] = arm
            if arm in ("rollback", "commit"):
                per = cpu_per_txn(meter, label, tag, arm)
                if per is not None:
                    s["cpu_us_per_txn"] = round(per, 2)
            rows_out.append(s)
        import json
        with open(args.json, "w") as f:
            json.dump({"meta": meta, "phases": rows_out}, f, indent=2)
        print("wrote %s" % args.json)

    return 0


if __name__ == "__main__":
    sys.exit(main())
