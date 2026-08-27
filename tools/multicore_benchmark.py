#!/usr/bin/env python3
"""Multi-core isolation benchmark: N non-interfering relations, one client
connection each, INSERT / point-SELECT / UPDATE / DELETE / scan per
relation, run concurrently. Compares `cores = 1` against `cores = N`.

Two shapes, and which one runs is decided by the flags:

* `--placement creating` (default): every relation is core 0's and core 0
  serves every statement whatever `cores` says (docs/inflight/in-progress/workplan-crosscore.md
  P6c), so the honest expectation is parity. The harness's original shape,
  kept as the control.

* `--placement rotate --peer-listeners`: the per-core writer shape
  (docs/inflight/in-progress/workplan-peer-writer.md PW6). Relations rotate over the peer cores,
  every core listens (`peer_listeners = on`, PW5), and each relation is
  written from a connection **the kernel accepted on its owner core** - a
  client cannot choose its core under SO_REUSEPORT (docs/inflight/in-progress/workplan-peer-writer.md
  §5, kds.conf.sample), so the driver opens connections until every needed
  core has enough, asks each one `SHOW META` for its `core=`, and reports
  how many it had to open. DDL still runs on core 0 only, so the setup
  connection is found the same way.

  `rotate` without `--peer-listeners` is probed and reported as NOT RUN:
  the relations sit on peers and core 0's connection may not write them.

Usage:
    tools/multicore_benchmark.py --server build-release/kds_server \
        --cores 2 --tables 4 --rows 2000 --workdir ~/mcbench
    tools/multicore_benchmark.py --server build-release/kds_server \
        --cores 3 --tables 2 --rows 2000 --placement rotate --peer-listeners

Starts two fresh server instances itself (cores=1, then cores=N), each on
its own data file and port, and prints one comparison table. The data file
goes under `--workdir`, which must be a block device (bench/docs/README.md:
never tmpfs) on a quiet box - both are checked, `--force` overrides.
"""

import argparse
import collections
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase  # noqa: E402

PHASES = ("insert", "point-select", "update", "delete", "scan")


class Conn:
    """One newline-protocol connection: send a line, read one reply line."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buf = b""

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def field(reply, key):
    """`key=<int>` out of a keyed reply (SHOW META, DESCRIBE); absence is an
    error, never a silent None - a None would become a core nobody serves."""
    for tok in reply.split():
        if tok.startswith(key + "="):
            return int(tok[len(key) + 1:])
    raise RuntimeError(f"reply carries no {key}= field: {reply}")


def filesystem_of(path):
    """The fstype of the mount holding `path`, from /proc/mounts."""
    best, fs = "", "?"
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                mount = parts[1]
                # A path component match, not a string prefix: `/mnt/ssd` is
                # not the mount of `/mnt/ssdX`, and treating it as one names
                # the wrong filesystem - which is the tmpfs guard's whole job.
                if ((path == mount or path.startswith(mount.rstrip("/") + "/"))
                        and len(mount) > len(best)):
                    best, fs = mount, parts[2]
    except OSError:
        pass
    return fs


def check_host(workdir, force):
    """The two things that turn a number into fiction (bench/docs/README.md):
    a tmpfs data file makes fsync free, and a loaded box attributes other
    processes' preemption to the engine - the smoke run of this shape once
    measured a 1 ms point-SELECT that was a compiler on the other CPU."""
    fs = filesystem_of(os.path.abspath(workdir))
    if fs == "tmpfs" and not force:
        sys.exit(f"{workdir} is on tmpfs, where fsync is free; point --workdir at a real "
                 f"device (df -T tells you which), or pass --force")
    load1 = os.getloadavg()[0]
    cores = os.cpu_count() or 1
    if load1 > 0.5 * cores and not force:
        sys.exit(f"1-minute load is {load1:.2f} on {cores} core(s); wait for the box to go "
                 f"quiet, or pass --force")
    return fs, load1


def wait_for_port(port, stderr_path, deadline_s=15):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    # The server's own words, not just the silence: a refused configuration
    # (cores above the machine's, peer listeners with tls) says why here.
    tail = ""
    try:
        with open(stderr_path) as f:
            tail = "".join(f.readlines()[-5:]).strip()
    except OSError:
        pass
    raise TimeoutError(f"server did not listen on {port}" + (f":\n{tail}" if tail else ""))


def start_server(binary, workdir, tag, cores, port, placement="creating",
                 peer_listeners=False):
    """Fresh data file + config, returns the process. `cores` is pinned into
    the superblock at bootstrap, so each configuration needs its own file."""
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\n"
                f"placement = {placement}\n"
                f"peer_listeners = {'on' if peer_listeners else 'off'}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([binary, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc


def session_core(conn):
    """The core serving `conn`, from SHOW META's `core=` (docs/spec/client-manual.md)."""
    return field(conn.cmd("SHOW META"), "core")


def refill_summary(meta):
    """A peer's `<kind>_refill_*` fields off SHOW META, one clause per lease
    kind: requests/grants, and the longest wait with its two legs."""
    out = []
    for kind in ("rowid", "trxid", "extent"):
        if f"{kind}_refill_requests=" not in meta:
            continue
        f = {k: field(meta, f"{kind}_refill_{k}")
             for k in ("requests", "grants", "wait_max_us", "submit_lag_max_us",
                       "grant_lag_max_us", "resume_lag_max_us", "submit_lag_max_iters",
                       "grant_lag_max_iters", "resume_lag_max_iters")}  # every printed field
        out.append(f"{kind} {f['requests']}/{f['grants']} wait_max={f['wait_max_us'] / 1000:.1f}ms "
                   f"(submit {f['submit_lag_max_us'] / 1000:.1f}ms/{f['submit_lag_max_iters']}it, "
                   f"to-grant {f['grant_lag_max_us'] / 1000:.1f}ms/{f['grant_lag_max_iters']}it, "
                   f"resume {f['resume_lag_max_us'] / 1000:.1f}ms/{f['resume_lag_max_iters']}it)")
    return ", ".join(out) or "none reported"


def collect_connections(port, needed, max_attempts):
    """Opens connections until every core in `needed` (core -> count) has that
    many, closing the rest. The kernel distributes SO_REUSEPORT accepts, so
    this is the only way a client gets a session on a chosen core. Returns
    ({core: [Conn]}, attempts). Raises after `max_attempts` opens - a core
    that never accepts is a finding, not something to spin on."""
    got = {core: [] for core in needed}
    attempts = 0
    try:
        while any(len(got[c]) < n for c, n in needed.items()):
            if attempts >= max_attempts:
                short = {c: n - len(got[c]) for c, n in needed.items() if len(got[c]) < n}
                raise RuntimeError(f"after {attempts} connections the kernel never gave "
                                   f"these cores enough sessions: {short}")
            conn = Conn(port)
            attempts += 1
            core = session_core(conn)
            if core in got and len(got[core]) < needed[core]:
                got[core].append(conn)
            else:
                conn.close()
    except BaseException:
        for conns in got.values():
            for c in conns:
                c.close()
        raise
    return got, attempts


# The engine's refusals that mean "again, later" (docs/spec/protocol.md §11): the
# wire's `retryable=1` - except CC3's cross-core write refusal, which carries
# the bit and is permanent for a session on the wrong core
# (docs/inflight/in-progress/workplan-peer-writer.md §5: it repeats forever) - and the three lease
# exhaustions a peer answers until its refill grant lands: the row-id lease
# on a relation's first INSERT (PW1b), the trx-id lease, and the extent lease
# (a btree insert that could not allocate). Those three carry the bit since
# 2026-08-25 (they are TxnConflict now - docs/inflight/known-gaps.md closes PW6's
# finding (2)); the message matching below stays as the fallback that reads
# a server built before that, and is what kept this driver from losing rows
# to them at v2.0.0-48-g314a06d.
RETRY_TEXTS = ("retry after the refill grant lands",
               "a refill must be granted before it can allocate again")
PERMANENT_TEXTS = ("writes are bound to core",)


def is_retryable(reply):
    if not reply.startswith("ERR"):
        return False
    if any(t in reply for t in PERMANENT_TEXTS):
        return False
    return "retryable=1" in reply or any(t in reply for t in RETRY_TEXTS)


DEFAULT_RETRY_DEADLINE_S = 10.0


def timed(conn, stmt, phase, retries, deadline_s=DEFAULT_RETRY_DEADLINE_S, backoff_s=0.0005):
    """One statement, retried while the engine says retry, for at most
    `deadline_s` of wall clock - a bound in time, not attempts, so a refusal
    that never clears costs the deadline and a recorded error, not an hour.
    The default is well above the longest wait measured so far (1.75 s, the
    PW6 results) so a slow refill is a tail, not an error; a give-up is
    counted apart from the retries, as `<phase>-gave-up`, because a
    harness bound and an engine's hard refusal must not read alike. The
    latency recorded is the whole wait - what a client experienced - and the
    retry count is kept beside the phase, since a retry is a cost the
    percentiles alone would hide inside the tail."""
    t0 = time.perf_counter()
    r = conn.cmd(stmt)
    n = 0
    while is_retryable(r) and time.perf_counter() - t0 < deadline_s:
        n += 1
        time.sleep(backoff_s)
        r = conn.cmd(stmt)
    phase.record(time.perf_counter() - t0, r)
    # `.get`, not `+=`: a caller may pass a plain dict as well as a Counter
    # (bench/run_pw6.py's probes do), and `+=` on a missing key raises.
    if n:
        retries[phase.name] = retries.get(phase.name, 0) + n
    if is_retryable(r):
        retries[phase.name + "-gave-up"] = retries.get(phase.name + "-gave-up", 0) + 1
    return r


def retry_line(per_table_retries):
    """The report's retries line over per-table dicts or Counters; zeros
    dropped, so `none` means none."""
    total = collections.Counter()
    for per in per_table_retries:
        total.update(per)
    return "retries: " + (" ".join(f"{p}={n}" for p, n in sorted(total.items()) if n)
                          or "none")


def stop_server(port):
    """STOP is accepted on any core and stops the instance (PW5's route)."""
    conn = Conn(port)
    try:
        conn.cmd("STOP")
    finally:
        conn.close()


# ckdbs's INSERT: the Keystone pk is implicit. PostgreSQL's twin (a serial
# pk needs the column list) passes its own spelling; nothing else differs.
INSERT_FMT = "INSERT INTO {t} VALUES ('u{i}', {b})"


def worker(conn, table, rows, phases, barrier, retries, counts=None, insert_fmt=INSERT_FMT,
           deadline_s=DEFAULT_RETRY_DEADLINE_S):
    """The per-relation workload. One connection, one relation - nothing this
    thread does touches another thread's relation. `counts` is where the
    final COUNT(*) reply goes; None skips it (the PostgreSQL twin, whose
    reply shape differs)."""
    def run(stmt, phase):
        return timed(conn, stmt, phases[phase], retries, deadline_s)

    try:
        barrier.wait()
        # INSERT rows (pk is engine-assigned; VALUES covers columns 1..n-1).
        for i in range(rows):
            run(insert_fmt.format(t=table, i=i, b=i * 10), "insert")
        # Point SELECT by pk (ids are 1..rows in issue order).
        for i in range(1, rows + 1):
            run(f"SELECT * FROM {table} WHERE id = {i}", "point-select")
        # UPDATE by pk.
        for i in range(1, rows + 1):
            run(f"UPDATE {table} SET balance = {i} WHERE id = {i}", "update")
        # DELETE the odd half by pk (delete-marks; nothing is reclaimed).
        for i in range(1, rows + 1, 2):
            run(f"DELETE FROM {table} WHERE id = {i}", "delete")
        # One full scan at the end: the surviving half.
        run(f"SELECT * FROM {table} WHERE balance > 0", "scan")
        # What survived, from the session that wrote it: a lost INSERT shows
        # here by name, where an error count would only say "some".
        if counts is not None:
            counts[table] = conn.cmd(f"SELECT COUNT(*) FROM {table}")
    finally:
        conn.close()


def run_config(binary, workdir, tag, cores, port, tables, rows, placement="creating",
               peer_listeners=False, max_connects=256,
               retry_deadline_s=DEFAULT_RETRY_DEADLINE_S, force=False):
    """Returns (wall, all_phases, owner_cores, report) - or
    (None, reason, owner_cores, None) when the configuration cannot run."""
    # The host guard sits on the measuring path, not only under main(): the
    # wrapper that produced the PW6 numbers calls this directly.
    fs, load1 = check_host(workdir, force)
    proc = start_server(binary, workdir, tag, cores, port, placement, peer_listeners)
    try:
        # DDL is core 0's alone (PW4), and under peer listeners the kernel
        # may hand this connection to any core - so the setup session is
        # collected like the writers, by asking.
        if peer_listeners:
            got, ddl_attempts = collect_connections(port, {0: 1}, max_connects)
            setup = got[0][0]
        else:
            setup = Conn(port)
        names = [f"bench{i}" for i in range(tables)]
        owner_cores = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            owner_cores[name] = field(setup.cmd(f"DESCRIBE {name}"), "owner_core")

        # Which connection writes which relation: its owner core's, under
        # peer listeners; a core-0 session otherwise.
        if peer_listeners:
            needed = collections.Counter(owner_cores.values())
            per_core, writer_attempts = collect_connections(port, needed, max_connects)
            writers = {name: per_core[owner_cores[name]].pop() for name in names}
            sessions = (f"ddl session on core 0 after {ddl_attempts} connection(s); "
                        f"{len(names)} writer session(s) on cores {sorted(needed)} "
                        f"after {writer_attempts} connection(s)")
        else:
            writers = {name: Conn(port) for name in names}
            sessions = "every session on core 0"
        setup.close()

        # **Can this configuration run the workload at all?** One probe row
        # from the first relation's own writer, in both arms so they stay
        # the same workload: with `placement = rotate` and no peer listener
        # the relation sits on a core no session reaches, and core 0's
        # session is refused (crosscore.md CC3). Reported as a finding in
        # the engine's own words, because an error storm from N threads x
        # rows says the same thing far less clearly.
        # Retried like any statement: on a peer the first INSERT is refused
        # until the row-id refill lands, and that is the contract, not the
        # finding this probe exists to make. Timed into the first relation's
        # own insert phase, because it *is* that relation's first INSERT -
        # the one that pays the refill - and a throwaway phase would hide
        # exactly the wait the PW6 results are about.
        #
        # One Phase set per table so per-relation latencies stay separable,
        # plus the aggregate wall clock across all threads - the number
        # that would move if the cores actually shared the work.
        all_phases = {n: {p: Phase(p) for p in PHASES} for n in names}
        retries = {n: collections.Counter() for n in names}
        counts = {}
        probe = timed(writers[names[0]], f"INSERT INTO {names[0]} VALUES ('probe', 1)",
                      all_phases[names[0]]["insert"], retries[names[0]], retry_deadline_s)
        if probe.startswith("ERR"):
            for w in writers.values():
                w.close()
            stop_server(port)   # owed even on the early out, or the wait below hangs
            return None, probe, owner_cores, None

        barrier = threading.Barrier(tables)
        threads = [threading.Thread(target=worker,
                                    args=(writers[n], n, rows, all_phases[n], barrier,
                                          retries[n], counts, INSERT_FMT, retry_deadline_s))
                   for n in names]
        t0 = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.perf_counter() - t0

        # What the peer's lease refills cost, from its own SHOW META
        # (docs/spec/client-manual.md): requests, grants and the longest wait per
        # kind, split into the ring-and-core-0 leg and this reactor's
        # resume leg. Read after the workload from one fresh session per
        # writer core; the lease-refill trace's instrument.
        refills = None
        if peer_listeners:
            # A diagnostic read after the measurement must not lose the
            # measurement: the session hunt is a nondeterministic
            # SO_REUSEPORT draw, and a failed one is reported, not raised.
            try:
                lines = []
                per_core, _ = collect_connections(port, {c: 1 for c in needed}, max_connects)
                for core, conns in sorted(per_core.items()):
                    lines.append(f"core {core}: " + refill_summary(conns[0].cmd("SHOW META")))
                    conns[0].close()
                refills = "refills: " + "; ".join(lines)
            except (RuntimeError, OSError) as e:
                refills = f"refills: unavailable ({e})"

        stop_server(port)

        # Survivors: the even ids, plus the probe row in the first relation
        # (id 1, odd, deleted - so plus the last id, rows + 1, which the
        # delete never names). Each id inserted once, each odd id deleted.
        lost = []
        for name in names:
            expected = rows // 2 + (1 if name == names[0] else 0)
            got = counts.get(name, "")
            # A multi-line reply travels as one line with `\n` escaped
            # (docs/spec/client-manual.md): the header, then the value.
            try:
                n = int(got.replace("\\n", "\n").split("\n")[-1].split(",")[-1])
            except ValueError:
                n = None
            if n != expected:
                lost.append(f"{name} expected {expected} got {got!r}")
        report = [f"host: workdir on {fs}, 1-minute load {load1:.2f} at start",
                  sessions, refills or "refills: none (no peer listener)",
                  "verify: " + ("; ".join(lost) if lost else
                                f"survivors as expected ({rows // 2}, and one more in "
                                f"{names[0]} for the probe row)"),
                  # Retries last, after the verify line: bench/run_pw6.py
                  # reads them off the report's tail, and a quoted server
                  # reply above carries `key=<int>` fields of its own.
                  retry_line(retries.values())]
        return wall, all_phases, owner_cores, report
    finally:
        # A driver failure above never sent STOP; the server must not
        # outlive the run that started it (the next run wants the port).
        # SIGTERM is the graceful path - a final sync and checkpoint - so on
        # a large file it can outlast the wait; kill then, and never let the
        # cleanup's own timeout mask what failed above.
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=15)


def summarize(tag, cores, wall, all_phases, owner_cores, tables, rows, report):
    total_stmts = sum(len(ph.latencies) for phases in all_phases.values()
                      for ph in phases.values())
    errors = sum(ph.errors for phases in all_phases.values() for ph in phases.values())
    print(f"\n== {tag}: cores={cores}, {tables} relations x {rows} rows ==")
    print("   placement: " + "  ".join(f"{n} owner_core={c}" for n, c in owner_cores.items()))
    for line in report:
        print(f"   {line}")
    print(f"   wall={wall:.2f}s  aggregate={total_stmts / wall:,.0f} stmt/s  errors={errors}")
    for name in PHASES:
        lats = sorted(sum((phases[name].latencies for phases in all_phases.values()), []))
        if not lats:
            continue
        p50 = lats[len(lats) // 2] * 1e6
        p99 = lats[int(len(lats) * 0.99)] * 1e6
        print(f"   {name:<13} n={len(lats):>6}  p50={p50:>7.0f}us  p99={p99:>7.0f}us")
    if errors:
        first = next(ph.first_error for phases in all_phases.values()
                     for ph in phases.values() if ph.first_error)
        print(f"   first error: {first}")
    phase_stats = {}
    for name in PHASES:
        lats = sorted(sum((phases[name].latencies for phases in all_phases.values()), []))
        if not lats:
            continue
        phase_stats[name] = {
            "n": len(lats),
            "p50_us": lats[len(lats) // 2] * 1e6,
            "p99_us": lats[int(len(lats) * 0.99)] * 1e6,
        }
    return {
        "tag": tag, "cores": cores, "tables": tables, "rows": rows,
        "wall_s": wall, "statements": total_stmts, "errors": errors,
        "throughput_stmts_s": total_stmts / wall,
        "owner_cores": {str(n): c for n, c in owner_cores.items()},
        "phases": phase_stats,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--cores", type=int, default=2,
                    help="core count for the multi-core run (must be <= nproc; the "
                         "server refuses more, and this driver then sees only the "
                         "missing listener)")
    ap.add_argument("--tables", type=int, default=4)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15460)
    ap.add_argument("--workdir", default=os.path.expanduser("~/mcbench"),
                    help="where the data files go - a block device, never tmpfs")
    ap.add_argument("--placement", choices=("creating", "rotate"), default="creating",
                    help="relation placement policy (docs/inflight/in-progress/workplan-crosscore.md P6c). "
                         "`rotate` puts relations on peer cores; with --peer-listeners "
                         "each is written from a session on its owner core, without it "
                         "the driver probes and reports NOT RUN.")
    ap.add_argument("--peer-listeners", action="store_true",
                    help="run the multi-core configuration with `peer_listeners = on` "
                         "(PW5) and one writer session per relation on its owner core "
                         "(PW6). Needs --placement rotate.")
    ap.add_argument("--max-connects", type=int, default=256,
                    help="how many connections to open while hunting for sessions on "
                         "the needed cores before giving up (the kernel distributes)")
    ap.add_argument("--only", choices=("both", "single", "multi"), default="both",
                    help="run one configuration instead of both, so the ratio can be "
                         "computed from **separate processes**. The default runs "
                         "single-core then multi-core in this one process, and that "
                         "shape carries a measured ordering bias - a cores=1 against "
                         "cores=1 null cell returns 1.099, because the second arm "
                         "always runs later (SS-B finding 10). Every cell in "
                         "`bench/v2.1.0/results-multicore-writers-v2.1.0.md` was taken "
                         "with the default, so a `--only` ratio is unbiased but is NOT "
                         "directly comparable with those numbers.")
    ap.add_argument("--json", default="",
                    help="write this run's per-configuration summary here")
    ap.add_argument("--force", action="store_true",
                    help="run even on tmpfs or a loaded box")
    ap.add_argument("--retry-deadline", type=float, default=DEFAULT_RETRY_DEADLINE_S,
                    help="seconds a statement is retried while the engine says retry "
                         "before it is recorded as an error and a `<phase>-gave-up`")
    args = ap.parse_args()
    if args.peer_listeners and args.placement != "rotate":
        ap.error("--peer-listeners needs --placement rotate (the server refuses the "
                 "pairing too: with creating-core placement a peer serves nothing)")

    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir, exist_ok=True)
    binary = os.path.abspath(args.server)

    results = {}
    # The baseline never carries peer listeners: `cores = 1` has no peer to
    # listen, and the server refuses the pairing.
    configs = [("single-core", 1, args.port, False),
               ("multi-core", args.cores, args.port + 1, args.peer_listeners)]
    if args.only == "single":
        configs = configs[:1]
    elif args.only == "multi":
        configs = configs[1:]
    for tag, cores, port, listeners in configs:
        wall, phases, owners, report = run_config(
            binary, args.workdir, tag, cores, port, args.tables, args.rows,
            args.placement, listeners, args.max_connects, args.retry_deadline, args.force)
        if wall is None:
            # The write-capability probe refused: this configuration cannot
            # run the workload, and saying so is the result.
            print(f"\n== {tag}: cores={cores}, placement={args.placement} ==")
            print("   placement: " + "  ".join(f"{n} owner_core={c}"
                                               for n, c in owners.items()))
            print("   NOT RUN - the relations cannot be written from this connection:")
            print(f"     {phases}")
            print("   A rotated relation is written only from a session on its owner\n"
                  "   core (crosscore.md CC3; DML shipping is unbuilt), and without\n"
                  "   `peer_listeners = on` only core 0 accepts. Pass --peer-listeners\n"
                  "   for the per-core writer shape (workplan-peer-writer.md PW6).")
            results[tag] = None
            continue
        results[tag] = summarize(tag, cores, wall, phases, owners,
                                 args.tables, args.rows, report)

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"only": args.only, "cores": args.cores, "tables": args.tables,
                       "rows": args.rows, "placement": args.placement,
                       "peer_listeners": bool(args.peer_listeners),
                       "configs": results}, fh, indent=2)

    if args.only != "both":
        # One configuration per process is the point of --only: the ratio is
        # computed outside, from two runs that were never each other's
        # second arm.
        only = results[configs[0][0]]
        print(f"\n== {args.only} only ==\n   " +
              ("not run (see above)" if only is None else
               f"{only['throughput_stmts_s']:,.0f} stmt/s over {only['wall_s']:.2f}s"))
        return
    single, multi = results["single-core"], results["multi-core"]
    if single is None or multi is None:
        print("\n== comparison ==\n   not computed: a configuration could not run "
              "(see above)")
        return
    print(f"\n== comparison ==\n   multi-core / single-core throughput: "
          f"{multi['throughput_stmts_s'] / single['throughput_stmts_s']:.3f}x")
    if args.placement == "creating":
        print("   (expected ~1.0x at placement=creating whatever the pipeline can do:\n"
              "    every relation is on core 0, so no statement ships - "
              "docs/inflight/in-progress/workplan-crosscore.md P6c)")
    elif args.cores == 2:
        print("   (rotation skips the system core, so at cores=2 every relation is\n"
              "    core 1's: this compares the peer write path against core 0's at\n"
              "    equal parallelism - a cost, not a scaling number; cores >= 3 is\n"
              "    where two writer cores exist)")


if __name__ == "__main__":
    main()
