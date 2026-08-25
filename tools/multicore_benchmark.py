#!/usr/bin/env python3
"""Multi-core isolation benchmark: N non-interfering relations, one client
connection each, INSERT / point-SELECT / UPDATE / DELETE / scan per
relation, run concurrently. Compares `cores = 1` against `cores = N`.

Two shapes, and which one runs is decided by the flags:

* `--placement creating` (default): every relation is core 0's and core 0
  serves every statement whatever `cores` says (docs/crosscore.md P6), so the
  honest expectation is parity. The harness's original shape, kept as the
  control.

* `--placement rotate --peer-listeners`: the per-core writer shape
  (docs/workplan-peer-writer.md PW6). Relations rotate over the peer cores,
  every core listens (`peer_listeners = on`, PW5), and each relation is
  written from a connection **the kernel accepted on its owner core** - a
  client cannot choose its core under SO_REUSEPORT, so the driver opens
  connections until every needed core has enough, asks each one `SHOW META`
  for its `core=`, and reports how many it had to open. DDL still runs on
  core 0 only, so the setup connection is found the same way.

  `rotate` without `--peer-listeners` is probed and reported as NOT RUN:
  the relations sit on peers and core 0's connection may not write them.

Usage:
    tools/multicore_benchmark.py --server build-release/kds_server \
        --cores 2 --tables 4 --rows 2000 --workdir /tmp/mcbench
    tools/multicore_benchmark.py --server build-release/kds_server \
        --cores 3 --tables 2 --rows 2000 --placement rotate --peer-listeners

Starts two fresh server instances itself (cores=1, then cores=N), each on
its own data file and port, and prints one comparison table. The data
file's device is the caller's business (bench/docs/README.md: a block
device, never tmpfs) - `--workdir` is where it goes.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import Phase  # noqa: E402


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


def wait_for_port(port, deadline_s=15):
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"server did not listen on {port}")


def start_server(binary, workdir, tag, cores, port, placement="creating",
                 peer_listeners=False):
    """Fresh data file + config, returns the process. `cores` is pinned into
    the superblock at bootstrap, so each configuration needs its own file."""
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\n"
                f"placement = {placement}\n"
                f"peer_listeners = {'on' if peer_listeners else 'off'}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    proc = subprocess.Popen([binary, "--config", conf],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    wait_for_port(port)
    return proc


def session_core(conn):
    """The core serving `conn`, from SHOW META's `core=` (docs/client-manual.md)."""
    meta = conn.cmd("SHOW META")
    for tok in meta.split():
        if tok.startswith("core="):
            return int(tok[len("core="):])
    raise RuntimeError(f"SHOW META carries no core= field: {meta}")


def collect_connections(port, needed, max_attempts):
    """Opens connections until every core in `needed` (core -> count) has that
    many, closing the rest. The kernel distributes SO_REUSEPORT accepts, so
    this is the only way a client gets a session on a chosen core (PW5:
    "clients cannot choose their core"). Returns ({core: [Conn]}, attempts).
    Raises after `max_attempts` opens - a core that never accepts is a
    finding, not something to spin on."""
    got = {core: [] for core in needed}
    attempts = 0
    while any(len(got[c]) < n for c, n in needed.items()):
        if attempts >= max_attempts:
            for conns in got.values():
                for c in conns:
                    c.close()
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
    return got, attempts


def is_retryable(reply):
    """The engine's retryable refusals: the wire's `retryable=1` bit
    (docs/protocol.md §11), and the row-id lease's exhaustion - a peer's
    first INSERT into a relation fails until the refill grant lands
    (docs/workplan-peer-writer.md PW1b), spelled as a retry in the message
    but not carrying the bit (a protocol inconsistency, recorded in the
    PW6 results rather than papered over here)."""
    return reply.startswith("ERR") and ("retryable=1" in reply or
                                        "retry after the refill grant lands" in reply)


def timed(conn, stmt, phase, retries, max_retries=2000, backoff_s=0.0005):
    """One statement, retried while the engine says retry. The latency
    recorded is the whole wait - what a client actually experienced -
    and the retry count is kept beside the phase, since a retry is a
    cost the percentiles alone would hide inside the tail."""
    t0 = time.perf_counter()
    r = conn.cmd(stmt)
    n = 0
    while is_retryable(r) and n < max_retries:
        n += 1
        time.sleep(backoff_s)
        r = conn.cmd(stmt)
    phase.record(time.perf_counter() - t0, r)
    retries[phase.name] = retries.get(phase.name, 0) + n
    return r


def worker(conn, table, rows, phases, barrier, retries):
    """The per-relation workload. One connection, one relation - nothing this
    thread does touches another thread's relation."""
    try:
        barrier.wait()
        # INSERT rows (pk is engine-assigned; VALUES covers columns 1..n-1).
        for i in range(rows):
            timed(conn, f"INSERT INTO {table} VALUES ('u{i}', {i * 10})",
                  phases["insert"], retries)
        # Point SELECT by pk (ids are 1..rows in issue order).
        for i in range(1, rows + 1):
            timed(conn, f"SELECT * FROM {table} WHERE id = {i}", phases["point-select"],
                  retries)
        # UPDATE by pk.
        for i in range(1, rows + 1):
            timed(conn, f"UPDATE {table} SET balance = {i} WHERE id = {i}",
                  phases["update"], retries)
        # DELETE the odd half by pk (delete-marks; nothing is reclaimed).
        for i in range(1, rows + 1, 2):
            timed(conn, f"DELETE FROM {table} WHERE id = {i}", phases["delete"], retries)
        # One full scan at the end: the surviving half.
        timed(conn, f"SELECT * FROM {table} WHERE balance > 0", phases["scan"], retries)
    finally:
        conn.close()


def owner_core_of(describe_reply):
    for tok in describe_reply.split():
        if tok.startswith("owner_core="):
            return int(tok[len("owner_core="):])
    return None


def run_config(binary, workdir, tag, cores, port, tables, rows, placement="creating",
               peer_listeners=False, max_connects=256):
    """Returns (wall, all_phases, owner_cores, session_report) - or
    (None, reason, owner_cores, None) when the configuration cannot run."""
    proc = start_server(binary, workdir, tag, cores, port, placement, peer_listeners)
    try:
        # DDL is core 0's alone (PW4), and under peer listeners the kernel
        # may hand this connection to any core - so the setup session is
        # collected like the writers, by asking.
        if peer_listeners:
            got, ddl_attempts = collect_connections(port, {0: 1}, max_connects)
            setup = got[0][0]
        else:
            setup, ddl_attempts = Conn(port), 1
        names = [f"bench{i}" for i in range(tables)]
        owner_cores = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            owner_cores[name] = owner_core_of(setup.cmd(f"DESCRIBE {name}"))

        # Which connection writes which relation: its owner core's, under
        # peer listeners; the one core-0 session otherwise.
        if peer_listeners:
            needed = {}
            for core in owner_cores.values():
                needed[core] = needed.get(core, 0) + 1
            per_core, writer_attempts = collect_connections(port, needed, max_connects)
            writers = {}
            for name in names:
                writers[name] = per_core[owner_cores[name]].pop()
            session_report = (f"ddl session on core 0 after {ddl_attempts} connection(s); "
                              f"{len(names)} writer session(s) on cores "
                              f"{sorted(needed)} after {writer_attempts} connection(s)")
        else:
            # **Can this configuration run the workload at all?** With
            # `placement = rotate` and no peer listener the relations sit on
            # cores no connection reaches, and a write to a peer-owned
            # relation from core 0 is refused (crosscore.md CC3). Probed
            # with one statement and reported as a finding, because an
            # error storm from N threads x rows says the same thing far
            # less clearly.
            probe = setup.cmd(f"INSERT INTO {names[0]} VALUES ('probe', 1)")
            if probe.startswith("ERR"):
                setup.cmd("STOP")   # owed even on the early out, or the wait below hangs
                setup.close()
                return None, probe, owner_cores, None
            writers = {name: Conn(port) for name in names}
            session_report = "every session on core 0"
        setup.close()

        # One Phase set per table so per-relation latencies stay separable,
        # plus the aggregate wall clock across all threads - the number
        # that would move if the cores actually shared the work.
        all_phases = {n: {p: Phase(p) for p in
                          ("insert", "point-select", "update", "delete", "scan")}
                      for n in names}
        retries = {n: {} for n in names}
        barrier = threading.Barrier(tables)
        threads = [threading.Thread(target=worker,
                                    args=(writers[n], n, rows, all_phases[n], barrier,
                                          retries[n]))
                   for n in names]
        t0 = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.perf_counter() - t0

        # STOP is accepted on any core and stops the instance (PW5's route).
        stop = Conn(port)
        stop.cmd("STOP")
        stop.close()
        total_retries = {}
        for per_table in retries.values():
            for phase, n in per_table.items():
                total_retries[phase] = total_retries.get(phase, 0) + n
        session_report += "; retries: " + (
            " ".join(f"{p}={n}" for p, n in sorted(total_retries.items())) or "none")
        return wall, all_phases, owner_cores, session_report
    finally:
        # A driver failure above never sent STOP; the server must not
        # outlive the run that started it (the next run wants the port).
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=15)


def summarize(tag, cores, wall, all_phases, owner_cores, tables, rows, session_report):
    total_stmts = sum(len(ph.latencies) for phases in all_phases.values()
                      for ph in phases.values())
    errors = sum(ph.errors for phases in all_phases.values() for ph in phases.values())
    print(f"\n== {tag}: cores={cores}, {tables} relations x {rows} rows ==")
    print("   placement: " + "  ".join(f"{n} owner_core={c}" for n, c in owner_cores.items()))
    print(f"   sessions: {session_report}")
    print(f"   wall={wall:.2f}s  aggregate={total_stmts / wall:,.0f} stmt/s  errors={errors}")
    for name in ("insert", "point-select", "update", "delete"):
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
    return total_stmts / wall


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--cores", type=int, default=2,
                    help="core count for the multi-core run")
    ap.add_argument("--tables", type=int, default=4)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15460)
    ap.add_argument("--workdir", default="/tmp/mcbench")
    ap.add_argument("--placement", choices=("creating", "rotate"), default="creating",
                    help="relation placement policy (docs/crosscore.md P6c). `rotate` "
                         "puts relations on peer cores; with --peer-listeners each is "
                         "written from a session on its owner core, without it the "
                         "driver probes and reports NOT RUN.")
    ap.add_argument("--peer-listeners", action="store_true",
                    help="run the multi-core configuration with `peer_listeners = on` "
                         "(PW5) and one writer session per relation on its owner core "
                         "(PW6). Needs --placement rotate.")
    ap.add_argument("--max-connects", type=int, default=256,
                    help="how many connections to open while hunting for sessions on "
                         "the needed cores before giving up (the kernel distributes)")
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
    for tag, cores, port, listeners in (("single-core", 1, args.port, False),
                                        ("multi-core", args.cores, args.port + 1,
                                         args.peer_listeners)):
        wall, phases, owners, sessions = run_config(
            binary, args.workdir, tag, cores, port, args.tables, args.rows,
            args.placement, listeners, args.max_connects)
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
                                 args.tables, args.rows, sessions)

    single, multi = results["single-core"], results["multi-core"]
    if single is None or multi is None:
        print("\n== comparison ==\n   not computed: a configuration could not run "
              "(see above)")
        return
    print(f"\n== comparison ==\n   multi-core / single-core throughput: "
          f"{multi / single:.3f}x")
    if args.placement == "creating":
        print("   (expected ~1.0x at placement=creating whatever the pipeline can do:\n"
              "    every relation is on core 0, so no statement ships - "
              "docs/crosscore.md P6)")
    elif args.cores == 2:
        print("   (rotation skips the system core, so at cores=2 every relation is\n"
              "    core 1's: this compares the peer write path against core 0's at\n"
              "    equal parallelism - a cost, not a scaling number; cores >= 3 is\n"
              "    where two writer cores exist)")


if __name__ == "__main__":
    main()
