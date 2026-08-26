#!/usr/bin/env python3
"""The cabin optimizer, priced: the null overhead and the self-created win.

`docs/workplan-physical-optimizer.md` PHY08 asks for two measurements and
this driver runs both, against a Release server on a block-device data file:

  null     What `cabin_optimizer = on` costs a workload with **zero eligible
           candidates** - pk lookups (kLookup steps carry no candidate
           column) and INSERTs (not fingerprint-tracked). Target:
           unmeasurable, snapshot cost only. Three interleaved arms driven
           through PO8's runtime switch on ONE server and ONE data file -
           `off1`, `on`, `off2`, flipped with `SET CABIN_OPTIMIZER` between
           blocks every round - so machine drift costs every arm equally and
           the off1/off2 gap is the noise floor. The tick itself is then
           priced from server CPU (`/proc/<pid>/stat`) over two idle
           windows, on and off, divided by the tick count `SHOW
           CABIN_OPTIMIZER` reports - client latency cannot resolve a
           background task, which is `docs/inflight/in-progress/workplan-aggregate-perf.md`'s
           rule applied to a cost that is not even per-statement.

  improve  A hot non-pk equality (`SELECT * FROM t WHERE val = 7`) probed
           repeatedly over BTREE relations of 200 / 1,000 / 10,000 rows,
           with the controller creating the Cabin mid-run. Phases per size:
           `walk` (fixed ops, optimizer OFF - the pre-Cabin baseline),
           `transition` (SET ON, probes continue while the controller
           snapshots -> Decides -> CREATEs; the driver polls `SHOW
           CABIN_OPTIMIZER` between blocks and records time-to-create),
           `served` (fixed ops after the Cabin serves). Equal work, not
           equal time: walk and served are the same op count. The value
           domain scales with rows (rows/10 values) so the hot value
           matches exactly 10 rows at every size - the axis is relation
           size, not answer size. Evidence captured untimed: the managed
           entry line of `SHOW CABIN_OPTIMIZER` (state, benefit_q16 /
           cost_q16, counters), `ANALYZE <probe>` before (`pages=` of the
           walk) and after (`cabin_hits=1 cabin_optimizer=true`), and
           `SHOW CABINS` hits/misses for the hit rate. A pk lookup on the
           same relation runs before and after as the in-run control: it
           touches no Cabin and must not move.

           The auto observation rule is n=2 per value: after the CREATE the
           first two probes of a value still walk (the second records), and
           the third is served. The transition phase absorbs that; the
           driver confirms serving via `SHOW CABINS` hits before starting
           the `served` phase.

           `--verify` (default on) compares one probe reply captured in the
           walk phase against one from the served phase, row for row
           including order - a Cabin that changes a reply is a bug, not a
           result.

Server configs the two cases expect (see bench/docs/README.md for the exact
invocations): both want `cabins = on` (default) and a lowered
`cabin_optimizer_snapshot_interval_ms` - the null case low (e.g. 10ms) so
the idle windows accumulate enough ticks to resolve, the improve case
moderate (e.g. 500ms) so time-to-create is visible in seconds. Both boot
with `cabin_optimizer = off` and let the driver drive PO8's runtime switch;
the improve case's measured `served` phase runs with the switch ON, as the
workplan's improvement case asks.

The PostgreSQL twin is `tools/pg_cabin_optimizer_benchmark.py`, which
imports the schema, the row generator and the probe from this file. It has
no optimizer to switch: at defaults PostgreSQL seq-scans this shape forever,
which is the honest baseline for a structure the engine declares for itself.

Rerunning against one data file needs `--suffix`; nothing here drops tables.
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

DEFAULT_PORT = 15432

# ---- the shared shape (the PostgreSQL twin imports these) -----------------
#
#   id    the Keystone pk, system-generated (invariant 11) - never in an
#         INSERT list
#   val   the hot non-pk equality column; no index, no declared Cabin, no
#         policy - which reads as CABIN AUTO, the optimizer's jurisdiction
#   pad   one varchar so a row is a realistic OLTP width (one 64-byte
#         inline cell) and a page holds a realistic number of rows
COLUMNS = "id int64, val int64, pad varchar"
CLUSTERED = "BTREE"

MATCHES = 10        # rows matching the hot value, held constant across sizes
HOT_VAL = 7         # the probed value; exists at every size (domain >= 20)
SIZES = (200, 1000, 10000)
SIZE_TAGS = {200: "200", 1000: "1k", 10000: "10k"}


def values_of(rows):
    """Distinct `val` values at this size: rows/10, so the hot value matches
    exactly MATCHES rows at 200, 1,000 and 10,000 alike."""
    return max(2, rows // MATCHES)


def make_rows(rows):
    """(val, pad) per row, deterministic: row i carries val = i % domain."""
    domain = values_of(rows)
    return [(i % domain, f"PAD{i:08d}") for i in range(rows)]


def probe_sql(name):
    return f"SELECT * FROM {name} WHERE val = {HOT_VAL}"


def pk_sql(name, key):
    return f"SELECT * FROM {name} WHERE id = {key}"


# ---- plumbing -------------------------------------------------------------

ECHO = False


def abort(message, reply=None):
    print(f"cabin_optimizer_benchmark aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


class Client:
    """One connection, one line in / one line out, errors counted."""

    def __init__(self, host, port, timeout):
        try:
            self._conn = ServerConnection(host, port, timeout=timeout)
        except OSError as e:
            abort(f"could not connect to {host}:{port}: {e}\n"
                  f"  start one with: ./build-release/kds_server "
                  f"~/bench-cabinopt/co.db --port {port} --config <conf>")
        self.errors = 0
        self.first_error = None

    def __call__(self, command):
        reply = format_reply(self._conn.send_command(command))
        if ECHO:
            print(f"[co] {command[:80]}  ->  {reply[:110]}", file=sys.stderr,
                  flush=True)
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def timed(self, command, phase):
        t0 = time.perf_counter()
        reply = self(command)
        phase.record(time.perf_counter() - t0, reply)
        return reply

    def close(self):
        self._conn.close()


def server_cpu_seconds(pid):
    """utime+stime of the server process, in seconds (SC_CLK_TCK jiffies).

    The meter workplan-aggregate-perf.md requires for fixed costs client
    latency cannot resolve; resolution is one jiffy (typically 10ms), which
    is why the idle tick windows below run for tens of seconds."""
    with open(f"/proc/{pid}/stat") as f:
        after_comm = f.read().rsplit(") ", 1)[1].split()
    return (int(after_comm[11]) + int(after_comm[12])) / os.sysconf("SC_CLK_TCK")


def parse_kv(line):
    """`a=1 b=x` -> dict. Values carry no spaces on every surface parsed."""
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
    return out


def show_optimizer(client):
    """SHOW CABIN_OPTIMIZER -> (header dict, [entry dicts])."""
    reply = client("SHOW CABIN_OPTIMIZER")
    if reply.startswith("ERR") or reply.startswith("CABIN_OPTIMIZER absent"):
        abort("SHOW CABIN_OPTIMIZER unavailable - is cabins=on?", reply)
    lines = reply.splitlines()
    return parse_kv(lines[0]), [parse_kv(ln) for ln in lines[1:] if ln.strip()]


def show_cabins(client):
    """SHOW CABINS -> [row dicts]."""
    reply = client("SHOW CABINS")
    lines = reply.splitlines()
    return [parse_kv(ln) for ln in lines[1:] if ln.strip()]


def select_rows(reply):
    if reply.startswith("ERR"):
        return None
    lines = [ln for ln in reply.splitlines() if ln.strip()]
    return [tuple(f.strip() for f in ln.split(",")) for ln in lines[1:]]


def set_optimizer(client, on):
    reply = client(f"SET CABIN_OPTIMIZER {'ON' if on else 'OFF'}")
    if not reply.startswith("OK"):
        abort("SET CABIN_OPTIMIZER refused", reply)


def create_and_load(client, name, rows, load_phase, batch=500):
    reply = client(f"CREATE TABLE {name} ({COLUMNS}) {CLUSTERED}")
    if reply.startswith("ERR"):
        abort(f"CREATE TABLE {name} failed", reply)
    pending = 0
    client("BEGIN")
    for val, pad in make_rows(rows):
        client.timed(f"INSERT INTO {name} VALUES ({val}, '{pad}')", load_phase)
        pending += 1
        if pending >= batch:
            client("COMMIT")
            client("BEGIN")
            pending = 0
    client("COMMIT")


# ---- case: null -----------------------------------------------------------

def run_null(args, client):
    tags = [SIZE_TAGS[s] for s in SIZES]
    if args.size_order == "reverse":
        # Diagnostic: distinguishes a delta that follows relation *size*
        # from one that follows *position within the arm block*.
        tags = list(reversed(tags))
    names = {SIZE_TAGS[s]: f"con_{SIZE_TAGS[s]}_{args.suffix}" for s in SIZES}
    ins_name = f"con_ins_{args.suffix}"

    set_optimizer(client, False)
    load = Phase("load", "setup, txn batches")
    for size in SIZES:
        create_and_load(client, names[SIZE_TAGS[size]], size, load)
    reply = client(f"CREATE TABLE {ins_name} ({COLUMNS}) {CLUSTERED}")
    if reply.startswith("ERR"):
        abort(f"CREATE TABLE {ins_name} failed", reply)

    arms = ("off1", "on", "off2")
    lookup_phases = {(arm, tag): Phase(f"pk-{tag}[{arm}]",
                                       f"{dict(zip(tags, SIZES))[tag]} rows")
                     for arm in arms for tag in tags}
    insert_phases = {arm: Phase(f"insert[{arm}]", "logged, group durability")
                     for arm in arms}
    cpu = {arm: 0.0 for arm in arms} if args.server_pid else None

    rng = random.Random(args.seed)
    sizes_by_tag = dict(zip(tags, SIZES))

    # One unrecorded warm-up round: the first touch of every page and the
    # first branch of every code path land here instead of inside off1's
    # first block, where the smoke run showed them as a fake arm delta.
    warmup = Phase("warmup", "unrecorded round")
    for tag in tags:
        for _ in range(args.lookups):
            client.timed(pk_sql(names[tag],
                                rng.randrange(1, sizes_by_tag[tag] + 1)),
                         warmup)

    for _ in range(args.rounds):
        draws = {tag: [rng.randrange(1, sizes_by_tag[tag] + 1)
                       for _ in range(args.lookups)] for tag in tags}
        ins_rows = [(rng.randrange(1000), f"PAD{rng.randrange(10**8):08d}")
                    for _ in range(args.inserts)]
        for arm in arms:
            set_optimizer(client, arm == "on")
            c0 = server_cpu_seconds(args.server_pid) if cpu is not None else 0
            for tag in tags:
                phase = lookup_phases[(arm, tag)]
                for key in draws[tag]:
                    client.timed(pk_sql(names[tag], key), phase)
            for val, pad in ins_rows:
                client.timed(f"INSERT INTO {ins_name} VALUES ({val}, '{pad}')",
                             insert_phases[arm])
            if cpu is not None:
                cpu[arm] += server_cpu_seconds(args.server_pid) - c0

    # The tick itself, priced from idle server CPU. ON first, then OFF, each
    # window the same length; the OFF window carries the reactor's idle cost
    # and the disabled tick's one-predicate return, so the difference divided
    # by the tick count is the enabled tick - snapshot + Decide over this
    # run's fingerprint population and zero candidates.
    idle = None
    if args.server_pid and args.idle_seconds > 0:
        set_optimizer(client, True)
        h0, _ = show_optimizer(client)
        c0 = server_cpu_seconds(args.server_pid)
        time.sleep(args.idle_seconds)
        h1, _ = show_optimizer(client)
        c1 = server_cpu_seconds(args.server_pid)
        set_optimizer(client, False)
        c2 = server_cpu_seconds(args.server_pid)
        time.sleep(args.idle_seconds)
        c3 = server_cpu_seconds(args.server_pid)
        ticks = int(h1["ticks"]) - int(h0["ticks"])
        idle = {
            "window_s": args.idle_seconds,
            "ticks": ticks,
            "on_cpu_s": round(c1 - c0, 4),
            "off_cpu_s": round(c3 - c2, 4),
            "per_tick_us": round((c1 - c0 - (c3 - c2)) / ticks * 1e6, 2)
            if ticks > 0 else None,
        }

    header, entries = show_optimizer(client)
    eligible_clean = header.get("managed") == "0" and header.get("creates") == "0"
    if not eligible_clean:
        print(f"WARNING: expected zero candidates, got managed="
              f"{header.get('managed')} creates={header.get('creates')}",
              file=sys.stderr)

    phases = [load] + [lookup_phases[(arm, tag)] for tag in tags for arm in arms] \
        + [insert_phases[arm] for arm in arms]
    extra = {
        "case": "null",
        "arms": list(arms),
        "rounds": args.rounds,
        "ops_per_arm_per_round": {"lookups": args.lookups * len(tags),
                                  "inserts": args.inserts},
        "server_cpu_s_per_arm": {k: round(v, 4) for k, v in cpu.items()}
        if cpu is not None else None,
        "idle_tick": idle,
        "final_show_cabin_optimizer": header,
        "zero_candidates_confirmed": eligible_clean,
    }
    return phases, extra


# ---- case: improve --------------------------------------------------------

def wait_for_create(client, name, phase, probe, deadline_s):
    """Probes in blocks while polling SHOW CABIN_OPTIMIZER (untimed) until
    the managed entry for `name` exists with a cabin_id and SHOW CABINS
    reports a hit on it - i.e. the Cabin exists AND serves. Returns the
    evidence dict or aborts on timeout."""
    t_on = time.perf_counter()
    created_at = None
    entry_seen = None
    while time.perf_counter() - t_on < deadline_s:
        for _ in range(50):
            client.timed(probe, phase)
        header, entries = show_optimizer(client)
        mine = [e for e in entries if e.get("rel") == name]
        if mine and mine[0].get("cabin_id", "0") not in ("0", None):
            if created_at is None:
                created_at = time.perf_counter() - t_on
                entry_seen = mine[0]
            cabin_id = mine[0]["cabin_id"]
            for row in show_cabins(client):
                if row.get("cabin_id") == cabin_id and \
                        int(row.get("hits", "0") or 0) > 0:
                    return {
                        "create_after_s": round(created_at, 3),
                        "serving_after_s": round(time.perf_counter() - t_on, 3),
                        "entry_at_create": entry_seen,
                        "cabin_id": cabin_id,
                        "ticks_at_serve": header.get("ticks"),
                    }
    abort(f"no serving Cabin on {name} within {deadline_s}s - "
          f"SHOW CABIN_OPTIMIZER said: {show_optimizer(client)[0]}")


def run_improve(args, client):
    set_optimizer(client, False)
    names = {SIZE_TAGS[s]: f"coh_{SIZE_TAGS[s]}_{args.suffix}" for s in SIZES}
    load = Phase("load", "setup, txn batches")
    for size in SIZES:
        create_and_load(client, names[SIZE_TAGS[size]], size, load)

    phases = [load]
    evidence = {}
    rng = random.Random(args.seed)
    for size in SIZES:
        tag = SIZE_TAGS[size]
        name = names[tag]
        probe = probe_sql(name)

        pk_pre = Phase(f"pk-{tag}[pre]", "control, optimizer off")
        for _ in range(args.pk_ops):
            client.timed(pk_sql(name, rng.randrange(1, size + 1)), pk_pre)

        walk = Phase(f"probe-{tag}[walk]", f"{size} rows, optimizer off")
        cpu_walk = 0.0
        for start in range(0, args.probe_ops, 100):
            c0 = server_cpu_seconds(args.server_pid) if args.server_pid else 0
            for _ in range(min(100, args.probe_ops - start)):
                client.timed(probe, walk)
            if args.server_pid:
                cpu_walk += server_cpu_seconds(args.server_pid) - c0
        baseline = select_rows(client(probe))
        analyze_walk = client(f"ANALYZE {probe}")

        set_optimizer(client, True)
        transition = Phase(f"probe-{tag}[transition]", "SET ON -> served")
        transition.trace = []
        created = wait_for_create(client, name, transition, probe,
                                  args.create_timeout)

        served = Phase(f"probe-{tag}[served]", f"{size} rows, Cabin serving")
        cpu_served = 0.0
        for start in range(0, args.probe_ops, 100):
            c0 = server_cpu_seconds(args.server_pid) if args.server_pid else 0
            for _ in range(min(100, args.probe_ops - start)):
                client.timed(probe, served)
            if args.server_pid:
                cpu_served += server_cpu_seconds(args.server_pid) - c0

        verified = None
        if args.verify:
            after_rows = select_rows(client(probe))
            verified = (baseline == after_rows)
            if not verified:
                abort(f"{name}: served reply differs from walked reply - "
                      f"a Cabin changed a query result")
        analyze_served = client(f"ANALYZE {probe}")
        header, entries = show_optimizer(client)
        mine = [e for e in entries if e.get("rel") == name]
        cabin_rows = [r for r in show_cabins(client)
                      if r.get("cabin_id") == created["cabin_id"]]

        pk_post = Phase(f"pk-{tag}[post]", "control, Cabin active")
        for _ in range(args.pk_ops):
            client.timed(pk_sql(name, rng.randrange(1, size + 1)), pk_post)
        set_optimizer(client, False)

        phases += [pk_pre, walk, transition, served, pk_post]
        evidence[tag] = {
            "rows": size,
            "matches": MATCHES,
            "created": created,
            "managed_entry": mine[0] if mine else None,
            "show_header": header,
            "cabin_row": cabin_rows[0] if cabin_rows else None,
            "analyze_walk": analyze_walk,
            "analyze_served": analyze_served,
            "verified_identical_reply": verified,
            "server_cpu_us_per_op": {
                "walk": round(cpu_walk / args.probe_ops * 1e6, 1),
                "served": round(cpu_served / args.probe_ops * 1e6, 1),
            } if args.server_pid else None,
            "transition_trace": [(round(t, 6), round(l * 1e6, 1))
                                 for t, l in transition.trace],
        }

    return phases, {"case": "improve", "probe": probe_sql("<rel>"),
                    "probe_ops": args.probe_ops, "sizes": evidence}


# ---- main -----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--case", choices=("null", "improve"), required=True)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--suffix", default="a", help="relation-name suffix so "
                    "reruns can share a data file (nothing drops tables)")
    ap.add_argument("--seed", type=int, default=20260810)
    ap.add_argument("--server-pid", type=int, default=0,
                    help="server pid for /proc CPU sampling (0 = off)")
    ap.add_argument("--json", default=None)
    # null case
    ap.add_argument("--rounds", type=int, default=60)
    ap.add_argument("--lookups", type=int, default=60,
                    help="pk lookups per size per arm per round")
    ap.add_argument("--inserts", type=int, default=10,
                    help="inserts per arm per round")
    ap.add_argument("--idle-seconds", type=float, default=60.0,
                    help="length of each idle tick-cost window")
    ap.add_argument("--size-order", choices=("normal", "reverse"),
                    default="normal")
    # improve case
    ap.add_argument("--probe-ops", type=int, default=1200,
                    help="hot-probe ops in each of walk and served")
    ap.add_argument("--pk-ops", type=int, default=300)
    ap.add_argument("--create-timeout", type=float, default=45.0)
    ap.add_argument("--verify", action=argparse.BooleanOptionalAction,
                    default=True)
    ap.add_argument("--echo", action="store_true")
    args = ap.parse_args()

    global ECHO
    ECHO = args.echo

    client = Client(args.host, args.port, args.timeout)
    meta_reply = client("SHOW META")

    t0 = time.time()
    if args.case == "null":
        phases, extra = run_null(args, client)
    else:
        phases, extra = run_improve(args, client)

    meta = {
        "engine": "ckdbs",
        "driver": "cabin_optimizer_benchmark.py",
        "case": args.case,
        "host": args.host, "port": args.port,
        "columns": COLUMNS, "clustered": CLUSTERED,
        "rows": "/".join(str(s) for s in SIZES),
        "table": f"co*_{args.suffix}",
        "sizes": list(SIZES), "seed": args.seed,
        "suffix": args.suffix,
        "started_utc": datetime.datetime.utcfromtimestamp(t0)
        .strftime("%Y-%m-%d %H:%M:%S"),
        "elapsed_s": round(time.time() - t0, 1),
        "client_errors": client.errors,
        "first_error": client.first_error,
        "show_meta": meta_reply,
    }
    report(phases, meta)
    if client.errors:
        print(f"\nERRORS: {client.errors}, first: {client.first_error}",
              file=sys.stderr)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"meta": meta, "extra": extra,
                       "phases": [p.summary() for p in phases]}, f, indent=2)
        print(f"json -> {args.json}")
    client.close()
    sys.exit(1 if client.errors else 0)


if __name__ == "__main__":
    main()
