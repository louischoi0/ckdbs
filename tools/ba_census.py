#!/usr/bin/env python3
"""BA's census driver and its PostgreSQL twin (BA-S3).

`instructions/v3.0.0/workorder-ba-parallelism.md` BA-R0. One cell is one engine,
one shape, one durability, one core count and one session count; it runs
`--runs` times, each against a fresh server (KDS) or fresh tables (PostgreSQL),
with every client a separate process (AT-S13's Python ceiling was one process)
started at a common instant and run for `--seconds` after `--warmup` seconds.

Shapes:
  point      a pk point read of a preloaded relation (`--rows` rows), a key
             drawn uniformly per statement
  trade      scenario 0's trade: two INSERTs into `trades` and two pk
             UPDATEs of `accounts`, autocommit, each client trading among its
             own accounts (no two clients write one account)
  insert1    a monotonic-pk INSERT (the pk omitted / an identity column) into
             one relation shared by every client
  insertN    the same into one relation per client: what separates the pk's
             issue (page 7 on KDS) from the rightmost leaf
  recent     an INSERT, then a pk read of a row written up to 300 ids before
             it - rows written within the last few hundred commits, which on
             KDS are classified through the visibility window (P2)

Durability: KDS `relaxed`, `group`, `strict` (the server's `durability`);
PostgreSQL `off` and `on` (`synchronous_commit`). Off is the like-for-like for
`relaxed`; PostgreSQL group-commits `on` by itself.

Pinning:
  --server-cpus LIST   KDS: written as `reactor_cpus` (reactor k on LIST[k],
                       core 0 included; LIST must name `--cores` CPUs).
                       PostgreSQL: the postmaster of the cluster on --pg-port
                       and every process it has forked (checkpointer, WAL
                       writer, I/O workers, ...) are re-pinned to LIST with
                       `taskset -pc` before the cell; a backend forked later
                       inherits the mask (BA-R0: the reactors' CPUs). The pin
                       outlives the tool.
  --client-cpus LIST   client i runs under `taskset -c LIST[i % len]`.
  On this host (BA-Q2) the clean map is --server-cpus 0,2 --client-cpus 4,5,6,7,
  and `cores = 2` is the largest cell whose reactors and clients share no
  physical core; a larger cell is indicative only, and its file says so.

The client-bound mark: each client's CPU time over its measured span. A cell
any of whose clients ran above 80 % of one CPU is marked `client_bound`: its
throughput is a floor on the engine, never its ceiling.

Per run the JSON holds: statements/s, per-statement latency p0/p25/p50/p95/p99
over every client, refusals (KDS `TXN_CONFLICT retryable=1`, retried; a
PostgreSQL serialization or deadlock error, retried), errors, each client's CPU
share and the client-bound mark, each KDS session's core, and for KDS the delta
of every numeric `SHOW META` field across the measured span - the
`contention_*` block (BA-S2) among them. **The contention fields are summed
over every core, but `sched_*` describes only the core the control session
landed on** (`meta_session_core`), so an item's share at `cores = k` is its
wait over k times the span, not over `sched_wall_us`. `*_longest_us` fields
are maxima since the server started and are reported as read
(`meta_longest`), never as a delta. Each run also records its server
binary's sha256 and the data file's device (bench/README.md rules 2 and 3);
every server runs from one copy of `--bin` made at start.

Ports have no default (bench/README.md rule 5): `--port` for KDS,
`--pg-port` for PostgreSQL. A KDS port something already answers on is
refused.

    tools/ba_census.py --engine kds --bin build-release/kds_server --port P \\
        --shape point --durability group --cores 2 --sessions 4 \\
        --server-cpus 0,2 --client-cpus 4,5,6,7 --out bench/.../archive
    tools/ba_census.py --engine pg --pg-port Q --shape trade --durability on ...
    tools/ba_census.py --dry-run --bin build/kds_server --port P --pg --pg-port Q
                                                              # BA-S3's exit
"""

import argparse
import hashlib
import json
import os
import random
import re
import resource
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import nearest_rank  # noqa: E402
from bb_overhead_benchmark import host_state, meta_all, wait_for_load  # noqa: E402

SHAPES = ("point", "trade", "insert1", "insertN", "recent")
KDS_DURABILITY = ("relaxed", "group", "strict")
PG_DURABILITY = ("off", "on")
CLIENT_BOUND = 0.80
RECENT_WINDOW = 300
ACCOUNTS_PER_CLIENT = 64
OPENING_BALANCE = 100_000_000
INSERTED_ID = re.compile(r"\bid=(\d+)")


# ---- the two backends: one statement in, one reply out ---------------------

class Kds:
    """KDS over KWP. A reply beginning `ERR` is an error; a retryable
    `TXN_CONFLICT` is a refusal the client retries."""

    def __init__(self, port):
        from ckdbs_cli import ServerConnection
        self.conn = ServerConnection("127.0.0.1", port)

    def run(self, sql):
        """(reply, None, None), or (None, "refused" | "error", the reply)."""
        r = self.conn.send_command(sql)
        if not r.startswith("ERR"):
            return r, None, None
        refused = "TXN_CONFLICT" in r and "retryable=1" in r
        return None, ("refused" if refused else "error"), r

    def inserted_id(self, sql):
        r, kind, err = self.run(sql)
        if kind:
            return None, kind, err
        got = INSERTED_ID.search(r)
        return (int(got.group(1)) if got else None), None, None


class Pg:
    """PostgreSQL over the v3 wire protocol (`pg_wire.py`)."""

    RETRYABLE = ("40001", "40P01")  # serialization failure, deadlock

    def __init__(self, port, database, sync):
        from pg_wire import PgConnection
        self.conn = PgConnection(port=port, database=database)
        self.run(f"SET synchronous_commit = {sync}")

    def _failed(self, error):
        # `pg_wire` renders an error as "<SQLSTATE> <message>".
        return None, ("refused" if error.startswith(self.RETRYABLE) else "error"), error

    def run(self, sql):
        _tag, _rows, _bytes, error = self.conn.query(sql)
        return self._failed(error) if error is not None else ("ok", None, None)

    def inserted_id(self, sql):
        rows, error = self.conn.fetch(sql + " RETURNING id")
        return self._failed(error) if error is not None else (int(rows[0][0]), None, None)


# ---- schema and load, the same rows on both engines ------------------------

def ddl(engine, name, columns):
    """`columns` excludes the pk. KDS's first column is the Keystone id, which
    an INSERT that omits it is issued; PostgreSQL's is an identity column."""
    if engine == "kds":
        return f"CREATE TABLE {name} (id int64, {', '.join(c + ' int64' for c in columns)}) BTREE"
    cols = ", ".join(c + " bigint NOT NULL" for c in columns)
    return f"CREATE TABLE {name} (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, {cols})"


def insert_sql(engine, name, *rows):
    """An INSERT of `rows` whose pks are issued: KDS omits the pk, PostgreSQL
    names the columns so the identity fills it."""
    vals = ", ".join("(" + ", ".join(str(v) for v in r) + ")" for r in rows)
    if engine == "kds":
        return f"INSERT INTO {name} VALUES {vals}"
    cols = ", ".join(f"c{i}" for i in range(len(rows[0])))
    return f"INSERT INTO {name} ({cols}) VALUES {vals}"


PRELOAD_BATCH = 200


def preload(engine, be, name, rows):
    """Setup rows, `PRELOAD_BATCH` to a statement: one autocommit per row would
    cost a sync each at `strict`, for rows nothing measures."""
    for at in range(0, len(rows), PRELOAD_BATCH):
        _must(be, insert_sql(engine, name, *rows[at:at + PRELOAD_BATCH]))


def tables(args, suffix):
    t = {"point": f"ba_point{suffix}", "trades": f"ba_trades{suffix}",
         "accounts": f"ba_accounts{suffix}", "recent": f"ba_recent{suffix}"}
    t["inserts"] = [f"ba_ins{suffix}_{i}" for i in range(args.sessions if args.shape == "insertN" else 1)]
    return t


def setup(args, be, suffix):
    t = tables(args, suffix)
    must = lambda sql: _must(be, sql)
    if args.shape == "point":
        must(ddl(args.engine, t["point"], ["c0", "c1"]))
        preload(args.engine, be, t["point"], [[i, i] for i in range(args.rows)])
    elif args.shape == "trade":
        must(ddl(args.engine, t["accounts"], ["c0", "c1", "c2"]))  # balance, qty, trade count
        must(ddl(args.engine, t["trades"], ["c0", "c1", "c2", "c3", "c4", "c5"]))
        preload(args.engine, be, t["accounts"],
                [[OPENING_BALANCE, 0, 0]] * (args.sessions * ACCOUNTS_PER_CLIENT))
    elif args.shape in ("insert1", "insertN"):
        for name in t["inserts"]:
            must(ddl(args.engine, name, ["c0", "c1"]))
    elif args.shape == "recent":
        must(ddl(args.engine, t["recent"], ["c0", "c1"]))
        preload(args.engine, be, t["recent"], [[i, i] for i in range(RECENT_WINDOW)])
    return t


def _must(be, sql):
    _r, kind, err = be.run(sql)
    if kind:
        raise SystemExit(f"setup: {sql!r} -> {err!r}")


def drop(args, be, suffix):
    t = tables(args, suffix)
    for name in [t["point"], t["trades"], t["accounts"], t["recent"]] + t["inserts"]:
        be.run(f"DROP TABLE IF EXISTS {name}")


# ---- one client process ----------------------------------------------------

def statements(args, t, index, rng, last):
    """One unit of the shape's work, as a generator of lists of (sql, kind).
    `kind` names the statement's latency distribution; "write+id" also
    records the issued id in `last["id"]`, which the recent read follows."""
    if args.shape == "point":
        while True:
            yield [(f"SELECT * FROM {t['point']} WHERE id = {rng.randint(1, args.rows)}", "read")]
    if args.shape == "trade":
        first = index * ACCOUNTS_PER_CLIENT + 1
        state = {a: [OPENING_BALANCE, 0, 0] for a in range(first, first + ACCOUNTS_PER_CLIENT)}
        day = 0
        while True:
            buyer, seller = rng.sample(sorted(state), 2)
            qty, price = 1, rng.randint(100, 10_000)
            day += 1
            for acct, sign in ((buyer, -1), (seller, 1)):
                state[acct][0] += sign * qty * price
                state[acct][1] -= sign * qty
                state[acct][2] += 1
            yield [(insert_sql(args.engine, t["trades"], [buyer, 7, 0, qty, price, day]), "write"),
                   (insert_sql(args.engine, t["trades"], [seller, 7, 1, qty, price, day]), "write")] + [
                (f"UPDATE {t['accounts']} SET c0 = {state[a][0]}, c1 = {state[a][1]}, "
                 f"c2 = {state[a][2]} WHERE id = {a}", "write") for a in (buyer, seller)]
    if args.shape in ("insert1", "insertN"):
        name = t["inserts"][index if args.shape == "insertN" else 0]
        n = 0
        while True:
            n += 1
            yield [(insert_sql(args.engine, name, [index, n]), "write")]
    if args.shape == "recent":
        n = 0
        while True:
            n += 1
            unit = [(insert_sql(args.engine, t["recent"], [index, n]), "write+id")]
            if last["id"] is not None:
                back = max(1, last["id"] - rng.randint(0, RECENT_WINDOW))
                unit.append((f"SELECT * FROM {t['recent']} WHERE id = {back}", "read"))
            yield unit


def client(args):
    if args.engine == "kds":
        be = Kds(args.port)
    else:
        be = Pg(args.port, args.pg_database, args.durability)
    core = meta_all(be.conn).get("core") if args.engine == "kds" else None
    t = tables(args, args.suffix)
    rng = random.Random(args.client_index * 7919 + 1)
    last = {"id": None}
    gen = statements(args, t, args.client_index, rng, last)
    lat, refusals, errors, first_err = {}, 0, 0, None
    while time.time() < args.start_at:
        time.sleep(0.0005)
    measure_from = args.start_at + args.warmup
    stop_at = measure_from + args.seconds
    cpu0 = t_start = None
    while True:
        now = time.time()
        if now >= stop_at:
            break
        if cpu0 is None and now >= measure_from:
            r0 = resource.getrusage(resource.RUSAGE_SELF)
            cpu0, t_start = r0.ru_utime + r0.ru_stime, now
        # `next(gen)` is taken per unit, after the last unit's id is in.
        for sql, which in next(gen):
            t0 = time.perf_counter()
            for _attempt in range(1000):
                if which == "write+id":
                    got, kind, err = be.inserted_id(sql)
                    if not kind:
                        last["id"] = got
                else:
                    _r, kind, err = be.run(sql)
                if kind != "refused":
                    break
                if cpu0 is not None:
                    refusals += 1
            if cpu0 is not None:
                lat.setdefault("write" if which == "write+id" else which, []).append(time.perf_counter() - t0)
                if kind:  # an error, or a refusal still refused after every retry
                    errors += 1
                    first_err = first_err or err
    r1 = resource.getrusage(resource.RUSAGE_SELF)
    t_end = time.time()
    if cpu0 is None:  # one unit outlasted the whole measured span: nothing measured
        cpu0, t_start = r1.ru_utime + r1.ru_stime, t_end
    cpu = (r1.ru_utime + r1.ru_stime) - cpu0
    print(json.dumps({"index": args.client_index, "core": core, "t_start": t_start, "t_end": t_end,
                      "cpu_s": cpu, "refusals": refusals, "errors": errors, "first_error": first_err,
                      "lat": lat}))


# ---- one run of one cell ---------------------------------------------------

def start_kds(args, wd, port):
    data = os.path.join(wd, "d.db")
    conf = os.path.join(wd, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {args.cores}\n"
                f"durability = {args.durability}\nbuffer_pool_frames = {args.frames}\n"
                f"log_file = s.log\nlog_dir = {wd}\nlog_level = warn\n")
        if args.server_cpus:
            f.write(f"reactor_cpus = {args.server_cpus}\n")
    # bench/README.md rule 5: a port something already answers on would hand
    # the connect loop below another instance while ours fails to bind.
    with socket.socket() as s:
        if s.connect_ex(("127.0.0.1", port)) == 0:
            raise SystemExit(f"port {port} is already in use; choose another with --port")
    err = open(os.path.join(wd, "s.stderr"), "w")
    proc = subprocess.Popen([args.bin, "--config", conf], stdout=err, stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    while True:
        try:
            return proc, err, Kds(port)
        except OSError:
            if proc.poll() is not None or time.time() > deadline:
                err.close()
                raise SystemExit(f"server did not listen; see {wd}/s.stderr")
            time.sleep(0.1)


def pg_data_directory(ctl):
    rows, error = ctl.conn.fetch("SHOW data_directory")
    if error is not None:
        raise SystemExit(f"SHOW data_directory -> {error}")
    return rows[0][0].decode()


def data_device(path):
    """bench/README.md rule 2: the device and filesystem under the data file,
    as `df -T` names them."""
    out = subprocess.run(["df", "-T", path], capture_output=True, text=True).stdout.split("\n")
    return " ".join(out[1].split()[:2]) if len(out) > 1 else None


def pin_postmaster(args, ctl):
    """Pins the postmaster of the cluster `ctl` is connected to, and every
    process it has already forked: a backend forked later inherits the mask,
    but the checkpointer, WAL writer, background writer and I/O workers were
    forked at startup and would otherwise stay on every CPU. The data
    directory comes from the server, so no other cluster can be pinned."""
    with open(os.path.join(pg_data_directory(ctl), "postmaster.pid")) as f:
        pid = f.readline().strip()
    children = subprocess.run(["pgrep", "-P", pid], capture_output=True, text=True).stdout.split()
    for p in [pid] + children:
        subprocess.run(["taskset", "-pc", args.server_cpus, p], check=True, capture_output=True)


def run_one(args, run):
    tag = (f"{args.engine}-{args.shape}-{args.durability}-k{args.cores}-s{args.sessions}-r{run}"
           if args.engine == "kds" else f"pg-{args.shape}-{args.durability}-s{args.sessions}-r{run}")
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    suffix = f"_{os.getpid()}_{run}"
    proc = err = ctl = None
    procs = []
    wd = os.path.join(args.workdir, tag)
    os.makedirs(wd, exist_ok=True)
    try:
        if args.engine == "kds":
            # bench/README.md rule 3: every server runs from the copy main()
            # made once; hashed per run all the same.
            with open(args.bin, "rb") as f:
                bin_sha256 = hashlib.sha256(f.read()).hexdigest()
            proc, err, ctl = start_kds(args, wd, args.port)
        else:
            bin_sha256 = None
            ctl = Pg(args.port, args.pg_database, args.durability)
            if args.server_cpus:
                pin_postmaster(args, ctl)
        setup(args, ctl, suffix)
        m0 = meta_all(ctl.conn) if args.engine == "kds" else {}
        start_at = time.time() + 3.0
        for i in range(args.sessions):
            cl = [sys.executable, os.path.abspath(__file__), "--client", "--engine", args.engine,
                  "--port", str(args.port), "--client-index", str(i), "--shape", args.shape,
                  "--sessions", str(args.sessions), "--rows", str(args.rows),
                  "--durability", args.durability, "--suffix", suffix, "--start-at", repr(start_at),
                  "--warmup", str(args.warmup), "--seconds", str(args.seconds),
                  "--pg-database", args.pg_database]
            if args.client_cpus:
                cpus = args.client_cpus.split(",")
                cl = ["taskset", "-c", cpus[i % len(cpus)]] + cl
            procs.append(subprocess.Popen(cl, stdout=subprocess.PIPE, text=True))
        # SHOW META around the measured span only: read just after warmup
        # and just after the clients stop.
        time.sleep(max(0.0, start_at + args.warmup - time.time()))
        m_from = meta_all(ctl.conn) if args.engine == "kds" else {}
        outs = []
        for p in procs:
            o, _ = p.communicate(timeout=args.warmup + args.seconds + 600)
            outs.append(json.loads(o.strip().splitlines()[-1]))
        m1 = meta_all(ctl.conn) if args.engine == "kds" else {}
        after = host_state()
        lat = sorted(x for o in outs for xs in o["lat"].values() for x in xs)
        kinds = sorted({k for o in outs for k in o["lat"]})
        by_kind = {}
        for k in kinds:
            xs = sorted(x for o in outs for x in o["lat"].get(k, []))
            by_kind[k] = {"statements": len(xs),
                          **{f"p{q}_us": round(nearest_rank(xs, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)}}
        span = max(o["t_end"] for o in outs) - min(o["t_start"] for o in outs)
        shares = [round(o["cpu_s"] / max(1e-9, o["t_end"] - o["t_start"]), 2) for o in outs]
        res = {
            "engine": args.engine, "shape": args.shape, "durability": args.durability,
            "cores": args.cores if args.engine == "kds" else None, "sessions": args.sessions,
            "run": run, "seconds": args.seconds, "warmup": args.warmup,
            "server_cpus": args.server_cpus, "client_cpus": args.client_cpus,
            "wait_s": round(wait, 1), "host_before": before, "host_after": after,
            "statements": len(lat), "span_s": round(span, 4),
            "statements_per_s": round(len(lat) / span, 1) if span > 0 else None,
            **({f"p{q}_us": round(nearest_rank(lat, q) * 1e6, 1) for q in (25, 50, 95, 99)} if lat else {}),
            "p0_us": round(lat[0] * 1e6, 1) if lat else None,
            "max_us": round(lat[-1] * 1e6, 1) if lat else None,
            # One distribution per statement kind: a shape that mixes reads
            # and writes (recent) reports each apart, beside the pooled one.
            "by_kind": by_kind,
            # One per client, with the core its session landed on: BA-R4's
            # premise is the writer hand-off's cost, read as a peer session's
            # commit latency against core 0's, which syncs inline.
            "by_client": [{"core": o["core"], "statements": n,
                           **({f"p{q}_us": round(nearest_rank(xs, q) * 1e6, 1) for q in (50, 99)}
                              if n else {})}
                          for o in outs
                          for xs in [sorted(x for v in o["lat"].values() for x in v)]
                          for n in [len(xs)]],
            "refusals": sum(o["refusals"] for o in outs), "errors": sum(o["errors"] for o in outs),
            "first_error": next((o["first_error"] for o in outs if o["first_error"]), None),
            "client_cpu_share": shares, "client_bound": max(shares) > CLIENT_BOUND,
            "session_cores": [o["core"] for o in outs],
            # `*_longest_us` is a maximum since the server started, not a
            # counter: its delta is meaningless, so it is reported as read.
            "meta_delta": {k: m1[k] - m_from[k] for k in m1 if k in m_from and not k.endswith("_longest_us")},
            "meta_longest": {k: v for k, v in m1.items() if k.endswith("_longest_us")},
            "meta_session_core": m1.get("core"),  # whose sched_* the block is
            "bin_sha256": bin_sha256,
            "data_device": data_device(wd if args.engine == "kds" else pg_data_directory(ctl)),
            "setup_meta": {k: m0[k] for k in ("recovery_checkpoint_us",) if k in m0},
        }
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
                json.dump(res, f, indent=1)
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} "
              f"stmts={res['statements']} errors={res['errors']} refusals={res['refusals']} "
              f"client_bound={res['client_bound']} cores={sorted(set(map(str, res['session_cores'])))}"
              + ("" if args.dry_run else f" stmt/s={res['statements_per_s']} p50={res.get('p50_us')}"),
              flush=True)
        return res
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        if ctl is not None and args.engine == "pg":
            drop(args, ctl, suffix)
            ctl.conn.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if err is not None:
            err.close()
        shutil.rmtree(wd, ignore_errors=True)


def dry_run(args):
    """BA-S3's exit: every shape once at `cores = 1`, one session, a second
    each, KDS always and PostgreSQL when `--pg` says it is up. The output
    proves the cells run and the replies are not errors; it claims no number."""
    failed = []
    engines = ["kds"] + (["pg"] if args.pg else [])
    for engine in engines:
        for shape in SHAPES:
            a = argparse.Namespace(**vars(args))
            a.engine, a.shape, a.cores, a.sessions = engine, shape, 1, 1
            a.durability = "relaxed" if engine == "kds" else "off"
            a.seconds, a.warmup, a.rows, a.server_cpus = 1.0, 0.2, 200, None
            a.out = None
            if engine == "pg":
                a.port = args.pg_port
            res = run_one(a, 0)
            if res["errors"] or not res["statements"]:
                failed.append(f"{engine}/{shape}: {res['first_error'] or 'no statement ran'}")
    print("dry run: " + ("every cell ran, no number claimed" if not failed else "FAILED " + "; ".join(failed)))
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--engine", choices=("kds", "pg"), default="kds")
    p.add_argument("--shape", choices=SHAPES, default="point")
    p.add_argument("--durability", default=None,
                   help="KDS: relaxed|group|strict (default group); PostgreSQL synchronous_commit: off|on (default on)")
    p.add_argument("--cores", type=int, default=1, help="KDS reactor cores")
    p.add_argument("--sessions", type=int, default=1, help="client processes, one session each")
    p.add_argument("--seconds", type=float, default=10.0, help="measured span per run")
    p.add_argument("--warmup", type=float, default=2.0, help="unmeasured span before it")
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--rows", type=int, default=10_000, help="the point shape's preloaded rows")
    p.add_argument("--bin", help="kds_server (build-release for any number)")
    p.add_argument("--port", type=int, default=None,
                   help="KDS port, required: no default (bench/README.md rule 5)")
    p.add_argument("--frames", type=int, default=65536, help="KDS buffer_pool_frames")
    p.add_argument("--pg", action="store_true", help="dry run: include PostgreSQL")
    p.add_argument("--pg-port", type=int, default=None, help="PostgreSQL port, required for --engine pg")
    p.add_argument("--pg-database", default="bench")
    p.add_argument("--server-cpus", default=None)
    p.add_argument("--client-cpus", default=None)
    p.add_argument("--workdir", default=os.path.expanduser("~/bench-runs/ba-census/work"))
    p.add_argument("--out", default=None, help="directory for one JSON per run")
    p.add_argument("--max-load", type=float, default=1.5)
    p.add_argument("--max-wait", type=float, default=120)
    p.add_argument("--dry-run", action="store_true")
    # a client process
    p.add_argument("--client", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--client-index", type=int, default=0, help=argparse.SUPPRESS)
    p.add_argument("--start-at", type=float, default=0.0, help=argparse.SUPPRESS)
    p.add_argument("--suffix", default="", help=argparse.SUPPRESS)
    args = p.parse_args()
    if args.durability is None:
        args.durability = "group" if args.engine == "kds" else "on"
    if args.client:
        client(args)
        return 0
    if (args.engine == "kds" or args.dry_run) and not (args.bin and args.port):
        p.error("KDS needs --bin and --port")
    if (args.engine == "pg" or args.pg) and not args.pg_port:
        p.error("PostgreSQL needs --pg-port")
    if args.bin:
        # bench/README.md rule 3: run from a copy, so a rebuild of --bin
        # mid-matrix cannot change what the later runs measure.
        os.makedirs(args.workdir, exist_ok=True)
        copy = os.path.join(args.workdir, "kds_server.census")
        shutil.copy2(args.bin, copy)
        args.bin = copy
    if args.dry_run:
        return dry_run(args)
    allowed = KDS_DURABILITY if args.engine == "kds" else PG_DURABILITY
    if args.durability not in allowed:
        p.error(f"--durability for {args.engine} is one of {', '.join(allowed)}")
    if args.engine == "pg":
        args.port = args.pg_port
    for run in range(args.runs):
        run_one(args, run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
