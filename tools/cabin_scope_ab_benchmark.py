#!/usr/bin/env python3
"""SB-M1: is a one-range relation's Cabin serve path cycle-neutral after SB1-SB4?

Work order SB (`instructions/v2.7.1/workorder-sb.md`) scoped a Cabin's
authority to (observed value x the ranges its core owns) so a split
relation cannot serve a subset as if it were the whole answer
(`docs/spec/cabin.md` SS4b). H-SB1 is the claim that on a relation of
**one range** - every relation until one asks to spread - this costs
nothing: `CabinScopeCovers` (`src/exec/step_vm.cpp`) returns true on
`access.ranges.empty()` before touching the range directory or
`ServableBy`, so the new code path is one load from a `TableAccess` the
step already holds and one predictable branch. SB2 asserted this; this
driver measures it.

**The two arms are two binaries, never one server with a config flip** -
the scoping is compiled in, not a runtime switch. `--port`/`--server-pid`
names the side under test (post-SB, 71f92f6); `--ab-port`/`--ab-server-pid`
names the baseline (pre-SB, e1897dc, code-identical to base 24ce3cd). Both
must already be running, each against its own data file, before this
driver starts - see `bench/docs/README.md` for the exact invocation.

**The shape.** Four relations per side per row-set size, all loaded with
the same generated data:

  {ch}   HEAP,  `val varchar CABIN`-declared via `CREATE CABIN`  - the
         Cabin-served probe under test, heap-clustered
  {cb}   BTREE, same declaration                                  - the
         btree-clustered twin, which serves differently on a hint miss
  {uh}   HEAP,  no Cabin                                          - the
         control: an equality that always walks, on both arms alike,
         because `CabinScopeCovers` is only ever reached from
         `RunCabinStep`, which a column with no declared Cabin never
         compiles into. Any delta here is the host, not SB
  {ub}   BTREE, no Cabin                                          - the
         control's btree twin

A declared Cabin (`CREATE CABIN`, not the engine's own `CABIN AUTO`) has
`kDeclaredRecordThreshold = 1` (`cabin_store.hpp`): the first probe of a
value is still a miss but records it, and the second probe onward is
`cabin_hits=1`. Every hot value is therefore warmed twice, untimed,
before the timed pass, and one `ANALYZE` per cabined relation confirms
`cabin_hits=1` before any statement is counted - a run that measured a
walk under a "Cabin-served" heading would be measuring nothing.

**Row-set size is swept inside one invocation** (`--rows` is a
comma-separated list, default `200,1000,10000`, rule 9's floor): one
server pair, one data file pair, one relation quartet per size per side,
so a fixed per-branch cost shows up as flat across three cardinalities
that differ 50x, and a per-row one would not.

**Server CPU** comes from a second pass of contiguous `/proc/<pid>/stat`
windows, never summed over the latency pass's blocks - `/proc` advances
in whole scheduler ticks, and a 10ms sampling error would swamp a
one-branch cost measured in nanoseconds.

**Byte-identity.** `--verify` (default on) runs a small query set per
size - a cabin hit, a cabin miss, an equality with an extra conjunct, a
pk lookup, a correlated self-join through the Cabin, and the same shapes
on the uncabined controls - against both sides and diffs every reply
field for field. The set borrows its shapes from
`tests/cabin_contract_test.cpp`'s `Queries()`.

Everything here interleaves block by block across both sides, alternating
which side goes first each block, because two sequential runs of any
driver in this suite disagree with themselves by more than the effect
under test (the pattern `catalog_read_ab_benchmark.py` established).
"""

import argparse
import json
import os
import random
import sys
import time

from bench_common import Phase
from ckdbs_cli import DEFAULT_HOST, ServerConnection, format_reply

COLUMNS = "id int64, val varchar, pad varchar"
MATCHES = 10          # rows per hot value, held (roughly) constant across sizes
HOT_VALUES = 8         # distinct probed values per relation
SIZES_DEFAULT = "200,1000,10000"
SIZE_TAG = {200: "200", 1000: "1k", 10000: "10k"}


def abort(message, reply=None):
    print(f"cabin_scope_ab aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


ECHO = False


def domain_of(rows):
    return max(HOT_VALUES, rows // MATCHES)


def make_rows(rng, rows, domain):
    """(val, pad) per row: row i's val cycles the domain, so every value
    matches ~MATCHES rows at every size."""
    return [(f"v{i % domain:06d}", f"PAD{i:08d}") for i in range(rows)]


def hot_values(rng, domain):
    idx = rng.sample(range(domain), min(HOT_VALUES, domain))
    return [f"v{i:06d}" for i in idx]


class Side:
    """One server: its connection, its relation names, its CPU meter."""

    def __init__(self, label, host, port, pid, suffix, timeout):
        self.label = label
        self.port = port
        self.pid = pid
        try:
            self._conn = ServerConnection(host, port, timeout=timeout)
        except OSError as e:
            abort(f"could not connect to {label} at {host}:{port}: {e}")
        self.suffix = suffix
        self.errors = 0
        self.first_error = None
        self.host = host
        self.timeout = timeout

    def names(self, tag):
        s = self.suffix
        return {
            "ch": f"sbm_ch_{tag}_{s}",
            "cb": f"sbm_cb_{tag}_{s}",
            "uh": f"sbm_uh_{tag}_{s}",
            "ub": f"sbm_ub_{tag}_{s}",
        }

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            print(f"[{self.label}] {command[:90]}  ->  {reply[:110]}",
                  file=sys.stderr, flush=True)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def must(self, command):
        reply = self(command)
        if reply.startswith("ERR"):
            abort(f"{self.label}: {command}", reply)
        return reply

    def cpu_seconds(self):
        if self.pid is None:
            return None
        try:
            with open(f"/proc/{self.pid}/stat") as f:
                fields = f.read().rsplit(") ", 1)[1].split()
        except OSError:
            return None
        ticks = int(fields[11]) + int(fields[12])
        return ticks / os.sysconf("SC_CLK_TCK")

    def close(self):
        self._conn.close()


class Meter:
    def __init__(self):
        self.phases = {}
        self.cpu = {}

    def phase(self, side, arm):
        key = (side.label, arm)
        if key not in self.phases:
            self.phases[key] = Phase(f"{arm}[{side.label}]")
        return self.phases[key]

    def add_cpu(self, side, arm, seconds, ops):
        key = (side.label, arm)
        total_s, total_ops = self.cpu.get(key, (0.0, 0))
        self.cpu[key] = (total_s + seconds, total_ops + ops)


def ordered(sides, block_no):
    return sides if block_no % 2 == 0 else list(reversed(sides))


def latency_pass(sides, meter, arms, block):
    for arm, per_side in arms:
        n = len(next(iter(per_side.values())))
        for i, start in enumerate(range(0, n, block)):
            for side in ordered(sides, i):
                phase = meter.phase(side, arm)
                for stmt in per_side[side.label][start:start + block]:
                    t0 = time.perf_counter()
                    reply = side(stmt)
                    phase.record(time.perf_counter() - t0, reply)


def cpu_pass(sides, meter, arms, rounds):
    for r in range(rounds):
        for arm, per_side in arms:
            for side in ordered(sides, r):
                stmts = per_side[side.label]
                before = side.cpu_seconds()
                for stmt in stmts:
                    side(stmt)
                after = side.cpu_seconds()
                if before is not None and after is not None:
                    meter.add_cpu(side, arm, after - before, len(stmts))


# ---- schema and load -------------------------------------------------------

def build_size(side, tag, rows_data):
    names = side.names(tag)
    side.must(f"CREATE TABLE {names['ch']} ({COLUMNS})")
    side.must(f"CREATE TABLE {names['cb']} ({COLUMNS}) BTREE")
    side.must(f"CREATE TABLE {names['uh']} ({COLUMNS})")
    side.must(f"CREATE TABLE {names['ub']} ({COLUMNS}) BTREE")
    batch = 500
    for rel in (names["ch"], names["cb"], names["uh"], names["ub"]):
        pending = 0
        side.must("BEGIN")
        for i, (val, pad) in enumerate(rows_data):
            side.must(f"INSERT INTO {rel} VALUES ({i + 1}, '{val}', '{pad}')")
            pending += 1
            if pending >= batch:
                side.must("COMMIT")
                side.must("BEGIN")
                pending = 0
        side.must("COMMIT")
    side.must(f"CREATE CABIN ON {names['ch']}(val)")
    side.must(f"CREATE CABIN ON {names['cb']}(val)")
    return names


def warm(side, names, hots):
    """Two untimed probes per hot value per cabined relation: the first
    records (kDeclaredRecordThreshold = 1, still a miss), the second is
    the first hit. Anything after this point in the run is a hit."""
    for rel in (names["ch"], names["cb"]):
        for v in hots:
            side.must(f"SELECT * FROM {rel} WHERE val = '{v}'")
            side.must(f"SELECT * FROM {rel} WHERE val = '{v}'")


def confirm_hit(side, names, hots):
    """ANALYZE one probe per cabined relation and require cabin_hits=1 -
    a run that measured a fallen-through walk under a Cabin heading would
    be measuring nothing."""
    for rel in (names["ch"], names["cb"]):
        reply = side.must(f"ANALYZE SELECT * FROM {rel} WHERE val = '{hots[0]}'")
        if "cabin_hits=1" not in reply:
            abort(f"{side.label}: {rel} did not compile to a Cabin hit after "
                  f"warmup - the run would price a walk, not the serve path",
                  reply)


# ---- arms -------------------------------------------------------------

def arms_for_size(sides, tag, names_by_side, hots_by_side, ops, rng):
    """One arm per shape, plus a repeat of the cabin-heap arm (the in-run
    noise floor) and the two uncabined controls (cannot be affected by
    SB1-SB4 - CabinScopeCovers is reached only from RunCabinStep, which a
    column with no declared Cabin never compiles into)."""
    def picks(seed, n):
        r = random.Random(seed)
        hots = hots_by_side[sides[0].label][tag]
        return [r.choice(hots) for _ in range(n)]

    vals_a = picks(rng.randrange(1 << 30), ops)
    vals_b = picks(rng.randrange(1 << 30), ops)

    def stmts(kind, vals):
        out = {}
        for s in sides:
            rel = names_by_side[s.label][tag][kind]
            out[s.label] = [f"SELECT * FROM {rel} WHERE val = '{v}'" for v in vals]
        return out

    return [
        (f"ping[{tag}]", {s.label: ["SHOW META"] * ops for s in sides}),
        (f"heap-cabin[{tag}]", stmts("ch", vals_a)),
        (f"heap-cabin-again[{tag}]", stmts("ch", vals_b)),
        (f"heap-ctl[{tag}]", stmts("uh", vals_a)),
        (f"btree-cabin[{tag}]", stmts("cb", vals_a)),
        (f"btree-ctl[{tag}]", stmts("ub", vals_a)),
    ]


# ---- verification -----------------------------------------------------

def queries_for(names, hot):
    ch, cb, uh, ub = names["ch"], names["cb"], names["uh"], names["ub"]
    return [
        ("hit-heap", f"SELECT * FROM {ch} WHERE val = '{hot}'"),
        ("hit-btree", f"SELECT * FROM {cb} WHERE val = '{hot}'"),
        ("miss-heap", f"SELECT * FROM {ch} WHERE val = 'zzznomatch'"),
        ("miss-btree", f"SELECT * FROM {cb} WHERE val = 'zzznomatch'"),
        ("extra-conjunct", f"SELECT * FROM {ch} WHERE val = '{hot}' AND id > 1"),
        ("pk-lookup", f"SELECT * FROM {ch} WHERE id = 3"),
        ("count-heap", f"SELECT COUNT(*) FROM {ch}"),
        ("count-btree", f"SELECT COUNT(*) FROM {cb}"),
        ("ctl-heap", f"SELECT * FROM {uh} WHERE val = '{hot}'"),
        ("ctl-btree", f"SELECT * FROM {ub} WHERE val = '{hot}'"),
        ("self-join",
         f"SELECT x.id, y.id FROM {cb} AS x JOIN {cb} AS y ON y.val = x.val "
         f"WHERE x.id = 3"),
    ]


def verify(sides, names_by_side, hots_by_side, tags):
    problems = []
    for tag in tags:
        hot = hots_by_side[sides[0].label][tag][0]
        qa = queries_for(names_by_side[sides[0].label][tag], hot)
        for name, sql in qa:
            replies = {s.label: s(sql) for s in sides}
            if len(set(replies.values())) != 1:
                problems.append(f"[{tag}] {name} differs across sides: "
                                f"{ {k: v[:200] for k, v in replies.items()} }")
    for side in sides:
        if side.errors:
            problems.append(f"{side.label}: {side.errors} error replies, "
                            f"first {side.first_error}")
    return problems


# ---- reporting ----------------------------------------------------------

def table(meter, sides, arms):
    labels = [s.label for s in sides]
    width = max(len(a) for a in arms) + 2
    header = (f"{'arm':<{width}}{'side':<8}{'ops':>7}{'mean':>9}{'p0':>8}"
              f"{'p25':>8}{'p50':>8}{'p95':>9}{'p99':>9}{'max':>10}{'tps':>10}{'err':>5}")
    print(header)
    print("-" * len(header))
    for arm in arms:
        for label in labels:
            phase = meter.phases.get((label, arm))
            if phase is None or phase.ops == 0:
                continue
            s = phase.summary()
            tps = 1e6 / s["mean_us"] if s["mean_us"] else 0.0
            print(f"{arm:<{width}}{label:<8}{s['ops']:>7}{s['mean_us']:>9.1f}"
                  f"{s['p0_us']:>8.1f}{s['p25_us']:>8.1f}{s['p50_us']:>8.1f}"
                  f"{s['p95_us']:>9.1f}{s['p99_us']:>9.1f}{s['max_us']:>10.1f}"
                  f"{tps:>10,.0f}{s['errors']:>5}")
        if len(labels) == 2:
            a = meter.phases.get((labels[0], arm))
            b = meter.phases.get((labels[1], arm))
            if a and b and a.ops and b.ops:
                sa, sb = a.summary(), b.summary()
                print(f"{'':<{width}}{'delta':<8}{'':>7}"
                      f"{sb['mean_us'] - sa['mean_us']:>+9.1f}"
                      f"{sb['p0_us'] - sa['p0_us']:>+8.1f}"
                      f"{sb['p25_us'] - sa['p25_us']:>+8.1f}"
                      f"{sb['p50_us'] - sa['p50_us']:>+8.1f}"
                      f"{sb['p95_us'] - sa['p95_us']:>+9.1f}"
                      f"{sb['p99_us'] - sa['p99_us']:>+9.1f}"
                      f"{'':>10}{'':>10}{'':>5}")
        print()


def cpu_table(meter, sides):
    if not meter.cpu:
        print("  no server CPU: pass --server-pid / --ab-server-pid")
        return
    arms = []
    for (_, arm) in meter.cpu:
        if arm not in arms:
            arms.append(arm)
    width = max(len(a) for (_, a) in meter.cpu) + 2
    header = f"{'arm':<{width}}{'side':<8}{'ops':>9}{'cpu us/op':>12}"
    print(header)
    print("-" * len(header))
    for arm in arms:
        per = {}
        for side in sides:
            entry = meter.cpu.get((side.label, arm))
            if entry is None or entry[1] == 0:
                continue
            per[side.label] = entry[0] / entry[1] * 1e6
            print(f"{arm:<{width}}{side.label:<8}{entry[1]:>9}"
                  f"{per[side.label]:>12.2f}")
        if len(per) == 2:
            a, b = [per[s.label] for s in sides]
            print(f"{'':<{width}}{'delta':<8}{'':>9}{b - a:>+12.2f}")
        print()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=15922,
                   help="side B - post-SB, the engine under test")
    p.add_argument("--label", default="B")
    p.add_argument("--server-pid", type=int, default=None)
    p.add_argument("--ab-port", type=int, default=15921,
                   help="side A - pre-SB baseline, on a second binary")
    p.add_argument("--ab-label", default="A")
    p.add_argument("--ab-server-pid", type=int, default=None)
    p.add_argument("--rows", default=SIZES_DEFAULT,
                   help="comma-separated row-set sizes, swept in one run")
    p.add_argument("--ops", type=int, default=3000, help="ops per read arm, per size")
    p.add_argument("--block", type=int, default=150)
    p.add_argument("--cpu-rounds", type=int, default=4)
    p.add_argument("--cpu-ops", type=int, default=1500)
    p.add_argument("--suffix", default=None)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--json", metavar="PATH")
    p.add_argument("--echo", action="store_true")
    p.add_argument("--no-verify", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="run even on a busy box")
    args = p.parse_args()

    global ECHO
    ECHO = args.echo

    load1, load5 = [float(x) for x in open("/proc/loadavg").read().split()[:2]]
    if (load1 > 1.5 or load5 > 2.0) and not args.force:
        abort(f"host is not quiet: load average {load1} / {load5}. --force to "
              f"override")
    if os.popen("pgrep -c cc1plus 2>/dev/null").read().strip() not in ("", "0") \
            and not args.force:
        abort("a C++ compile is running on this box")

    suffix = args.suffix or f"{int(time.time())}"
    sizes = [int(x) for x in args.rows.split(",")]
    tags = [SIZE_TAG.get(sz, str(sz)) for sz in sizes]

    sides = [Side(args.ab_label, args.host, args.ab_port, args.ab_server_pid,
                  suffix, args.timeout),
             Side(args.label, args.host, args.port, args.server_pid,
                  suffix, args.timeout)]

    versions = {s.label: s.must("SHOW META").replace("\n", " | ") for s in sides}

    names_by_side = {s.label: {} for s in sides}
    hots_by_side = {s.label: {} for s in sides}

    build_started = time.perf_counter()
    for sz, tag in zip(sizes, tags):
        rows_data = make_rows(random.Random(args.seed), sz, domain_of(sz))
        hots = hot_values(random.Random(args.seed + sz), domain_of(sz))
        for side in sides:
            names = build_size(side, tag, rows_data)
            names_by_side[side.label][tag] = names
            hots_by_side[side.label][tag] = hots
            warm(side, names, hots)
            confirm_hit(side, names, hots)
    build_elapsed = time.perf_counter() - build_started

    meter = Meter()
    rng = random.Random(args.seed + 100)
    started = time.perf_counter()
    all_arms = []
    for tag in tags:
        arms = arms_for_size(sides, tag, names_by_side, hots_by_side, args.ops, rng)
        all_arms.extend(arms)
        latency_pass(sides, meter, arms, args.block)
    latency_elapsed = time.perf_counter() - started

    cpu_started = time.perf_counter()
    cpu_rng = random.Random(args.seed + 200)
    cpu_arms = []
    for tag in tags:
        arms = arms_for_size(sides, tag, names_by_side, hots_by_side, args.cpu_ops,
                             cpu_rng)
        cpu_arms.extend(arms)
    cpu_pass(sides, meter, cpu_arms, args.cpu_rounds)
    cpu_elapsed = time.perf_counter() - cpu_started

    arm_names = [a for a, _ in all_arms]

    fallthroughs = {}
    for side in sides:
        meta = side.must("SHOW META")
        fallthroughs[side.label] = "cabin_scope_fallthroughs=" in meta

    print()
    print(f"cabin_scope_ab - sizes {sizes}, {args.ops} ops/arm, suffix {suffix}, "
          f"build {build_elapsed:.1f}s")
    for side in sides:
        print(f"  {side.label}: port {side.port}, pid {side.pid}, "
              f"SHOW META carries cabin_scope_fallthroughs="
              f"{fallthroughs[side.label]}")
    print(f"  load average at start {load1} / {load5}; latency pass "
          f"{latency_elapsed:.1f}s, cpu pass {cpu_elapsed:.1f}s")
    print()
    table(meter, sides, arm_names)
    print(f"server CPU per operation (contiguous windows, {args.cpu_rounds} rounds)")
    print()
    cpu_table(meter, sides)

    problems = []
    if not args.no_verify:
        problems = verify(sides, names_by_side, hots_by_side, tags)
        if problems:
            print("VERIFY FAILED")
            for line in problems:
                print(f"  {line}")
        else:
            print(f"verify: ok - {len(tags)} sizes, "
                  f"{len(queries_for(names_by_side[sides[0].label][tags[0]], 'x'))} "
                  f"queries each, byte-identical across both sides")

    if args.json:
        payload = {
            "meta": {
                "driver": "cabin_scope_ab_benchmark.py",
                "sizes": sizes,
                "ops": args.ops,
                "cpu_ops": args.cpu_ops,
                "cpu_rounds": args.cpu_rounds,
                "block": args.block,
                "suffix": suffix,
                "seed": args.seed,
                "sides": [{"label": s.label, "port": s.port, "pid": s.pid}
                          for s in sides],
                "show_meta": versions,
                "cabin_scope_fallthroughs_present": fallthroughs,
                "loadavg_start": [load1, load5],
                "verify_problems": problems,
                "build_elapsed_s": build_elapsed,
            },
            "phases": [meter.phases[k].summary() for k in meter.phases],
            "cpu": [{"side": k[0], "arm": k[1], "seconds": v[0], "ops": v[1],
                     "us_per_op": v[0] / v[1] * 1e6 if v[1] else None}
                    for k, v in meter.cpu.items()],
        }
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"  wrote {args.json}")

    for side in sides:
        side.close()
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
