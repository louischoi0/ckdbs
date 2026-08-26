#!/usr/bin/env python3
"""Is the insert phase's pace set by the WAL drain cadence rather than by the
device or by core count? A direct test, because the matrix can only show the
symptom.

The claim under test, stated so it can fail. `Expeditor::Config`'s
`wal_drain_interval_ns` defaults to 1 ms (include/kds/server/expeditor.hpp:424,
`wal_drain_interval_us = 1000` in kds.conf.sample) and every peer core is given
the same value (src/server/expeditor.cpp:1274). The group committer runs once
per reactor iteration as a post-task hook and again on that timer
(src/server/expeditor.cpp:1650-1660, src/server/core_runtime.cpp:718-734), and
a committing statement parks until it fires. If that cadence is what a
committing INSERT waits on, then:

  * insert throughput should rise roughly as the cadence shortens, and
  * the `cores = 4` : `cores = 1` ratio should stay near 1.0 at every cadence,
    because the cadence is per core and identical on all of them.

If instead the device or the core count were binding, throughput would flatten
as the cadence shortens and the ratio would move. Either outcome is a result.

This measures. It does not propose a value for the interval - that constant is
not this run's to pick, and a shorter cadence trades durability latency for
throughput in a way only the operator decides.

Usage:
    bench/drain_cadence_probe.py --server build-release/kds_server \
        --workdir ~/mcbench/cadence --tables 6 --rows 1000
"""

import argparse
import collections
import json
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    Conn, collect_connections, field, is_retryable, wait_for_port,
)


def percentiles(lats_us):
    if not lats_us:
        return {}
    s = sorted(lats_us)
    def at(p):
        return s[min(len(s) - 1, int(len(s) * p))]
    return dict(p50=round(at(0.50), 1), p99=round(at(0.99), 1),
                n=len(s))


def insert_worker(conn, name, rows, out, index, deadline_s):
    lats = []
    errors = 0
    retries = 0
    first_error = None
    for i in range(1, rows + 1):
        # The Keystone pk is implicit; a column list is refused
        # (tools/multicore_benchmark.py:288).
        stmt = f"INSERT INTO {name} VALUES ('o{i % 7}', {i * 10})"
        end = time.time() + deadline_s
        while True:
            t0 = time.perf_counter()
            r = conn.cmd(stmt)
            dt = (time.perf_counter() - t0) * 1e6
            if not r.startswith("ERR"):
                lats.append(dt)
                break
            # A peer answers its first INSERT with a lease-refill refusal
            # until the grant lands; those carry retryable=1 (PW1b).
            if is_retryable(r) and time.time() < end:
                retries += 1
                continue
            errors += 1
            if first_error is None:
                first_error = r
            break
    out[index] = dict(name=name, lat=lats, errors=errors, retries=retries,
                      first_error=first_error)


def run_one(args, cores, placement, listeners, drain_us, tag, port):
    workdir = os.path.join(args.workdir, tag)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, "s.conf")
    with open(conf, "w") as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\nplacement = {placement}\n"
                f"peer_listeners = {'on' if listeners else 'off'}\n"
                f"wal_drain_interval_us = {drain_us}\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
    stderr_path = os.path.join(workdir, "s.stderr")
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)
    try:
        wait_for_port(port, stderr_path)
        time.sleep(2)

        if listeners:
            got, _ = collect_connections(port, {0: 1}, args.max_connects)
            setup = got[0][0]
        else:
            setup = Conn(port)

        names = [f"cd{i}" for i in range(args.tables)]
        owners = {}
        for name in names:
            r = setup.cmd(f"CREATE TABLE {name} "
                          f"(id int64, owner varchar, balance int64) BTREE")
            if r.startswith("ERR"):
                raise RuntimeError(f"{name}: {r}")
            owners[name] = int(field(setup.cmd(f"DESCRIBE {name}"), "owner_core"))

        if listeners:
            needed = collections.Counter(owners.values())
            per_core, _ = collect_connections(port, needed, args.max_connects)
            writers = {n: per_core[owners[n]].pop() for n in names}
        else:
            writers = {n: Conn(port) for n in names}
        setup.close()

        out = [None] * len(names)
        threads = [threading.Thread(target=insert_worker,
                                    args=(writers[n], n, args.rows, out, i,
                                          args.retry_deadline))
                   for i, n in enumerate(names)]
        t0 = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        seconds = time.time() - t0

        lats = [x for r in out if r for x in r["lat"]]
        result = dict(tag=tag, cores=cores, drain_us=drain_us,
                      owner_cores=owners, seconds=round(seconds, 3),
                      inserts=len(lats),
                      inserts_per_second=round(len(lats) / seconds, 1)
                      if seconds else 0.0,
                      latency_us=percentiles(lats),
                      errors=sum(r["errors"] for r in out if r),
                      retries=sum(r["retries"] for r in out if r))
        # Rows in must equal rows out, per relation - a release build makes
        # no MayWrite check, so this is the only guard there is.
        bad = []
        for n in names:
            reply = writers[n].cmd(f"SELECT COUNT(*) FROM {n}")
            rows = [ln for ln in reply.replace("\\n", "\n").splitlines()
                    if ln.strip()]
            got = rows[-1].split(",")[-1].strip() if len(rows) > 1 else None
            if got != str(args.rows):
                bad.append(f"{n}: expected {args.rows} got {got!r}")
        result["lost_rows"] = bad

        try:
            writers[names[0]].cmd("STOP")
        except OSError:
            pass
    finally:
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="build-release/kds_server")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--tables", type=int, default=6)
    ap.add_argument("--rows", type=int, default=1000)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--port", type=int, default=16500)
    ap.add_argument("--cadences", default="1000,500,250,100")
    ap.add_argument("--max-connects", type=int, default=256)
    ap.add_argument("--retry-deadline", type=float, default=10.0)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    cadences = [int(c) for c in args.cadences.split(",") if c.strip()]
    os.makedirs(args.workdir, exist_ok=True)

    rows = []
    port = args.port
    for us in cadences:
        single = run_one(args, 1, "creating", False, us, f"s{us}", port)
        port += 2
        multi = run_one(args, args.cores, "rotate", True, us, f"m{us}", port)
        port += 2
        ratio = (multi["inserts_per_second"] / single["inserts_per_second"]
                 if single["inserts_per_second"] else None)
        rows.append(dict(drain_us=us, single=single, multi=multi,
                         ratio=round(ratio, 3) if ratio else None))
        print(f"drain={us:>5}us  single={single['inserts_per_second']:>8.1f} ips "
              f"(p50 {single['latency_us'].get('p50')}us)  "
              f"multi={multi['inserts_per_second']:>8.1f} ips "
              f"(p50 {multi['latency_us'].get('p50')}us)  ratio={ratio:.3f}  "
              f"lost={len(single['lost_rows']) + len(multi['lost_rows'])}",
              flush=True)

    print(json.dumps(rows, indent=2))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(rows, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
