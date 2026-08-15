#!/usr/bin/env python3
"""Multi-core isolation benchmark: 4 non-interfering relations, one client
connection each, INSERT / point-SELECT / UPDATE / DELETE / scan per
relation, run concurrently. Compares `cores = 1` against `cores = N`.

What this can and cannot demonstrate today (docs/crosscore.md, workplan
P0-P8): relations are placed on the creating core, DDL runs on core 0, and
the step pipeline is unbuilt - so **core 0 serves every statement whatever
`cores` says**, and the honest expectation is parity between the two
configurations. The harness exists so that the day cross-core dispatch
lands, the same run demonstrates the scaling it was built to show: each
relation reports its `owner_core` (DESCRIBE), and the workload is already
shaped so no statement ever touches two relations.

Usage:
    tools/multicore_benchmark.py --server build-release/kds_server \
        --cores 2 --tables 4 --rows 2000 --workdir /tmp/mcbench

Starts two fresh server instances itself (cores=1, then cores=N), each on
its own data file and port, and prints one comparison table.
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


def start_server(binary, workdir, tag, cores, port, placement="creating"):
    """Fresh data file + config, returns the process. `cores` is pinned into
    the superblock at bootstrap, so each configuration needs its own file."""
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    with open(conf, "w") as f:
        f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\n"
                f"placement = {placement}\n"
                f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    proc = subprocess.Popen([binary, "--config", conf],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    wait_for_port(port)
    return proc


def worker(port, table, rows, phases, barrier):
    """The per-relation workload. One connection, one relation - nothing this
    thread does touches another thread's relation."""
    conn = Conn(port)
    try:
        barrier.wait()
        # INSERT rows (pk is engine-assigned; VALUES covers columns 1..n-1).
        ph = phases["insert"]
        for i in range(rows):
            t0 = time.perf_counter()
            r = conn.cmd(f"INSERT INTO {table} VALUES ('u{i}', {i * 10})")
            ph.record(time.perf_counter() - t0, r)
        # Point SELECT by pk (ids are 1..rows in issue order).
        ph = phases["point-select"]
        for i in range(1, rows + 1):
            t0 = time.perf_counter()
            r = conn.cmd(f"SELECT * FROM {table} WHERE id = {i}")
            ph.record(time.perf_counter() - t0, r)
        # UPDATE by pk.
        ph = phases["update"]
        for i in range(1, rows + 1):
            t0 = time.perf_counter()
            r = conn.cmd(f"UPDATE {table} SET balance = {i} WHERE id = {i}")
            ph.record(time.perf_counter() - t0, r)
        # DELETE the odd half by pk (delete-marks; nothing is reclaimed).
        ph = phases["delete"]
        for i in range(1, rows + 1, 2):
            t0 = time.perf_counter()
            r = conn.cmd(f"DELETE FROM {table} WHERE id = {i}")
            ph.record(time.perf_counter() - t0, r)
        # One full scan at the end: the surviving half.
        ph = phases["scan"]
        t0 = time.perf_counter()
        r = conn.cmd(f"SELECT * FROM {table} WHERE balance > 0")
        ph.record(time.perf_counter() - t0, r)
    finally:
        conn.close()


def run_config(binary, workdir, tag, cores, port, tables, rows, placement="creating"):
    proc = start_server(binary, workdir, tag, cores, port, placement)
    try:
        setup = Conn(port)
        names = [f"bench{i}" for i in range(tables)]
        owner_cores = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} (id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            d = setup.cmd(f"DESCRIBE {name}")
            owner = [tok for tok in d.split() if tok.startswith("owner_core=")]
            owner_cores[name] = owner[0] if owner else "owner_core=?"

        # **Can this configuration run the workload at all?** With
        # `placement = rotate` the relations land on peer cores, and a
        # write to a peer-owned relation is refused: cross-core writes are
        # a retryable refusal (crosscore.md CC3), DML statement shipping is
        # unbuilt, and only core 0 carries a listener - so there is no
        # connection from which those rows could be inserted. Probed with
        # one statement and reported as a finding, because an error storm
        # from 4 threads x N rows says the same thing far less clearly.
        probe = setup.cmd(f"INSERT INTO {names[0]} VALUES ('probe', 1)")
        if probe.startswith("ERR"):
            setup.cmd("STOP")   # owed even on the early out, or the wait below hangs
            setup.close()
            return None, probe, owner_cores
        setup.close()

        # One Phase set per table so per-relation latencies stay separable,
        # plus the aggregate wall clock across all threads - the number
        # that would move if the cores actually shared the work.
        all_phases = {n: {p: Phase(p) for p in
                          ("insert", "point-select", "update", "delete", "scan")}
                      for n in names}
        barrier = threading.Barrier(tables)
        threads = [threading.Thread(target=worker,
                                    args=(port, n, rows, all_phases[n], barrier))
                   for n in names]
        t0 = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        wall = time.perf_counter() - t0

        stop = Conn(port)
        stop.cmd("STOP")
        stop.close()
        return wall, all_phases, owner_cores
    finally:
        proc.wait(timeout=15)


def summarize(tag, cores, wall, all_phases, owner_cores, tables, rows):
    total_stmts = sum(len(ph.latencies) for phases in all_phases.values()
                      for ph in phases.values())
    errors = sum(ph.errors for phases in all_phases.values() for ph in phases.values())
    print(f"\n== {tag}: cores={cores}, {tables} relations x {rows} rows ==")
    print("   placement: " + "  ".join(f"{n} {c}" for n, c in owner_cores.items()))
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
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--cores", type=int, default=2,
                    help="core count for the multi-core run (must be <= nproc)")
    ap.add_argument("--tables", type=int, default=4)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15460)
    ap.add_argument("--workdir", default="/tmp/mcbench")
    ap.add_argument("--placement", choices=("creating", "rotate"), default="creating",
                    help="relation placement policy (docs/crosscore.md P6c). "
                         "`rotate` puts relations on peer cores, which is what a "
                         "cross-core statement needs - and which no connection can "
                         "currently populate; the driver probes and reports that "
                         "rather than producing an error storm.")
    args = ap.parse_args()

    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir, exist_ok=True)
    binary = os.path.abspath(args.server)

    results = {}
    for tag, cores, port in (("single-core", 1, args.port),
                             (f"multi-core", args.cores, args.port + 1)):
        wall, phases, owners = run_config(binary, args.workdir, tag, cores, port,
                                          args.tables, args.rows, args.placement)
        if wall is None:
            # The write-capability probe refused: this configuration cannot
            # run the workload, and saying so is the result.
            print(f"\n== {tag}: cores={cores}, placement={args.placement} ==")
            print("   placement: " + "  ".join(f"{n} {c}" for n, c in owners.items()))
            print(f"   NOT RUN - the relations cannot be written from this connection:")
            print(f"     {phases}")
            print("   A peer-owned relation has no writer today: cross-core writes are\n"
                  "   refused (crosscore.md CC3), DML statement shipping is unbuilt, and\n"
                  "   core 0 alone carries a listener. So `placement = rotate` cannot be\n"
                  "   populated over the wire, and this driver cannot show the pipeline\n"
                  "   scaling until one of those lands. The pipeline's CPU cost is\n"
                  "   measured instead by bench/crosscore_pipeline_bench.cpp, which builds\n"
                  "   its rows in-process (workplan P4e).")
            results[tag] = None
            continue
        results[tag] = summarize(tag, cores, wall, phases, owners,
                                 args.tables, args.rows)

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


if __name__ == "__main__":
    main()
