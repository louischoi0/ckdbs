#!/usr/bin/env python3
"""BN's census and its A/B: where a statement's time goes, from both ends.

    bn_census.py --bin A [--bin-b B] --workdir DIR [--port P] [--pin-server 2]
                 [--pin-driver 4] [--rows 1000] [--scan-rows 10000]
                 [--ops 3000] [--scan-ops 200] [--range-ops 60] [--blocks 12]
                 [--runs 3] [--sampler SO --sampler-dir D --profile-ops N]
                 --out OUT.json

`instructions/v3.0.0/workorder-bn-opt-read-update-path.md` §3 (BN-S1) and §5
(the close). Per run a fresh server per binary (`cores = 1`,
`durability = relaxed`, a fresh data file), the same relations loaded into
each, then every shape below over both driver paths:

  literal   an unnamed C_PARSE of the statement's text per execution (what
            every driver in tools/ does, `kwp.Connection.execute`)
  prepared  one named C_PARSE, then BIND/EXECUTE per execution
            (`kwp.Connection.execute_prepared`)

The shapes: `noop` (a C_PING frame, answered by the session without the
dispatcher - the request path alone), `ping` (SHOW META, the BJ-era floor),
point SELECT, point UPDATE (literal, `v + 1`, a row whose `varchar` cell
spills and is not assigned, and one assigning a spilled value), INSERT,
DELETE by pk (its rows inserted untimed before the arm), an every-row UPDATE
of `--rows` rows, and an unindexed equality over `--scan-rows`.

With `--bin-b`, both servers run side by side (one port each, both pinned to
`--pin-server`, only one ever busy) and every arm is cut into `--blocks`
blocks taken alternately, the first server of a block alternating, so host
drift lands on both sides alike.

With `--arm LABEL=BINARY[,scan_kernel=on|off]` (repeatable; it replaces
`--bin`/`--bin-b`), any number of servers run side by side the same way, the
order of a block rotating by one server per block and per run so that each
server takes each position equally often. `scan_kernel=` writes that key into
the arm's config (BM-Q5's switch; absent: the engine's default, and a binary
that predates the key is never given it). BM-S6's close runs four: A, A' (the
same commit from a second export), B, and B with the kernel off.
`--scan-kernel on|off` does the same for every server of a single-`--bin` run.

Per (binary, shape, path) it records the client's wall time per statement
and, over the arm, **the server's on-CPU time** (the sum of
/proc/<pid>/task/*/schedstat's first field, every server thread) and **the
driver thread's on-CPU time** (/proc/thread-self/schedstat), each per
statement. The two overlap - the client handles one reply while the server
is still writing the next frames - so they are not a partition of the wall
time. The server's idle burn (its timers) is measured over an idle second
per run and reported, not subtracted.

With `--sampler` (a build of tools/kds_sampler.c), a profile pass of
`--bin` runs first under LD_PRELOAD: `--profile-ops` statements per literal
shape between SIGUSR1 and SIGUSR2, one dump per arm, named in the output for
`tools/kds_sampler_report.py`.

Every reply is checked against the tag its shape must produce, and the
every-row and spill arms' end states are read back. After the arms, each
server takes `--rows` updates of an unspilled row and of a spilled one,
`pages_allocated` from SHOW META read around each.
"""
import argparse, json, os, shutil, signal, struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from bb_overhead_benchmark import host_state  # noqa: E402

SPILL = "x" * 300  # longer than the instance's inline cell, so it spills


def sched_ns(pid):
    total = 0
    base = f"/proc/{pid}/task"
    for tid in os.listdir(base):
        try:
            with open(f"{base}/{tid}/schedstat") as f:
                total += int(f.read().split()[0])
        except OSError:
            pass
    return total


def sched_stat(pid):
    """(on-CPU ns, run-queue wait ns) summed over a process's threads: the second
    field of /proc/<pid>/task/*/schedstat is the time the thread was runnable
    and waiting for its CPU, which is what another session's task on the same
    CPU costs it."""
    cpu = wait = 0
    base = f"/proc/{pid}/task"
    try:
        tids = os.listdir(base)
    except OSError:
        return 0, 0
    for tid in tids:
        try:
            with open(f"{base}/{tid}/schedstat") as f:
                v = f.read().split()
            cpu += int(v[0])
            wait += int(v[1])
        except (OSError, IndexError):
            pass
    return cpu, wait


def self_stat():
    """The calling thread's (on-CPU ns, run-queue wait ns)."""
    with open("/proc/thread-self/schedstat") as f:
        v = f.read().split()
    return int(v[0]), int(v[1])


def self_ns():
    with open("/proc/thread-self/schedstat") as f:
        return int(f.read().split()[0])


def pct(xs, p):
    xs = sorted(xs)
    return xs[max(0, min(len(xs) - 1, -(-p * len(xs) // 100) - 1))] if xs else 0.0


def meta(conn, key):
    _, rows, tag, _ = conn.execute("SHOW META")
    text = tag + " " + " ".join(b" ".join(c or b"" for c in r).decode() for r in rows)
    for tok in text.replace("\\n", " ").split():
        if tok.startswith(key + "="):
            return int(tok.split("=", 1)[1])
    return None


class Server:
    def __init__(self, a, label, binary, port, tag, sampler_out=None, scan_kernel=None):
        self.label = label
        os.makedirs(a.workdir, exist_ok=True)
        self.data = os.path.join(a.workdir, f"bk-{label}-{tag}.db")
        conf = os.path.join(a.workdir, f"bk-{label}-{tag}.conf")
        # A fresh volume is the data file **and** its WAL directory: a log
        # left by an earlier invocation whose early segments were recycled
        # refuses the mount ("segment 0 is missing").
        if os.path.exists(self.data):
            os.remove(self.data)
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        with open(conf, "w") as f:
            # `cores` and `durability` default to BN's cell (1, relaxed); a
            # caller that varies them sets them on `a` (tools/bm_scan.py's
            # writer cell: 2, group, `reactor_cpus` pinning a reactor to a CPU each).
            f.write(f"data_file = {self.data}\nport = {port}\n"
                    f"cores = {getattr(a, 'cores', 1)}\n"
                    f"durability = {getattr(a, 'durability', 'relaxed')}\n"
                    f"buffer_pool_frames = 65536\n"
                    f"log_file = bk-{label}-{tag}.log\nlog_dir = {a.workdir}\n"
                    f"log_level = warn\n")
            if getattr(a, "reactor_cpus", None):
                f.write(f"reactor_cpus = {a.reactor_cpus}\n")
            # BM-Q5's switch; None writes nothing (the engine's default, and a
            # binary that predates the key refuses an unknown one).
            sk = scan_kernel if scan_kernel is not None else getattr(a, "scan_kernel", None)
            if sk is not None:
                f.write(f"scan_kernel = {sk}\n")
        env = dict(os.environ)
        if sampler_out:
            env["LD_PRELOAD"] = a.sampler
            env["KDS_SAMPLER_OUT"] = sampler_out
        cmd = ["taskset", "-c", str(a.pin_server), binary, "--config", conf]
        self.err = open(os.path.join(a.workdir, f"bk-{label}-{tag}.stderr"), "w")
        self.proc = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT, env=env)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=120)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server {label} did not listen on {port}")
                time.sleep(0.1)
        self.prepared = set()

    def stop(self):
        try:
            self.conn.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.err.close()


def must(conn, sql, tag):
    _, _, got, _ = conn.execute(sql)
    if not got.startswith(tag):
        raise SystemExit(f"setup: {sql!r} -> {got!r}")


def load(conn, name, rows, cols="id int64, v int64, w int64", row=None):
    must(conn, f"CREATE TABLE {name} ({cols}) BTREE", "CREATE")
    row = row or (lambda i: f"({i}, {i}, {i})")
    for lo in range(1, rows + 1, 500):
        hi = min(rows, lo + 499)
        must(conn, f"INSERT INTO {name} VALUES " + ",".join(row(i) for i in range(lo, hi + 1)),
             "INSERT")


def shapes(a):
    """(name, sql with ?, params(q), expected tag prefix, kind)."""
    i64 = kwp.Connection.int64
    n, sn = a.rows, a.scan_rows
    key = lambda q: 1 + (q * 7919) % n
    txt = lambda q: (kwp.T_VARCHAR, 0, (SPILL[:-8] + "%08d" % q).encode())
    return [
        ("noop", None, lambda q: [], "", "point"),
        ("ping", "SHOW META", lambda q: [], "", "point"),
        ("select-pk", "SELECT * FROM t WHERE id = ?", lambda q: [i64(key(q))], "SELECT", "point"),
        ("update-lit", "UPDATE t SET v = ? WHERE id = ?",
         lambda q: [i64(q), i64(key(q))], "UPDATED 1", "point"),
        ("update-expr", "UPDATE t SET v = v + 1 WHERE id = ?",
         lambda q: [i64(key(q))], "UPDATED 1", "point"),
        ("update-spill", "UPDATE sp SET v = ? WHERE id = ?",
         lambda q: [i64(q), i64(key(q))], "UPDATED 1", "point"),
        ("update-spill-assign", "UPDATE sa SET txt = ? WHERE id = ?",
         lambda q: [txt(q), i64(key(q))], "UPDATED 1", "point"),
        ("insert", "INSERT INTO i VALUES (?, ?, ?)",
         lambda q: [i64(10_000_000 + q)] * 3, "INSERT", "point"),
        ("delete-pk", "DELETE FROM d WHERE id = ?", lambda q: [i64(q)], "DELETED 1", "point"),
        ("update-range", "UPDATE r SET v = ? WHERE id >= 1",
         lambda q: [i64(q)], f"UPDATED {n}", "range"),
        ("scan", "SELECT * FROM s WHERE w = ?",
         lambda q: [i64(1 + (q * 7919) % sn)], "SELECT", "scan"),
    ]


def literal_of(sql, params):
    out, it = [], iter(params)
    for ch in sql:
        if ch != "?":
            out.append(ch)
            continue
        oid, _, raw = next(it)
        out.append("'" + raw.decode() + "'" if oid == kwp.T_VARCHAR
                   else str(struct.unpack("<q", raw)[0]))
    return "".join(out)


def run_block(srv, shape, path, qs, acc):
    """Runs the statements numbered `qs` of one arm on `srv`, adding to `acc`."""
    name, sql, params, tag, _ = shape
    conn = srv.conn
    stmt = None
    if path == "prepared":
        stmt = f"s_{name}"
        if stmt not in srv.prepared:
            conn.prepare(stmt, sql)
            srv.prepared.add(stmt)
    s0, c0, w0 = sched_ns(srv.proc.pid), self_ns(), time.perf_counter()
    for q in qs:
        p = params(q)
        t0 = time.perf_counter()
        if sql is None:
            ok = conn.ping()
            acc["lat"].append(time.perf_counter() - t0)
            acc["bad"] += 0 if ok else 1
            continue
        if stmt:
            _, rows, got, _ = conn.execute_prepared(stmt, p)
        else:
            _, rows, got, _ = conn.execute(literal_of(sql, p))
        acc["lat"].append(time.perf_counter() - t0)
        if not got.startswith(tag) or (name in ("select-pk", "scan") and len(rows) != 1):
            acc["bad"] += 1
    acc["wall"] += time.perf_counter() - w0
    block_ns = sched_ns(srv.proc.pid) - s0
    acc["server_ns"] += block_ns
    acc["driver_ns"] += self_ns() - c0
    # Per block, so an arm whose cost grows as it runs (a lengthening chain)
    # shows it: the first block against the last.
    acc.setdefault("block_server_us", []).append(round(block_ns / max(1, len(qs)) / 1e3, 2))


def summarize(name, path, label, acc):
    ops = len(acc["lat"])
    lat = acc["lat"]
    return {"shape": name, "path": path, "bin": label, "ops": ops, "bad": acc["bad"],
            "p0_us": min(lat) * 1e6, "p25_us": pct(lat, 25) * 1e6,
            "p50_us": pct(lat, 50) * 1e6, "p90_us": pct(lat, 90) * 1e6,
            "p99_us": pct(lat, 99) * 1e6, "mean_us": acc["wall"] / ops * 1e6,
            "server_cpu_us": acc["server_ns"] / ops / 1e3,
            "driver_cpu_us": acc["driver_ns"] / ops / 1e3,
            "block_server_us": acc.get("block_server_us", [])}


def setup(a, conn):
    load(conn, "t", a.rows)
    load(conn, "r", a.rows)
    load(conn, "s", a.scan_rows)
    for name in ("sp", "sa"):
        load(conn, name, a.rows, "id int64, v int64, txt varchar",
             lambda i: f"({i}, {i}, '{SPILL}')")
    must(conn, "CREATE TABLE i (id int64, v int64, w int64) BTREE", "CREATE")
    must(conn, "CREATE TABLE d (id int64, v int64, w int64) BTREE", "CREATE")


def preload_deletes(conn, first, count):
    """The rows `delete-pk` deletes, keys `first..first+count-1`, untimed."""
    for lo in range(first, first + count, 500):
        hi = min(first + count, lo + 500)
        must(conn, "INSERT INTO d VALUES " + ",".join(f"({k}, {k}, {k})" for k in range(lo, hi)),
             "INSERT")


def ops_for(a, kind):
    return {"point": a.ops, "range": a.range_ops, "scan": a.scan_ops}[kind]


def end_state(conn, a, q_range_last):
    """The every-row arm's last value and the spill arm's values, read back."""
    _, rows, _, _ = conn.execute("SELECT v FROM r WHERE id = 1")
    v = struct.unpack("<q", rows[0][0])[0] if rows else None
    _, rows, _, _ = conn.execute("SELECT txt FROM sp WHERE id = 1")
    return {"range_v_ok": v == q_range_last,
            "spill_txt_ok": bool(rows) and rows[0][0].decode() == SPILL}


def cpu_times():
    """Per-CPU (busy, total) jiffies from /proc/stat: busy is everything but
    idle and iowait. Differenced around a cell, the CPUs the measurement does
    not use (the server's and the driver's SMT siblings, and the rest) say
    whether another session ran beside it."""
    out = {}
    with open("/proc/stat") as f:
        for line in f:
            if line.startswith("cpu") and line[3].isdigit():
                v = line.split()
                n = [int(x) for x in v[1:]]
                out[int(v[0][3:])] = (sum(n[:8]) - n[3] - n[4], sum(n[:8]))
    return out


def cpu_busy_pct(c0, c1):
    """{cpu: busy % between two cpu_times() reads}."""
    return {k: round(100.0 * (c1[k][0] - c0[k][0]) / max(1, c1[k][1] - c0[k][1]), 1) for k in c1}


def _cpu_list(spec):
    return [int(x) for x in str(spec).split(",") if x != ""]


def siblings_of(cpu):
    """The SMT siblings of `cpu` (itself excluded), from sysfs."""
    try:
        with open(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list") as f:
            text = f.read().strip()
    except OSError:
        return []
    out = []
    for part in text.split(","):
        lo, _, hi = part.partition("-")
        out.extend(range(int(lo), int(hi or lo) + 1))
    return [c for c in out if c != cpu]


def measurement_cpus(a):
    """(own, siblings): the CPUs the server (or PostgreSQL backend) and the
    driver are pinned to, and the SMT siblings of those that nothing of the
    measurement uses. Another session's work on the siblings, or on the pinned
    CPUs themselves, is what a quiet cell must not have."""
    own = set(_cpu_list(getattr(a, "pin_server", None) or getattr(a, "pin_backend", "2")))
    own |= set(_cpu_list(a.pin_driver))
    sib = set()
    for c in own:
        sib |= set(siblings_of(c))
    return own, sib - own


def wait_quiet(a):
    """With `--cell-quiet-pct P` > 0: waits for a two-second window in which
    every CPU of the measurement and every sibling of one is at most P % busy
    (our own idle servers burn under 1 % each), for at most
    `--cell-quiet-wait` seconds. Returns the seconds waited."""
    limit = getattr(a, "cell_quiet_pct", 0)
    if limit <= 0:
        return 0.0
    own, sib = measurement_cpus(a)
    watch = own | sib
    t0 = time.time()
    while True:
        c0 = cpu_times()
        time.sleep(2.0)
        busy = cpu_busy_pct(c0, cpu_times())
        if all(busy.get(c, 0) <= limit for c in watch):
            return round(time.time() - t0, 1)
        if time.time() - t0 >= getattr(a, "cell_quiet_wait", 600):
            return round(time.time() - t0, 1)


def clean_cell(a, fn, retriable=True, pids=()):
    """Runs `fn()` - one cell on every arm, returning its result - and records
    the host around it: the 1-minute load, any competing build or test run,
    and every CPU's busy % over the cell. With `--cell-quiet-pct` > 0 the cell
    starts in a quiet window (`wait_quiet`) and is *clean* when the SMT
    siblings of the measurement's CPUs stayed at or under
    `--cell-clean-pct` % busy while it ran; an unclean cell is run again, up
    to `--cell-retries` more times, when `retriable` (a cell whose statements
    can be repeated: reads, and writes that set a value rather than insert or
    delete a key), and kept with `clean = False` after that. With `pids` (the
    measured processes, all pinned to `--pin-server`; this driver thread is
    pinned to `--pin-driver`) it also records the *foreign* busy % of those
    pinned CPUs - /proc/stat's busy % less what the measured tasks' own
    schedstat accounts for, i.e. another session's task on the CPU itself -
    and a cell above `--cell-foreign-pct` is unclean too. `wait_pct`, the
    measured threads' run-queue wait over their on-CPU time, is recorded and
    not gated on: the server's own threads share its one CPU, so it is never
    zero. Returns (result, host record)."""
    limit = getattr(a, "cell_quiet_pct", 0)
    waited_total, attempts = 0.0, 0
    while True:
        attempts += 1
        waited_total += wait_quiet(a)
        host = host_state()
        c0 = cpu_times()
        w0 = [sched_stat(p) for p in pids] + [self_stat()]
        t0 = time.perf_counter()
        res = fn()
        wall_ns = (time.perf_counter() - t0) * 1e9
        w1 = [sched_stat(p) for p in pids] + [self_stat()]
        busy = cpu_busy_pct(c0, cpu_times())
        own, sib = measurement_cpus(a)
        sib_busy = sum(busy.get(c, 0) for c in sib) / max(1, len(sib))
        cpu_d = sum(y[0] - x[0] for x, y in zip(w0, w1))
        wait_pct = 100.0 * sum(y[1] - x[1] for x, y in zip(w0, w1)) / max(1, cpu_d)
        srv_cpus = _cpu_list(getattr(a, "pin_server", None) or getattr(a, "pin_backend", "2"))
        drv_cpus = _cpu_list(a.pin_driver)
        srv_own = sum(y[0] - x[0] for x, y in zip(w0[:-1], w1[:-1]))
        drv_own = w1[-1][0] - w0[-1][0]
        foreign_srv = (sum(busy.get(c, 0) for c in srv_cpus) / len(srv_cpus)
                       - 100.0 * srv_own / (wall_ns * len(srv_cpus)))
        foreign_drv = (sum(busy.get(c, 0) for c in drv_cpus) / len(drv_cpus)
                       - 100.0 * drv_own / (wall_ns * len(drv_cpus)))
        host.update({"cpu_busy_pct": busy, "sibling_busy_pct": round(sib_busy, 1),
                     "wait_pct": round(wait_pct, 2), "foreign_server_cpu_pct": round(foreign_srv, 1),
                     "foreign_driver_cpu_pct": round(foreign_drv, 1),
                     "attempts": attempts, "quiet_waited_s": round(waited_total, 1)})
        fp = getattr(a, "cell_foreign_pct", 5.0)
        host["clean"] = limit <= 0 or (sib_busy <= getattr(a, "cell_clean_pct", 20) and
                                       foreign_srv <= fp and foreign_drv <= fp)
        if host["clean"] or not retriable or attempts > getattr(a, "cell_retries", 3):
            return res, host


def quiet_gate(a):
    """Waits until the 1-minute load average is at or under `--quiet-load`,
    for at most `--quiet-wait` seconds; returns (waited s, load at the end)."""
    t0 = time.time()
    while True:
        with open("/proc/loadavg") as f:
            load = float(f.read().split()[0])
        if a.quiet_load <= 0 or load <= a.quiet_load or time.time() - t0 >= a.quiet_wait:
            return round(time.time() - t0, 1), load
        time.sleep(5)


def wait_idle(srv, limit_s=90.0, ceiling_us_per_s=10000):
    """Waits for the server's CPU to fall to idle burn (the checkpoint and the
    pool's background writeback after a load): two consecutive one-second
    windows at or under the ceiling. Returns (waited s, last us/s)."""
    t0 = time.time()
    quiet = 0
    while True:
        s0 = sched_ns(srv.proc.pid)
        time.sleep(1.0)
        us = (sched_ns(srv.proc.pid) - s0) / 1e3
        quiet = quiet + 1 if us <= ceiling_us_per_s else 0
        if quiet >= 2 or time.time() - t0 >= limit_s:
            return round(time.time() - t0, 1), round(us, 1)


def parse_arms(a):
    """[(label, binary, port, scan_kernel)] from `--arm`, or from `--bin` and
    `--bin-b` (labels A and B). `--arm LABEL=BINARY[,scan_kernel=on|off]`."""
    if getattr(a, "arm", None):
        out = []
        for i, spec in enumerate(a.arm):
            label, rest = spec.split("=", 1)
            binary, *opts = rest.split(",")
            sk = None
            for o in opts:
                k, v = o.split("=", 1)
                if k != "scan_kernel" or v not in ("on", "off"):
                    raise SystemExit(f"--arm {spec!r}: only scan_kernel=on|off is an option")
                sk = v
            out.append((label, binary, a.port + i, sk))
        if len({x[0] for x in out}) != len(out):
            raise SystemExit("--arm labels must be distinct")
        return out
    return [("A", a.bin, a.port, None)] + ([("B", a.bin_b, a.port + 1, None)] if a.bin_b else [])


def rotated(servers, b, r):
    """Block `b` of run `r`: the servers rotated one place per block and per
    run, so each takes each position equally often (two servers: the old
    alternation)."""
    k = (b + r) % len(servers)
    return servers[k:] + servers[:k]


def one_run(a, r):
    tag = f"r{r}"
    waited, load = quiet_gate(a)
    before = host_state()
    bins = parse_arms(a)
    servers = []
    out = {"run": r, "host_before": before, "arms": [], "idle_cpu_us_per_s": {},
           "quiet_gate": {"waited_s": waited, "load": load}}
    try:
        for label, binary, port, sk in bins:
            servers.append(Server(a, label, binary, port, tag, scan_kernel=sk))
        for srv in servers:
            setup(a, srv.conn)
        for srv in servers:
            wait_idle(srv)
        for srv in servers:
            s0 = sched_ns(srv.proc.pid); time.sleep(1.0)
            out["idle_cpu_us_per_s"][srv.label] = (sched_ns(srv.proc.pid) - s0) / 1e3
        q = 1
        range_last = None
        for sh in shapes(a):
            if a.only and sh[0] not in a.only.split(","):
                continue
            ops = ops_for(a, sh[4])
            paths = ["literal", "prepared"] if r % 2 == 0 else ["prepared", "literal"]
            for path in paths:
                if sh[1] is None and path == "prepared":
                    continue  # C_PING has no statement to prepare
                per = max(1, ops // a.blocks)
                if sh[0] == "delete-pk":
                    for srv in servers:
                        preload_deletes(srv.conn, q, a.blocks * per)
                # bench/README.md rule 4, per cell: the host record is `clean_cell`'s.
                def do_cell(sh=sh, path=path, per=per, q=q):
                    accs = {s.label: {"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0,
                                      "driver_ns": 0} for s in servers}
                    for b in range(a.blocks):
                        qs = range(q + b * per, q + (b + 1) * per)
                        for srv in rotated(servers, b, r):
                            run_block(srv, sh, path, qs, accs[srv.label])
                    return accs
                accs, host = clean_cell(a, do_cell, sh[0] not in ("insert", "delete-pk"),
                                        [s.proc.pid for s in servers])
                if sh[0] == "update-range":
                    range_last = q + a.blocks * per - 1
                q += a.blocks * per
                for srv in servers:
                    res = summarize(sh[0], path, srv.label, accs[srv.label])
                    res["host"] = host
                    out["arms"].append(res)
                    print(json.dumps({k: (round(v, 2) if isinstance(v, float) else v)
                                      for k, v in res.items()
                                      if k not in ("host", "block_server_us")}), flush=True)
        for srv in servers:
            out.setdefault("end_state", {})[srv.label] = end_state(srv.conn, a, range_last)
            # The spill's page growth, against the same updates of an unspilled row.
            for name, tbl in (("pages-unspilled", "t"), ("pages-spilled", "sp")):
                p0 = meta(srv.conn, "pages_allocated")
                for k in range(1, a.rows + 1):
                    srv.conn.execute(f"UPDATE {tbl} SET v = {k} WHERE id = {k}")
                p1 = meta(srv.conn, "pages_allocated")
                out.setdefault(name, {})[srv.label] = {"updates": a.rows,
                                                       "pages_allocated_delta": p1 - p0}
            print(srv.label, out["end_state"][srv.label], out["pages-unspilled"][srv.label],
                  out["pages-spilled"][srv.label], flush=True)
    finally:
        for srv in servers:
            srv.stop()
    out["host_after"] = host_state()
    return out


def profile(a):
    os.makedirs(a.sampler_dir, exist_ok=True)
    base = os.path.join(a.sampler_dir, "dump")
    srv = Server(a, "prof", a.bin, a.port, "prof", sampler_out=base)
    dumps = []
    try:
        setup(a, srv.conn)
        q = 1
        only = set(a.profile_only.split(",")) if a.profile_only else None
        for sh in shapes(a):
            if only is not None and sh[0] not in only:
                continue
            for path in ("literal", "prepared"):
                if sh[1] is None and path == "prepared":
                    continue
                ops = {"point": a.profile_ops,
                       "range": a.profile_range_ops or max(1, a.profile_ops // 500),
                       "scan": a.profile_scan_ops or max(1, a.profile_ops // 50)}[sh[4]]
                acc = {"lat": [], "bad": 0, "wall": 0.0, "server_ns": 0, "driver_ns": 0}
                if sh[0] == "delete-pk":
                    preload_deletes(srv.conn, q, ops)
                os.kill(srv.proc.pid, signal.SIGUSR1)
                run_block(srv, sh, path, range(q, q + ops), acc)
                os.kill(srv.proc.pid, signal.SIGUSR2)
                time.sleep(0.5)
                q += ops
                res = summarize(sh[0], path, "prof", acc)
                res["dump"] = f"{base}.{len(dumps)}"
                dumps.append(res)
                print(json.dumps(res), flush=True)
    finally:
        srv.stop()
    return dumps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--bin-b")
    ap.add_argument("--arm", action="append",
                    help="LABEL=BINARY[,scan_kernel=on|off], repeatable; replaces --bin/--bin-b")
    ap.add_argument("--scan-kernel", choices=["on", "off"],
                    help="write `scan_kernel = <v>` into every server's config (default: not written)")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=31931)
    ap.add_argument("--pin-server", default="2")
    ap.add_argument("--pin-driver", default="4")
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--scan-rows", type=int, default=10000)
    ap.add_argument("--ops", type=int, default=3000)
    ap.add_argument("--scan-ops", type=int, default=200)
    ap.add_argument("--range-ops", type=int, default=60)
    ap.add_argument("--blocks", type=int, default=12)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--quiet-load", type=float, default=0.0,
                    help="before each run, wait for the 1-min loadavg to reach this (0: no gate)")
    ap.add_argument("--quiet-wait", type=float, default=600.0)
    ap.add_argument("--cell-quiet-pct", type=float, default=0.0,
                    help="before each cell wait for a window in which the measurement's CPUs and "
                    "their SMT siblings are at most this %% busy (0: no wait)")
    ap.add_argument("--cell-quiet-wait", type=float, default=600.0,
                    help="at most this many seconds per attempt (then the cell runs, flagged)")
    ap.add_argument("--cell-clean-pct", type=float, default=20.0,
                    help="a cell is clean when its SMT siblings averaged at most this %% busy")
    ap.add_argument("--cell-foreign-pct", type=float, default=5.0,
                    help="a cell is clean when another session's tasks took at most this %% of "
                    "the server's and of the driver's pinned CPU")
    ap.add_argument("--cell-retries", type=int, default=3,
                    help="an unclean cell is run again this many times (reads and idempotent writes)")
    ap.add_argument("--sampler")
    ap.add_argument("--sampler-dir")
    ap.add_argument("--profile-ops", type=int, default=100000)
    ap.add_argument("--profile-range-ops", type=int, default=0)
    ap.add_argument("--profile-scan-ops", type=int, default=0)
    ap.add_argument("--profile-only", help="comma-separated shape names")
    ap.add_argument("--only", help="census: comma-separated shape names (default: every shape)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not a.bin and not a.arm:
        ap.error("one of --bin or --arm is required")
    os.sched_setaffinity(0, {int(a.pin_driver)})
    result = {"args": vars(a), "runs": []}
    if a.sampler:
        result["profile"] = profile(a)
    for r in range(a.runs):
        result["runs"].append(one_run(a, r))
        with open(a.out, "w") as f:
            json.dump(result, f, indent=1)
    with open(a.out, "w") as f:
        json.dump(result, f, indent=1)


if __name__ == "__main__":
    main()
