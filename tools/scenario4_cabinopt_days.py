#!/usr/bin/env python3
"""Business-days scenario for the cabin optimizer: a rotating hot set over N
simulated trading days, `cabin_optimizer` off / on / statically declared.

`bench/results-cabin-optimizer.md` (PHY08's single-shot cases) measured the
controller's CREATE and the served win, and recorded its honest tail: the
lifecycle past CREATE - DECAYING, DROP, re-nomination - was never exercised
under real traffic. This scenario exercises exactly that. The workload is a
compressed market week:

    per day:  open     an insert burst into the day's hot board
              trading  a paced session of hot non-pk equality probes,
                       skewed onto that day's hot symbols, plus pk-lookup
                       controls - equal work per arm, wall-paced in slots so
                       the controller's decay clock gets a realistic session
              close    full-scan aggregates (COUNT/SUM/GROUP BY)
              night    an idle gap, the on-arm polled for state transitions

The **hot set rotates two ways**, deliberately, because the controller's
managed unit is a `(relation, column)` shape, not a value:

    boards   day 1 concentrates on `board_a.symbol`, day 2 on
             `board_b.symbol`, day 3 on `board_a` again with a *different*
             hot-symbol set. A whole shape goes cold overnight - this is
             what can exercise ACTIVE -> DECAYING -> DROP, and day 3's
             return is the re-nomination path (PO5: "re-nomination starts
             from scratch as CANDIDATE").
    tapes    `tape_200` / `tape_1k` / `tape_10k` (the row-set sweep, rule 9)
             are probed every day, but the hot *values* rotate disjointly
             per day. The shape stays hot, so the per-column Cabin should
             persist and pay only per-value re-observation (n=2) - value
             rotation is NOT expected to trigger a DROP, and measuring that
             distinction is half the point of the scenario.

Arms - one server process and one fresh disk-backed data file each, all on
identical configs, interleaved per *block* inside every phase so machine
drift costs each arm the same wall time:

    off       cabins exist as a feature but none is created; the optimizer
              is never enabled. Pure walks - the baseline.
    on        `SET CABIN_OPTIMIZER ON` at day 1 open; the controller
              observes, CREATEs, serves, DECAYs, DROPs on its own.
    declared  the operator declares `CREATE CABIN` on every board and tape
              symbol column up front, optimizer off - the static ceiling
              the autonomous arm is judged against, and the arm that cannot
              retire anything when the hot set rotates.

Time compression is explicit and must be stated in the results document:
the servers run `decay_half_life` lowered from its 600 s default (the
driver does not set it - the server config does; this driver's results
were taken at 5 s, a 120x compression) and `cabin_optimizer_snapshot_
interval_ms` lowered from 10,000 to 500. Under 120x, one simulated hour is
30 wall seconds; the default `--session-seconds 45` is then a 1.5-hour
condensed session and `--overnight-seconds 45` a 1.5-hour night - a
shortened night, chosen because what the lifecycle rules consume is cold
time *in half-lives* (DECAYING at B < theta_drop x C, DROP after a
2-half-life cooldown), and 45 s is already 9 half-lives.

Evidence captured untimed at every phase boundary and every 3 s overnight:
`SHOW CABIN_OPTIMIZER` (states, creates/extends/heals/drops counters, the
budget line, per-entry B/C) on the on arm, `SHOW CABINS` (hits/misses per
cabin) on the on and declared arms, and `ANALYZE` of the hot probes at each
trading close. The correctness guard runs per day: an identical
verification set (hot probes, a zero-row probe, COUNT/SUM) is executed on
every arm and the replies are compared **byte for byte** - a Cabin, however
it was created, chooses where to look, never what is visible.

Wants three servers started on fresh data files under a real block device
(never tmpfs), e.g.:

    for arm in off on declared; do
        ./build-release/kds_server ~/bench-cabinopt-days/$arm.db \
            --port <port> --config <conf-with-lowered-halflife> &
    done
    python3 tools/scenario4_cabinopt_days.py \
        --port-off 15651 --port-on 15652 --port-declared 15653 \
        --pid-off <pid> --pid-on <pid> --pid-declared <pid> --json out.json

Rerunning against the same data files needs `--suffix`; nothing here drops
tables. The PostgreSQL twin is `tools/pg_scenario4_cabinopt_days.py`.
"""

import argparse
import datetime
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase, report
from ckdbs_cli import DEFAULT_HOST, ServerConnection, format_reply

# ---- the shared shape (the PostgreSQL twin imports these) -----------------
#
#   id      the Keystone pk, system-generated (invariant 11)
#   symbol  the hot non-pk equality column - no index, no declared policy,
#           which reads as CABIN AUTO: the optimizer's jurisdiction
#   qty     an int64 the close-phase SUM folds
#   price   int64 minor units (cents) - money is never a float here
COLUMNS = "id int64, symbol varchar, qty int64, price int64"
CLUSTERED = "BTREE"

MATCHES = 10          # rows per symbol at load: domain = rows / 10
BOARD_ROWS = 10000    # both boards; the size where the walk visibly pays
TAPE_SIZES = (200, 1000, 10000)
TAPE_TAGS = {200: "200", 1000: "1k", 10000: "10k"}

HOT_BOARD_SYMBOLS = 8   # per day
HOT_TAPE_VALUES = 6     # per day, disjoint slices of the first 18 symbols
BOARD_HOT_SHARE = 0.8   # trading draws: 80% hot, 20% uniform
TAPE_HOT_SHARE = 0.85

ZERO_ROW_SYMBOL = "ZZZZ99"  # never inserted anywhere: the zero-row probe

ARMS = ("off", "on", "declared")


def board_symbol(board, i):
    return f"{board.upper()}{i:04d}"     # A0007 / B0007


def tape_symbol(i):
    return f"T{i:04d}"


def make_board_rows(board, rows, rng):
    """(symbol, qty, price) per row; symbol i%domain like the PHY08 driver,
    so every symbol matches exactly MATCHES rows at load."""
    domain = rows // MATCHES
    return [(board_symbol(board, i % domain), rng.randrange(1, 1000),
             rng.randrange(100, 100000)) for i in range(rows)]


def make_tape_rows(rows, rng):
    domain = rows // MATCHES
    return [(tape_symbol(i % domain), rng.randrange(1, 1000),
             rng.randrange(100, 100000)) for i in range(rows)]


def probe_sql(table, symbol):
    return f"SELECT * FROM {table} WHERE symbol = '{symbol}'"


def pk_sql(table, key):
    return f"SELECT * FROM {table} WHERE id = {key}"


def insert_sql(table, row):
    symbol, qty, price = row
    return f"INSERT INTO {table} VALUES ('{symbol}', {qty}, {price})"


# ---- the day plan ---------------------------------------------------------

def build_day_plans(args, rng):
    """Every draw for every day, made once and replayed identically on every
    arm (equal work, identical statements - the interleave rule)."""
    boards = ["a", "b"]
    rotation = [boards[d % 2] for d in range(args.days)]     # a, b, a, ...
    domain = BOARD_ROWS // MATCHES

    # Disjoint hot sets per board across its hot days: one shuffle per
    # board, consumed in slices, so board_a's day-3 hot symbols share
    # nothing with its day-1 set.
    shuffled = {b: rng.sample(range(domain), domain) for b in boards}
    used = {b: 0 for b in boards}

    plans = []
    for day in range(args.days):
        board = rotation[day]
        lo = used[board]
        hot_idx = shuffled[board][lo:lo + HOT_BOARD_SYMBOLS]
        used[board] += HOT_BOARD_SYMBOLS
        hot_syms = [board_symbol(board, i) for i in hot_idx]

        t0 = (day * HOT_TAPE_VALUES) % 18
        hot_tape = [tape_symbol(t0 + i) for i in range(HOT_TAPE_VALUES)]

        open_rows = []
        for _ in range(args.open_inserts):
            if rng.random() < 0.6:
                sym = rng.choice(hot_syms)
            else:
                sym = board_symbol(board, rng.randrange(domain))
            open_rows.append((sym, rng.randrange(1, 1000),
                              rng.randrange(100, 100000)))

        blocks = []
        board_per = args.board_probes // args.blocks
        tape_per = args.tape_probes // args.blocks
        pk_per = args.pk_ops // args.blocks
        for _ in range(args.blocks):
            bdraw = [rng.choice(hot_syms) if rng.random() < BOARD_HOT_SHARE
                     else board_symbol(board, rng.randrange(domain))
                     for _ in range(board_per)]
            tdraw = {}
            for size in TAPE_SIZES:
                tdomain = size // MATCHES
                tdraw[size] = [rng.choice(hot_tape)
                               if rng.random() < TAPE_HOT_SHARE
                               else tape_symbol(rng.randrange(tdomain))
                               for _ in range(tape_per)]
            pkdraw = [rng.randrange(1, BOARD_ROWS + 1) for _ in range(pk_per)]
            blocks.append({"board": bdraw, "tape": tdraw, "pk": pkdraw})

        plans.append({"day": day + 1, "board": board, "hot_syms": hot_syms,
                      "hot_tape": hot_tape, "open_rows": open_rows,
                      "blocks": blocks})
    return plans


# ---- plumbing -------------------------------------------------------------

def abort(message, reply=None):
    print(f"scenario4_cabinopt_days aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def server_cpu_seconds(pid):
    """utime+stime of one server process (docs/inflight/in-progress/workplan-aggregate-perf.md's
    meter: client latency cannot resolve server fixed costs). Resolution is
    one jiffy per read; per-block sums over a session stay within ~10%."""
    if not pid:
        return 0.0
    with open(f"/proc/{pid}/stat") as f:
        after_comm = f.read().rsplit(") ", 1)[1].split()
    return (int(after_comm[11]) + int(after_comm[12])) / os.sysconf("SC_CLK_TCK")


class Arm:
    """One arm: a name, a connection, and its accumulators."""

    def __init__(self, name, host, port, pid, timeout):
        self.name = name
        self.pid = pid
        try:
            self.conn = ServerConnection(host, port, timeout=timeout)
        except OSError as e:
            abort(f"arm '{name}': could not connect to {host}:{port}: {e}")
        self.errors = 0
        self.first_error = None
        self.cpu = {}          # phase-group key -> seconds

    def __call__(self, command):
        reply = format_reply(self.conn.send_command(command))
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def timed(self, command, phase):
        t0 = time.perf_counter()
        reply = self(command)
        dt = time.perf_counter() - t0
        phase.record(dt, reply)
        phase.elapsed += dt          # busy time only; slot sleeps excluded
        return reply

    def add_cpu(self, key, seconds):
        self.cpu[key] = self.cpu.get(key, 0.0) + seconds


def parse_kv(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
    return out


def capture_optimizer(arm):
    reply = arm("SHOW CABIN_OPTIMIZER")
    lines = reply.splitlines()
    return {"header": lines[0],
            "entries": [ln for ln in lines[1:] if ln.strip()]}


def capture_cabins(arm):
    reply = arm("SHOW CABINS")
    return [ln for ln in reply.splitlines() if ln.strip()]


# ---- setup ----------------------------------------------------------------

def table_names(suffix):
    names = {"board_a": f"board_a_{suffix}", "board_b": f"board_b_{suffix}"}
    for size in TAPE_SIZES:
        names[f"tape_{TAPE_TAGS[size]}"] = f"tape_{TAPE_TAGS[size]}_{suffix}"
    return names


def load_arm(arm, names, args, load_phase):
    """Identical DDL + insert sequence on every arm: same seed, same order,
    so the three servers hold byte-identical user relations and the same
    system-generated ids."""
    rng = random.Random(args.seed)     # one fresh generator per arm
    for key in ("board_a", "board_b"):
        reply = arm(f"CREATE TABLE {names[key]} ({COLUMNS}) {CLUSTERED}")
        if reply.startswith("ERR"):
            abort(f"CREATE TABLE {names[key]} failed on {arm.name}", reply)
    for size in TAPE_SIZES:
        key = f"tape_{TAPE_TAGS[size]}"
        reply = arm(f"CREATE TABLE {names[key]} ({COLUMNS}) {CLUSTERED}")
        if reply.startswith("ERR"):
            abort(f"CREATE TABLE {names[key]} failed on {arm.name}", reply)

    def batch_insert(table, rows):
        pending = 0
        arm("BEGIN")
        for row in rows:
            arm.timed(insert_sql(table, row), load_phase)
            pending += 1
            if pending >= 500:
                arm("COMMIT")
                arm("BEGIN")
                pending = 0
        arm("COMMIT")

    batch_insert(names["board_a"], make_board_rows("a", BOARD_ROWS, rng))
    batch_insert(names["board_b"], make_board_rows("b", BOARD_ROWS, rng))
    for size in TAPE_SIZES:
        batch_insert(names[f"tape_{TAPE_TAGS[size]}"],
                     make_tape_rows(size, rng))


def declare_cabins(arm, names):
    for key in sorted(names):
        reply = arm(f"CREATE CABIN ON {names[key]}(symbol)")
        if not reply.startswith("CREATED CABIN"):
            abort(f"CREATE CABIN on {names[key]} failed on {arm.name}", reply)


# ---- the run --------------------------------------------------------------

def run_days(args, arms, names, plans, run_t0):
    on_arm = next((a for a in arms if a.name == "on"), None)
    decl_arm = next((a for a in arms if a.name == "declared"), None)

    phases = []
    evidence = []       # the lifecycle event stream, in wall order
    verify_results = []
    overruns = 0

    def note(day, tag):
        ev = {"t_wall_s": round(time.time() - run_t0, 1), "day": day,
              "tag": tag}
        if on_arm:
            ev["on"] = capture_optimizer(on_arm)
        evidence.append(ev)
        return ev

    if on_arm:
        reply = on_arm("SET CABIN_OPTIMIZER ON")
        if not reply.startswith("OK"):
            abort("SET CABIN_OPTIMIZER ON refused", reply)

    for plan in plans:
        day = plan["day"]
        board_tbl = names[f"board_{plan['board']}"]
        hot_syms = plan["hot_syms"]

        note(day, "day-open")

        # ---- open: the insert burst, interleaved in 4 blocks -------------
        open_phases = {a.name: Phase(f"d{day}-open[{a.name}]",
                                     f"{args.open_inserts} inserts, "
                                     f"{board_tbl}") for a in arms}
        rows = plan["open_rows"]
        step = max(1, len(rows) // 4)
        for lo in range(0, len(rows), step):
            chunk = rows[lo:lo + step]
            for arm in arms:
                c0 = server_cpu_seconds(arm.pid)
                for row in chunk:
                    arm.timed(insert_sql(board_tbl, row),
                              open_phases[arm.name])
                arm.add_cpu((day, "open"), server_cpu_seconds(arm.pid) - c0)
        note(day, "after-open")

        # ---- trading: the paced session ----------------------------------
        pgroup = {}
        for a in arms:
            pgroup[(a.name, "board")] = Phase(
                f"d{day}-board[{a.name}]",
                f"hot equality on {board_tbl}, {HOT_BOARD_SYMBOLS} hot syms")
            for size in TAPE_SIZES:
                pgroup[(a.name, f"tape{TAPE_TAGS[size]}")] = Phase(
                    f"d{day}-tape{TAPE_TAGS[size]}[{a.name}]",
                    f"{size} rows, rotating hot values")
            pgroup[(a.name, "pk")] = Phase(
                f"d{day}-pk[{a.name}]", "control: touches no Cabin")

        slot = args.session_seconds / args.blocks
        session_t0 = time.perf_counter()
        for bi, block in enumerate(plan["blocks"]):
            for arm in arms:
                c0 = server_cpu_seconds(arm.pid)
                ph = pgroup[(arm.name, "board")]
                for sym in block["board"]:
                    arm.timed(probe_sql(board_tbl, sym), ph)
                arm.add_cpu((day, "board"), server_cpu_seconds(arm.pid) - c0)
                c0 = server_cpu_seconds(arm.pid)
                for size in TAPE_SIZES:
                    ph = pgroup[(arm.name, f"tape{TAPE_TAGS[size]}")]
                    tbl = names[f"tape_{TAPE_TAGS[size]}"]
                    for sym in block["tape"][size]:
                        arm.timed(probe_sql(tbl, sym), ph)
                arm.add_cpu((day, "tape"), server_cpu_seconds(arm.pid) - c0)
                ph = pgroup[(arm.name, "pk")]
                for key in block["pk"]:
                    arm.timed(pk_sql(board_tbl, key), ph)
            if bi % 3 == 2:
                note(day, f"trading-block-{bi + 1}")
            deadline = session_t0 + (bi + 1) * slot
            remain = deadline - time.perf_counter()
            if remain > 0:
                time.sleep(remain)
            else:
                overruns += 1

        ev = note(day, "after-trading")
        # ANALYZE of the day's hot probes, per arm, untimed: the plan-shape
        # evidence (FilterScan pages= against CabinProbe cabin_optimizer=).
        ev["analyze"] = {
            a.name: {"board": a(f"ANALYZE {probe_sql(board_tbl, hot_syms[0])}"),
                     "tape10k": a(f"ANALYZE {probe_sql(names['tape_10k'], plan['hot_tape'][0])}")}
            for a in arms}
        if decl_arm:
            ev["declared_cabins"] = capture_cabins(decl_arm)
        if on_arm:
            ev["on_cabins"] = capture_cabins(on_arm)

        # ---- close: full-scan aggregates ---------------------------------
        close_phases = {a.name: Phase(f"d{day}-close[{a.name}]",
                                      "COUNT/SUM/GROUP BY full scans")
                        for a in arms}
        close_set = [f"SELECT COUNT(*) FROM {board_tbl}",
                     f"SELECT SUM(qty) FROM {board_tbl}",
                     f"SELECT symbol, COUNT(*) FROM {board_tbl} GROUP BY symbol",
                     f"SELECT COUNT(*) FROM {names['tape_10k']}"]
        for _ in range(args.close_rounds):
            for arm in arms:
                c0 = server_cpu_seconds(arm.pid)
                for sql in close_set:
                    arm.timed(sql, close_phases[arm.name])
                arm.add_cpu((day, "close"), server_cpu_seconds(arm.pid) - c0)

        # ---- the correctness guard: byte-identical replies ---------------
        verify_set = [probe_sql(board_tbl, s) for s in hot_syms]
        verify_set += [probe_sql(names[f"tape_{TAPE_TAGS[size]}"], v)
                       for size in TAPE_SIZES
                       for v in plan["hot_tape"][:2]]
        verify_set += [probe_sql(board_tbl, ZERO_ROW_SYMBOL),
                       f"SELECT COUNT(*) FROM {names['board_a']}",
                       f"SELECT COUNT(*) FROM {names['board_b']}",
                       f"SELECT SUM(qty) FROM {board_tbl}"]
        mismatches = []
        for sql in verify_set:
            replies = {arm.name: arm(sql) for arm in arms}
            base = replies[arms[0].name]
            for other in arms[1:]:
                if replies[other.name] != base:
                    mismatches.append({"sql": sql, "arms":
                                       {arms[0].name: base,
                                        other.name: replies[other.name]}})
        verify_results.append({"day": day, "statements": len(verify_set),
                               "compared_arms": [a.name for a in arms],
                               "mismatches": mismatches})
        if mismatches:
            print(f"VERIFY FAILED day {day}: {len(mismatches)} statements "
                  f"differ between arms - a Cabin changed a reply",
                  file=sys.stderr)

        note(day, "after-close")

        # ---- overnight: idle, the on arm polled for transitions ----------
        night_t0 = time.perf_counter()
        last_states = None
        while time.perf_counter() - night_t0 < args.overnight_seconds:
            time.sleep(min(3.0, max(0.1, args.overnight_seconds -
                                    (time.perf_counter() - night_t0))))
            if on_arm:
                cap = capture_optimizer(on_arm)
                states = tuple(sorted(
                    (parse_kv(e).get("rel", "?"), parse_kv(e).get("state", "?"))
                    for e in cap["entries"]))
                header_drops = parse_kv(cap["header"]).get("drops")
                key = (states, header_drops)
                if key != last_states:
                    evidence.append({"t_wall_s": round(time.time() - run_t0, 1),
                                     "day": day, "tag": "overnight", "on": cap})
                    last_states = key

        day_phases = [open_phases[a.name] for a in arms]
        day_phases += [pgroup[(a.name, g)] for a in arms
                       for g in ("board", "tape200", "tape1k", "tape10k", "pk")]
        day_phases += [close_phases[a.name] for a in arms]
        phases += day_phases

    return phases, evidence, verify_results, overruns


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port-off", type=int, default=15651)
    ap.add_argument("--port-on", type=int, default=15652)
    ap.add_argument("--port-declared", type=int, default=0,
                    help="0 skips the declared arm")
    ap.add_argument("--pid-off", type=int, default=0)
    ap.add_argument("--pid-on", type=int, default=0)
    ap.add_argument("--pid-declared", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--suffix", default="a")
    ap.add_argument("--seed", type=int, default=20260810)
    ap.add_argument("--days", type=int, default=3)
    ap.add_argument("--blocks", type=int, default=12,
                    help="interleave blocks per trading session")
    ap.add_argument("--session-seconds", type=float, default=45.0,
                    help="paced wall length of one trading session")
    ap.add_argument("--overnight-seconds", type=float, default=45.0)
    ap.add_argument("--open-inserts", type=int, default=240)
    ap.add_argument("--board-probes", type=int, default=2400,
                    help="hot-board probes per arm per day")
    ap.add_argument("--tape-probes", type=int, default=396,
                    help="probes per tape size per arm per day")
    ap.add_argument("--pk-ops", type=int, default=240)
    ap.add_argument("--close-rounds", type=int, default=3)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    arm_ports = [("off", args.port_off, args.pid_off),
                 ("on", args.port_on, args.pid_on)]
    if args.port_declared:
        arm_ports.append(("declared", args.port_declared, args.pid_declared))
    arms = [Arm(name, args.host, port, pid, args.timeout)
            for name, port, pid in arm_ports]

    names = table_names(args.suffix)
    plan_rng = random.Random(args.seed + 1)
    plans = build_day_plans(args, plan_rng)

    run_t0 = time.time()
    load_phases = []
    for arm in arms:
        lp = Phase(f"load[{arm.name}]", "setup, txn batches of 500")
        load_arm(arm, names, args, lp)
        load_phases.append(lp)
    decl_arm = next((a for a in arms if a.name == "declared"), None)
    if decl_arm:
        declare_cabins(decl_arm, names)

    phases, evidence, verify_results, overruns = \
        run_days(args, arms, names, plans, run_t0)

    # Final state, every arm: the off arm's ticks=0 line is the null
    # evidence; the on arm's counters are the lifecycle ledger.
    final = {a.name: capture_optimizer(a) for a in arms}
    for a in arms:
        final[a.name]["cabins"] = capture_cabins(a)

    meta = {
        "engine": "ckdbs",
        "driver": "scenario4_cabinopt_days.py",
        "host": args.host,
        "port": "/".join(str(p) for _, p, _ in arm_ports),
        "columns": COLUMNS, "clustered": CLUSTERED,
        "rows": f"boards 2x{BOARD_ROWS}, tapes {'/'.join(map(str, TAPE_SIZES))}",
        "table": f"board_*/{'tape_*'}_{args.suffix}",
        "arms": [a.name for a in arms],
        "days": args.days, "seed": args.seed, "suffix": args.suffix,
        "session_seconds": args.session_seconds,
        "overnight_seconds": args.overnight_seconds,
        "slot_overruns": overruns,
        "started_utc": datetime.datetime.utcfromtimestamp(run_t0)
        .strftime("%Y-%m-%d %H:%M:%S"),
        "elapsed_s": round(time.time() - run_t0, 1),
        "client_errors": sum(a.errors for a in arms),
        "first_error": next((a.first_error for a in arms if a.first_error),
                            None),
    }
    report(load_phases + phases, meta)

    # Per-arm-per-day TPS over busy time (the sessions are wall-paced, so
    # wall TPS would measure the pacing, not the engine).
    print("per-arm busy-time TPS (timed ops / summed latencies):")
    by_arm_day = {}
    for ph in phases:
        day = int(ph.name.split("-")[0][1:])
        arm = ph.name.split("[")[1].rstrip("]")
        ops, busy = by_arm_day.get((arm, day), (0, 0.0))
        by_arm_day[(arm, day)] = (ops + ph.ops, busy + sum(ph.latencies))
    for (arm, day), (ops, busy) in sorted(by_arm_day.items()):
        print(f"  day {day} [{arm:9s}]  ops={ops:6d}  busy={busy:7.2f}s  "
              f"tps={ops / busy if busy else 0:8.0f}")

    verdicts = ["PASS" if not v["mismatches"] else "FAIL"
                for v in verify_results]
    print(f"\nverify (byte-identical replies across arms, per day): "
          f"{' '.join(verdicts)}")

    if args.json:
        payload = {
            "meta": meta,
            "phases": [p.summary() for p in load_phases + phases],
            "server_cpu_s": {a.name: {f"d{d}-{g}": round(v, 4)
                                      for (d, g), v in sorted(a.cpu.items())}
                             for a in arms},
            "evidence": evidence,
            "verify": verify_results,
            "final": final,
            "plans": [{k: v for k, v in p.items() if k != "open_rows"}
                      | {"open_inserts": len(p["open_rows"])}
                      for p in plans],
        }
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2, default=str)
        print(f"json -> {args.json}")

    errors = sum(a.errors for a in arms)
    if errors:
        print(f"\nERRORS: {errors}, first: "
              f"{next(a.first_error for a in arms if a.first_error)}",
              file=sys.stderr)
    failed = errors or any(v["mismatches"] for v in verify_results)
    for arm in arms:
        arm.conn.close()
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
