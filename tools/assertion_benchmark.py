#!/usr/bin/env python3
"""What a declared assertion costs on the write path: with, against without.

`docs/spec/assertion.md` restricts `CREATE ASSERTION` to group ceilings
(`GROUP BY (cols) CHECK COUNT(*)|SUM(col) <= N`) precisely so that the check
can ride the write path instead of re-evaluating a query. This driver prices
that ride as a *difference*: every measured statement runs against relations
with byte-identical contents, one carrying no assertion and the others
carrying the declared bounds, and the gap is the answer.

**Read the `enforcing=` stamp before reading any number.** As of AST01-AST03
a declaration is parsed, validated and recorded and enforced by nothing -
`SHOW ASSERTIONS` says `enforcing=0` - so the honest expectation today is a
zero delta on every phase, and this run pins that baseline. The same driver,
unchanged, prices enforcement the day AST06/AST07 publish a Bound Cabin root
and the stamp flips to `enforcing=1`. The driver reads the stamp from the
server rather than assuming it, and records it in the output; a number
without its stamp is unreadable, because the two states measure different
engines. `--expect-enforcing on|off` turns a surprise into a failed run.

The relations, all BTREE, all loaded with the same rows in the same order:

    ast_none    no assertion - the control every delta is against
    ast_twin    no assertion - the noise floor: none vs twin is the same
                configuration by construction, and a delta smaller than that
                gap on any other row is not a finding
    ast_cnt     CHECK COUNT(*)  <= N   GROUP BY (grp)
    ast_sum     CHECK SUM(amount) <= N GROUP BY (grp)
    ast_multi   both of the above - prices per-assertion scaling
    ast_cap     a small dedicated relation with a deliberately tiny COUNT
                ceiling, for the violation phase only (its contents diverge
                by design, so it is excluded from every comparison)

The measured phases, and what each one will mean under enforcement:

    declare      CREATE ASSERTION on the already-loaded relation, once. AS7
                 says the create scans the relation to build the Bound Cabin
                 and initial aggregates; this is that scan's price (today:
                 declaration + validation only).
    insert       the checked statement: a new row raises its group's COUNT
                 and SUM, so this is where a ceiling check must run.
    update-sum   SET amount = ?  - raises/lowers a summed column; checked
                 under a SUM assertion.
    update-grp   SET grp = ?     - moves a row between groups; the
                 destination group's ceiling is what needs checking.
    update-note  SET note = ?    - touches no asserted column. Must stay
                 free under enforcement, the same rule the index write hook
                 and the Cabin hook already obey.
    pk-select    the read control: assertions live on the write path, so
                 this row must not move at all, in either enforcement state.
    delete       AS11's dividend, measured: v1 is upper bounds only, which
                 is exactly what makes DELETE check-free. The delta here
                 must be zero even under enforcement.
    violate      inserts into the capped group of `ast_cap`, past its
                 ceiling. Under enforcing=0 all succeed (the gap
                 `example/assertion.sql` §2 demonstrates, in numbers);
                 under enforcing=1 all are refused and the latency prices
                 the refusal path.

**Ceilings are computed with 2x headroom** over the loosest bound the
workload can reach (every row in one group), so nothing in the comparison
phases ever violates: the pass path is the measurement, and the violation
path is its own phase on its own relation.

---- How it measures -------------------------------------------------------

Statements are interleaved across the relations in **blocks** (default 25)
rather than run relation-by-relation, so a machine that gets busier partway
through costs every relation equally instead of inventing a result. Blocks
rather than single statements because of the second meter: with
`--server-pid`, the driver samples the server's CPU (`/proc/<pid>/stat`,
utime+stime) around each block and reports **server CPU per operation** per
relation beside the wall-clock percentiles. That is the measurement
`docs/inflight/in-progress/workplan-aggregate-perf.md` requires for per-statement fixed costs -
client latency carries the Python socket stack, which is most of a small
statement - and it cannot be attributed per-relation under per-statement
interleaving, so the block is the unit. Build the statement strings before
the block starts; formatting is not server time.

Every relation in a round sees the *same* argument draw, so the columns of
the table are one comparison rather than five runs. `--verify` closes the
loop: after the measured phases it runs the assertion's own aggregate
(`SELECT grp, COUNT(*), SUM(amount) ... GROUP BY grp`) on every comparison
relation and requires identical row sets - declaring a bound must change no
answer.

There is no PostgreSQL twin: PostgreSQL parses `CREATE ASSERTION` in no
released version, and the nearest emulation (a constraint trigger) measures
a different mechanism, not a baseline.

Run against a **Release** server (`build-release/kds_server`) on a **block
device** data file, like every write measurement in `bench/` (see
`bench/docs/README.md`). Rerunning against one data file needs `--suffix`,
because nothing drops a table.
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

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 15432

AMOUNT_MAX = 1000          # amounts are drawn in [0, AMOUNT_MAX)
COMPARE_TAGS = ("none", "twin", "cnt", "sum", "multi")
ASSERTED = {"cnt": ("count",), "sum": ("sum",), "multi": ("count", "sum")}


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
    """Samples one process's CPU seconds from /proc/<pid>/stat.

    utime+stime only: what the server itself burned, not what it waited on.
    The comm field may contain spaces, so the parse anchors on the closing
    paren rather than splitting the whole line.
    """

    def __init__(self, pid):
        self._path = "/proc/%d/stat" % pid
        self._tick = os.sysconf("SC_CLK_TCK")
        self()  # fail now, not mid-measurement, if the pid is wrong

    def __call__(self):
        with open(self._path) as f:
            line = f.read()
        fields = line.rsplit(")", 1)[1].split()
        # fields[0] is stat field 3 (state); utime/stime are fields 14/15.
        return (int(fields[11]) + int(fields[12])) / self._tick


# ---- setup ---------------------------------------------------------------

def load(conn, names, rows, groups, batch):
    """Loads every comparison relation with identical rows, interleaved.

    Interleaved for the same reason index_benchmark.py's load is: these
    relations are compared against each other, and loading one fully first
    would hand the next a data file that already holds it. Batched in
    transactions because the load is setup, not a measurement. No assertion
    exists yet, so every relation pays an identical load by construction.
    """
    tags = list(names)
    if batch > 1:
        conn.must("BEGIN")
    pending = 0
    for i in range(rows):
        values = "%d, %d, 'r%d'" % (i % groups, (i * 7) % AMOUNT_MAX, i)
        for tag in tags:
            conn.must("INSERT INTO %s VALUES (%s)" % (names[tag], values))
        pending += 1
        if batch > 1 and pending >= batch:
            conn.must("COMMIT")
            conn.must("BEGIN")
            pending = 0
    if batch > 1:
        conn.must("COMMIT")


def declare(conn, phases, names, suffix, count_ceiling, sum_ceiling):
    """CREATE ASSERTION on the loaded relations, each timed once.

    Returns the assertion names, for the enforcement stamp lookup.
    """
    stmts = {
        "cnt": [("cnt_lim%s" % suffix,
                 "CHECK COUNT(*) <= %d" % count_ceiling)],
        "sum": [("sum_lim%s" % suffix,
                 "CHECK SUM(amount) <= %d" % sum_ceiling)],
        "multi": [("multi_cnt%s" % suffix,
                   "CHECK COUNT(*) <= %d" % count_ceiling),
                  ("multi_sum%s" % suffix,
                   "CHECK SUM(amount) <= %d" % sum_ceiling)],
    }
    created = []
    for tag, defs in stmts.items():
        phase = bench_common.Phase("declare[%s]" % tag)
        started = time.perf_counter()
        for name, check in defs:
            stmt = ("CREATE ASSERTION %s ON %s GROUP BY (grp) %s"
                    % (name, names[tag], check))
            t0 = time.perf_counter()
            reply = conn.send(stmt)
            phase.record(time.perf_counter() - t0, reply)
            if reply.startswith("ERR"):
                raise RuntimeError("%s -> %s" % (stmt, reply))
            created.append(name)
        phase.elapsed = time.perf_counter() - started
        phases.append(phase)
    return created


def observed_enforcing(conn, assertion_names):
    """Reads enforcing= per assertion out of SHOW ASSERTIONS.

    Returns 'on', 'off' or 'mixed'. The driver does not take its own word
    for which engine it measured - the stamp comes from the server.
    """
    reply = conn.must("SHOW ASSERTIONS")
    state = {}
    for line in reply.split("\\n")[1:]:
        name = re.search(r"\bname=(\S+)", line)
        enf = re.search(r"\benforcing=(\d)", line)
        if name and enf:
            state[name.group(1)] = enf.group(1)
    missing = [n for n in assertion_names if n not in state]
    if missing:
        raise RuntimeError("SHOW ASSERTIONS did not list: %s" % ", ".join(missing))
    values = {state[n] for n in assertion_names}
    if values == {"1"}:
        return "on"
    if values == {"0"}:
        return "off"
    return "mixed"


# ---- the measured loop ---------------------------------------------------

def run_interleaved(conn, phases, cpu, names, phase_name, commands, block, sampler):
    """Runs one phase's commands against every relation, block-interleaved.

    `commands` is a list of per-round format strings taking the relation
    name, so every relation sees the same argument draw. Wall clock is
    per statement; server CPU, when sampled, is per block per relation.
    """
    tags = list(names)
    by_tag = {tag: bench_common.Phase("%s[%s]" % (phase_name, tag)) for tag in tags}
    for start in range(0, len(commands), block):
        chunk = commands[start:start + block]
        for tag in tags:
            stmts = [c % names[tag] for c in chunk]  # formatted outside the clock
            phase = by_tag[tag]
            began = time.perf_counter()
            before = sampler() if sampler else 0.0
            for stmt in stmts:
                t0 = time.perf_counter()
                reply = conn.send(stmt)
                phase.record(time.perf_counter() - t0, reply)
            if sampler:
                key = (phase_name, tag)
                cpu[key] = cpu.get(key, 0.0) + sampler() - before
            phase.elapsed += time.perf_counter() - began
    phases.extend(by_tag[tag] for tag in tags)
    return by_tag


def verify(conn, names, statement):
    """One aggregate over every comparison relation; the row sets must match.

    Compared as sorted sets rather than raw replies: the claim is that a
    declared bound changed no *answer*, and group emission order is the
    fold's business, not this driver's.
    """
    replies = {}
    for tag in COMPARE_TAGS:
        reply = conn.must(statement % names[tag])
        replies[tag] = sorted(reply.split("\\n"))
    baseline = replies["none"]
    for tag, rows in replies.items():
        if rows != baseline:
            raise RuntimeError(
                "verify failed: %s answers differently from %s for `%s`"
                % (names[tag], names["none"], statement % "..."))


# ---- reporting -----------------------------------------------------------

def delta_report(grouped, cpu):
    """Per phase: each asserted relation against the control, twin first.

    The twin line is the noise floor. A delta on any asserted row smaller
    than the twin's is not a finding, and the report says so once instead
    of hoping the reader remembers.
    """
    print("deltas against %s (twin = in-run noise floor):" % "ast_none")
    header = "  %-14s %-8s %12s %10s %14s" % (
        "phase", "vs", "mean us", "delta", "server cpu us")
    print(header)
    print("  " + "-" * (len(header) - 2))
    for phase_name, by_tag in grouped:
        base = by_tag["none"].summary()
        for tag in ("twin", "cnt", "sum", "multi"):
            s = by_tag[tag].summary()
            delta_us = s["mean_us"] - base["mean_us"]
            pct = (delta_us / base["mean_us"] * 100.0) if base["mean_us"] else 0.0
            key = (phase_name, tag)
            base_key = (phase_name, "none")
            if key in cpu and base_key in cpu and by_tag[tag].ops:
                cpu_us = (cpu[key] - cpu[base_key]) / by_tag[tag].ops * 1e6
                cpu_col = "%+14.2f" % cpu_us
            else:
                cpu_col = "%14s" % "-"
            print("  %-14s %-8s %12.1f %+9.1fus %s   (%+.1f%%)"
                  % (phase_name, tag, s["mean_us"], delta_us, cpu_col, pct))
    print()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--suffix", default="",
                        help="appended to every table and assertion name; "
                             "required to rerun against one data file, "
                             "because nothing drops a table")
    parser.add_argument("--rows", type=int, default=5000,
                        help="setup rows per relation (default: 5000)")
    parser.add_argument("--groups", type=int, default=64,
                        help="distinct grp values - the group count is what "
                             "a per-group ceiling structure scales with "
                             "(default: 64)")
    parser.add_argument("--ops", type=int, default=2000,
                        help="measured statements per phase per relation "
                             "(default: 2000)")
    parser.add_argument("--block", type=int, default=25,
                        help="statements per relation per interleaving "
                             "round; also the server-CPU sampling unit "
                             "(default: 25)")
    parser.add_argument("--load-batch", type=int, default=500,
                        help="setup-load transaction size (default: 500)")
    parser.add_argument("--cap", type=int, default=8,
                        help="ast_cap's COUNT ceiling for the violation "
                             "phase (default: 8)")
    parser.add_argument("--violate-ops", type=int, default=200,
                        help="inserts attempted past the ceiling "
                             "(default: 200)")
    parser.add_argument("--server-pid", type=int, default=None,
                        help="kds_server pid; enables per-block server CPU "
                             "sampling from /proc/<pid>/stat")
    parser.add_argument("--expect-enforcing", choices=("on", "off", "any"),
                        default="any",
                        help="fail the run if the server's observed "
                             "enforcing state differs (default: any)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--verify", action="store_true",
                        help="require identical aggregates across every "
                             "comparison relation after the measured phases")
    parser.add_argument("--json", default=None,
                        help="write the standard summary JSON here")
    args = parser.parse_args()

    if args.ops > args.rows:
        print("--ops must be <= --rows: the delete phase needs that many "
              "distinct live rows", file=sys.stderr)
        return 2

    rng = random.Random(args.seed)
    names = {tag: "ast_%s%s" % (tag, args.suffix) for tag in COMPARE_TAGS}
    cap_name = "ast_cap%s" % args.suffix

    # The loosest bound any comparison-phase workload can reach is every row
    # in one group; 2x that can never trip, so the pass path is what runs.
    total_rows = args.rows + args.ops
    count_ceiling = 2 * total_rows
    sum_ceiling = 2 * total_rows * AMOUNT_MAX

    conn = Conn(args.host, args.port, args.timeout)
    sampler = ServerCpu(args.server_pid) if args.server_pid else None
    phases, cpu, grouped = [], {}, []

    try:
        # ---- setup: identical relations, then the declarations ----------
        for tag in COMPARE_TAGS:
            conn.must("CREATE TABLE %s (id int64, grp int64, amount int64, "
                      "note varchar) BTREE" % names[tag])
        conn.must("CREATE TABLE %s (id int64, grp int64, amount int64, "
                  "note varchar) BTREE" % cap_name)
        load(conn, names, args.rows, args.groups, args.load_batch)

        created = declare(conn, phases, names, args.suffix,
                          count_ceiling, sum_ceiling)
        cap_assertion = "cap_lim%s" % args.suffix
        conn.must("CREATE ASSERTION %s ON %s GROUP BY (grp) "
                  "CHECK COUNT(*) <= %d" % (cap_assertion, cap_name, args.cap))
        created.append(cap_assertion)

        enforcing = observed_enforcing(conn, created)
        if args.expect_enforcing != "any" and enforcing != args.expect_enforcing:
            print("expected enforcing=%s, server reports %s - refusing to "
                  "measure a mislabelled engine"
                  % (args.expect_enforcing, enforcing), file=sys.stderr)
            return 2

        # ---- measured phases, identical draws for every relation --------
        # insert: fresh rows continuing the load's numbering.
        inserts = ["INSERT INTO %%s VALUES (%d, %d, 'w%d')"
                   % (i % args.groups, (i * 7) % AMOUNT_MAX, i)
                   for i in range(args.rows, total_rows)]
        # updates and the select draw pks from the setup range: those rows
        # exist in every relation under the same id, and none is deleted yet.
        upd_sum = ["UPDATE %%s SET amount = %d WHERE id = %d"
                   % (rng.randrange(AMOUNT_MAX), rng.randrange(1, args.rows + 1))
                   for _ in range(args.ops)]
        upd_grp = ["UPDATE %%s SET grp = %d WHERE id = %d"
                   % (rng.randrange(args.groups), rng.randrange(1, args.rows + 1))
                   for _ in range(args.ops)]
        upd_note = ["UPDATE %%s SET note = 'u%d' WHERE id = %d"
                    % (j, rng.randrange(1, args.rows + 1))
                    for j in range(args.ops)]
        selects = ["SELECT amount FROM %%s WHERE id = %d"
                   % rng.randrange(1, args.rows + 1) for _ in range(args.ops)]
        # delete last, and distinct ids: a second DELETE of the same row
        # answers DELETED 0, which is a different statement's work.
        deletes = ["DELETE FROM %%s WHERE id = %d" % k
                   for k in rng.sample(range(1, args.rows + 1), args.ops)]

        for phase_name, commands in (
                ("insert", inserts),
                ("update-sum", upd_sum),
                ("update-grp", upd_grp),
                ("update-note", upd_note),
                ("pk-select", selects),
                ("delete", deletes)):
            by_tag = run_interleaved(conn, phases, cpu, names, phase_name,
                                     commands, args.block, sampler)
            grouped.append((phase_name, by_tag))

        unexpected = sum(p.errors for p in phases)
        if unexpected:
            first = next(p for p in phases if p.errors)
            print("measured phases produced %d error replies (first in %s: "
                  "%s) - this run is not a measurement"
                  % (unexpected, first.name, first.first_error), file=sys.stderr)
            return 2

        # ---- the violation phase, on its own relation --------------------
        for i in range(args.cap):
            conn.must("INSERT INTO %s VALUES (0, 1, 'fill%d')" % (cap_name, i))
        violate = bench_common.Phase(
            "violate[cap]",
            "inserts past a COUNT(*) <= %d ceiling; errors are the point"
            % args.cap)
        began = time.perf_counter()
        for i in range(args.violate_ops):
            t0 = time.perf_counter()
            reply = conn.send("INSERT INTO %s VALUES (0, 1, 'over%d')"
                              % (cap_name, i))
            violate.record(time.perf_counter() - t0, reply)
        violate.elapsed = time.perf_counter() - began
        phases.append(violate)

        if args.verify:
            verify(conn, names,
                   "SELECT grp, COUNT(*), SUM(amount) FROM %s GROUP BY grp")

        # ---- report -------------------------------------------------------
        meta = {
            "engine": "ckdbs",
            "columns": 4,
            "rows": args.rows,
            "clustered": "BTREE",
            "host": args.host,
            "port": args.port,
            "table": "ast_*%s" % args.suffix,
            "groups": args.groups,
            "ops": args.ops,
            "block": args.block,
            "seed": args.seed,
            "enforcing": enforcing,
            "count_ceiling": count_ceiling,
            "sum_ceiling": sum_ceiling,
            "cap": args.cap,
        }
        if sampler:
            meta["server_cpu_s"] = {
                "%s[%s]" % key: round(seconds, 6)
                for key, seconds in sorted(cpu.items())}

        footer = [
            "enforcing=%s observed via SHOW ASSERTIONS - with AST01-AST03 "
            "only, every delta should be noise" % enforcing,
            "violate[cap]: %d/%d refused - %s"
            % (violate.errors, violate.ops,
               "enforcement is live" if violate.errors else
               "declared bounds do not enforce yet (AST07 open), as "
               "example/assertion.sql §2 demonstrates"),
            "latencies include the Python client's socket cost; the twin "
            "row is this run's noise floor",
        ]
        bench_common.report(phases, meta, footer)
        delta_report(grouped, cpu)
        if args.verify:
            print("verify: aggregates identical across %s"
                  % ", ".join(names[t] for t in COMPARE_TAGS))
        if args.json:
            bench_common.write_json(args.json, meta, phases)
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
