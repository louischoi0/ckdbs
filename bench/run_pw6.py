#!/usr/bin/env python3
"""The PW6 per-core writer benchmark's matrix runner, twin and report
(docs/inflight/in-progress/workplan-peer-writer.md §6, row PW6; results in
bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md).

Wraps tools/multicore_benchmark.py without modifying it: the driver is
imported and `run_config` called directly, which hands back every
per-statement latency instead of the driver's p50/p99 print. On top of it:

  --matrix   the interleaved cells (A control / B rotate x2 / C rotate x4),
             `--reps` times in A,B,C,A,B,C,... order, each configuration
             gated on a quiet box (no compiler, no test binary, 1-minute
             load under --quiet-load) and stamped with the load it started
             at. One workdir per invocation, fresh server and data file per
             configuration (the driver's own rule).
  --cell     one ckdbs cell, once.
  --pg       the PostgreSQL twin of one cell: N tables x M rows, one
             connection per table, the identical statement sequence timed by
             the identical `timed()` - there is no tools/pg_multicore_
             benchmark.py, so the twin lives here until one is built.
  --probes   the wait-breakdown probes: fdatasync on the data device (the
             commit wait's floor), and the SHOW META round trip on a core-0
             session and on a core-1 session (the client+socket floor for
             each side of the comparison).
  --report   read every result.json under --workdir and print the markdown
             tables the results file is built from.

Every timed cell needs the copied binary (`--binary`), never
build-release's own - bench/docs/README.md says why.
"""

import argparse
import glob
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import multicore_benchmark as mb  # noqa: E402
from bench_common import Phase, nearest_rank  # noqa: E402

PHASES = mb.PHASES  # the driver's order; the report slices off "scan" as the last
PCTS = (0, 25, 50, 75, 90, 95, 99, 100)
PG_INSERT = "INSERT INTO {t} (owner, balance) VALUES ('u{i}', {b})"

# The three cells the results file compares. A is the control (both
# configurations serve on core 0, parity expected); B is the PW6 shape at
# the parallelism this host allows; C the same with twice the writers.
CELLS = {
    "A-creating-t2": dict(placement="creating", peer_listeners=False, tables=2),
    "B-rotate-t2": dict(placement="rotate", peer_listeners=True, tables=2),
    "C-rotate-t4": dict(placement="rotate", peer_listeners=True, tables=4),
}


# ---- helpers ---------------------------------------------------------------

def loadavg():
    return list(os.getloadavg())


def busy_processes():
    found = []
    for name in ("cc1plus", "cc1", "ld", "as", "kds_tests", "cmake", "ninja", "make"):
        if subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0:
            found.append(name)
    return found


def wait_quiet(limit, poll_s=5.0, log=print):
    """Blocks until no build or test process runs and the 1-minute load is
    under `limit`. Returns the load the gate opened at."""
    waited = 0.0
    while True:
        busy = busy_processes()
        load1 = loadavg()[0]
        if not busy and load1 < limit:
            if waited:
                log(f"   quiet after {waited:.0f}s: load {load1:.2f}")
            return load1
        if waited == 0 or int(waited) % 30 == 0:
            log(f"   waiting for a quiet box: load {load1:.2f} busy={busy or 'none'}")
        time.sleep(poll_s)
        waited += poll_s


def stragglers():
    r = subprocess.run(["pgrep", "-x", "kds_server"], capture_output=True, text=True)
    return r.stdout.split()


def percentiles(lats_us):
    """bench_common.nearest_rank over every percentile the tables print."""
    s = sorted(lats_us)
    if not s:
        return {f"p{p}": 0.0 for p in PCTS} | {"mean": 0.0, "n": 0}
    return {"n": len(s), "mean": statistics.fmean(s)} | {
        f"p{p}": nearest_rank(s, p) for p in PCTS}


def parse_session_report(report):
    """The driver's session line: how many connections the hunt took, and
    the retries per phase."""
    attempts = [int(x) for x in re.findall(r"after (\d+) connection\(s\)", report)]
    retries = {}
    if "retries:" in report:
        retries = {k: int(v) for k, v in
                   re.findall(r"(\w[\w-]*)=(\d+)", report.split("retries:")[-1])}
    return {"ddl_connects": attempts[0] if attempts else None,
            "writer_connects": attempts[1] if len(attempts) > 1 else None,
            "retries": retries}


def summarize_config(wall, all_phases, owners, session_report):
    out = {"wall_s": wall, "owner_cores": owners, "session_report": session_report,
           "phases": {}, "per_table": {}, "raw_us": {}}
    total = 0
    for ph in PHASES:
        pooled = [l * 1e6 for t in all_phases for l in all_phases[t][ph].latencies]
        errors = sum(all_phases[t][ph].errors for t in all_phases)
        first_error = next((all_phases[t][ph].first_error for t in all_phases
                            if all_phases[t][ph].first_error), None)
        out["phases"][ph] = percentiles(pooled) | {"errors": errors, "first_error": first_error}
        # Per-connection busy time of the phase: the sum of its latencies
        # (statements are back to back on one connection). The aggregate
        # rate is total statements over the slowest connection's busy time.
        busy = [sum(all_phases[t][ph].latencies) for t in all_phases]
        out["phases"][ph]["busy_max_s"] = max(busy) if busy else 0.0
        out["phases"][ph]["rate_stmt_s"] = (len(pooled) / max(busy)) if busy and max(busy) else 0.0
        out["raw_us"][ph] = [round(x, 1) for x in pooled]
        total += len(pooled)
    out["stmts"] = total
    out["stmt_per_s"] = total / wall if wall else 0.0
    for t, phases in all_phases.items():
        ins = phases["insert"].latencies
        out["per_table"][t] = {
            "owner_core": owners.get(t),
            "first_insert_us": ins[0] * 1e6 if ins else None,
            "second_insert_us": ins[1] * 1e6 if len(ins) > 1 else None,
            "insert_max_us": max(ins) * 1e6 if ins else None,
            "scan_us": phases["scan"].latencies[0] * 1e6 if phases["scan"].latencies else None,
        }
    out |= parse_session_report(session_report)
    return out


def print_config(tag, cfg):
    print(f"   {tag}: wall={cfg['wall_s']:.2f}s stmt/s={cfg['stmt_per_s']:,.0f} "
          f"ddl_connects={cfg['ddl_connects']} writer_connects={cfg['writer_connects']} "
          f"retries={cfg['retries']}")
    for ph in PHASES:
        p = cfg["phases"][ph]
        print(f"      {ph:<13} n={p['n']:>6} mean={p['mean']:>8.1f} p0={p['p0']:>7.1f} "
              f"p25={p['p25']:>7.1f} p50={p['p50']:>7.1f} p75={p['p75']:>7.1f} "
              f"p90={p['p90']:>7.1f} p95={p['p95']:>7.1f} p99={p['p99']:>8.1f} "
              f"p100={p['p100']:>8.1f} err={p['errors']}")
    for t, pt in cfg["per_table"].items():
        print(f"      {t}: owner_core={pt['owner_core']} first_insert={pt['first_insert_us']:.0f}us "
              f"second={pt['second_insert_us']:.0f}us max={pt['insert_max_us']:.0f}us "
              f"scan={pt['scan_us']:.0f}us")


# ---- the ckdbs cell -----------------------------------------------------------

def run_cell(binary, workdir, cell, rows, port, quiet_load, max_connects):
    spec = CELLS[cell]
    os.makedirs(workdir, exist_ok=True)
    result = {"cell": cell, "spec": spec, "rows": rows, "port": port,
              "workdir": workdir, "binary": binary,
              "binary_sha256": hashlib.sha256(open(binary, "rb").read()).hexdigest(),
              "configs": {}}
    for tag, cores, p, listeners in (("single-core", 1, port, False),
                                     ("multi-core", 2, port + 1, spec["peer_listeners"])):
        if stragglers():
            raise RuntimeError(f"a kds_server is already running: {stragglers()}")
        load_at_start = wait_quiet(quiet_load)
        started = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        wall, phases, owners, report = mb.run_config(
            binary, workdir, tag, cores, p, spec["tables"], rows,
            spec["placement"], listeners, max_connects)
        if wall is None:
            raise RuntimeError(f"{cell}/{tag} could not run: {phases}")
        # The driver hands back the session line, the retries and (since the
        # post-run rewrite) the verify line as a list; the parser reads the
        # joined form it was written against.
        cfg = summarize_config(wall, phases, owners,
                               "; ".join(report) if isinstance(report, list) else report)
        cfg |= {"cores": cores, "port": p, "peer_listeners": listeners,
                "placement": spec["placement"], "tables": spec["tables"],
                "started_utc": started, "load_at_start": load_at_start,
                "load_at_end": loadavg()}
        if stragglers():
            raise RuntimeError(f"kds_server outlived {cell}/{tag}: {stragglers()}")
        result["configs"][tag] = cfg
        print(f"\n== {cell} {tag}: cores={cores} tables={spec['tables']} rows={rows} "
              f"placement={spec['placement']} peer_listeners={listeners} "
              f"load_at_start={load_at_start:.2f} ==")
        print_config(tag, cfg)
    s, m = result["configs"]["single-core"], result["configs"]["multi-core"]
    result["ratio_multi_over_single"] = m["stmt_per_s"] / s["stmt_per_s"]
    print(f"   multi/single throughput: {result['ratio_multi_over_single']:.3f}x")
    with open(os.path.join(workdir, "result.json"), "w") as f:
        json.dump(result, f)
    return result


# ---- the PostgreSQL twin ------------------------------------------------------

class PgConn:
    """tools/pg_wire.py's connection behind the driver's `cmd()` shape, so
    `mb.timed` times both engines through one code path."""

    def __init__(self, port, user, database):
        from pg_wire import PgConnection
        self.c = PgConnection("127.0.0.1", port, user, database, timeout=60)

    def cmd(self, line):
        return self.c.send_command(line)

    def scalar(self, sql):
        v = self.c.scalar(sql)
        return v.decode() if isinstance(v, bytes) else v

    def close(self):
        self.c.close()


def run_pg(workdir, tables, rows, port, user, database, quiet_load):
    os.makedirs(workdir, exist_ok=True)
    setup = PgConn(port, user, database)
    version = setup.scalar("SHOW server_version")
    sync = setup.scalar("SHOW synchronous_commit")
    names = [f"bench{i}" for i in range(tables)]
    for n in names:
        setup.cmd(f"DROP TABLE IF EXISTS {n}")
        r = setup.cmd(f"CREATE TABLE {n} (id bigserial PRIMARY KEY, owner varchar, balance bigint)")
        if r.startswith("ERR"):
            raise RuntimeError(f"{n}: {r}")
    load_at_start = wait_quiet(quiet_load)
    started = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
    conns = {n: PgConn(port, user, database) for n in names}
    all_phases = {n: {p: Phase(p) for p in PHASES} for n in names}
    retries = {n: {} for n in names}
    barrier = threading.Barrier(tables)
    # The driver's own worker: the same five statements, PostgreSQL's
    # spelling of the INSERT (a serial pk needs the column list), and no
    # COUNT(*) verify (its reply shape differs). Sharing the function is
    # what makes "the identical statement sequence" a fact, not a comment.
    threads = [threading.Thread(target=mb.worker,
                                args=(conns[n], n, rows, all_phases[n], barrier, retries[n],
                                      None, PG_INSERT))
               for n in names]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.perf_counter() - t0
    for n in names:
        setup.cmd(f"DROP TABLE {n}")
    setup.close()
    report = "every session a PostgreSQL backend; " + mb.retry_line(retries.values())
    cfg = summarize_config(wall, all_phases, {n: None for n in names}, report)
    cfg |= {"engine": f"PostgreSQL {version}", "synchronous_commit": sync,
            "tables": tables, "started_utc": started, "load_at_start": load_at_start,
            "load_at_end": loadavg()}
    print(f"\n== PostgreSQL {version} (synchronous_commit={sync}): {tables} tables x {rows} rows, "
          f"load_at_start={load_at_start:.2f} ==")
    print_config("pg", cfg)
    result = {"cell": f"PG-t{tables}", "rows": rows, "workdir": workdir,
              "configs": {"postgresql": cfg}}
    with open(os.path.join(workdir, "result.json"), "w") as f:
        json.dump(result, f)
    return result


# ---- the wait-breakdown probes -----------------------------------------------

def fdatasync_probe(path, n, size=4096, append=False):
    """Time `n` (write, fdatasync) pairs on `path`'s device: overwrite in
    place (no metadata change) or append (the file grows, so the
    journal's metadata commit is on the path too)."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    buf = b"\x5a" * size
    lats = []
    try:
        for i in range(n):
            t0 = time.perf_counter()
            os.pwrite(fd, buf, i * size if append else 0)
            os.fdatasync(fd)
            lats.append((time.perf_counter() - t0) * 1e6)
    finally:
        os.close(fd)
        os.unlink(path)
    return percentiles(lats)


def ping_probe(binary, workdir, port, n, cores, placement, listeners, want_core):
    """SHOW META round trips on a session the kernel accepted on `want_core`."""
    # The tag names the data file, and `cores` is pinned into its
    # superblock - so it carries the core count, or the second server
    # refuses the first one's file.
    proc = mb.start_server(binary, workdir, f"ping-c{want_core}-cores{cores}", cores, port,
                           placement, listeners)
    try:
        if listeners:
            got, attempts = mb.collect_connections(port, {want_core: 1}, 256)
            conn = got[want_core][0]
        else:
            conn, attempts = mb.Conn(port), 1
        lats = []
        for _ in range(n):
            t0 = time.perf_counter()
            conn.cmd("SHOW META")
            lats.append((time.perf_counter() - t0) * 1e6)
        conn.cmd("STOP")
        conn.close()
        return percentiles(lats) | {"connects": attempts}
    finally:
        proc.wait(timeout=15)


def read_under_writer_probe(binary, workdir, port, cores, placement, listeners, core, n,
                            warm=500):
    """Three floors on one server, on sessions of `core`: the single-session
    INSERT (the `warm` rows, one writer, nothing else running); the point
    SELECT alone; and the point SELECT while a second session on the same
    core commits INSERTs back to back. The last is the read wait behind a
    commit, which the matrix's C cell showed and could not attribute."""
    proc = mb.start_server(binary, workdir, f"ruw-c{core}-cores{cores}", cores, port, placement,
                           listeners)
    try:
        if listeners:
            got, _ = mb.collect_connections(port, {0: 1}, 256)
            setup = got[0][0]
        else:
            setup = mb.Conn(port)
        r = setup.cmd("CREATE TABLE ruw (id int64, owner varchar, balance int64) BTREE")
        if r.startswith("ERR"):
            raise RuntimeError(r)
        if listeners:
            got, attempts = mb.collect_connections(port, {core: 2}, 256)
            reader, writer = got[core]
        else:
            reader, writer, attempts = mb.Conn(port), mb.Conn(port), 2
        setup.close()
        warm_phase, retries = Phase("insert-alone"), {}
        for i in range(warm):
            mb.timed(writer, f"INSERT INTO ruw VALUES ('u{i}', {i})", warm_phase, retries)

        def read(count):
            lats = []
            for i in range(count):
                t0 = time.perf_counter()
                reader.cmd(f"SELECT * FROM ruw WHERE id = {1 + i % warm}")
                lats.append((time.perf_counter() - t0) * 1e6)
            return lats

        alone = read(n)
        stop, wrote, werr, wlats = threading.Event(), [0], [0], []

        def writes():
            i = warm
            while not stop.is_set():
                t0 = time.perf_counter()
                rep = writer.cmd(f"INSERT INTO ruw VALUES ('w{i}', {i})")
                wlats.append((time.perf_counter() - t0) * 1e6)
                i += 1
                wrote[0] += 1
                if rep.startswith("ERR"):
                    werr[0] += 1

        t = threading.Thread(target=writes)
        t.start()
        time.sleep(0.2)
        under = read(n)
        stop.set()
        t.join()
        writer.close()
        reader.cmd("STOP")
        reader.close()
        return {"insert_alone": percentiles([x * 1e6 for x in warm_phase.latencies])
                | {"errors": warm_phase.errors, "retries": retries},
                "select_alone": percentiles(alone),
                "select_under_writer": percentiles(under),
                "writer_during": percentiles(wlats) | {"errors": werr[0]},
                "connects": attempts}
    finally:
        proc.wait(timeout=15)


def run_probes(binary, workdir, port, n, quiet_load):
    os.makedirs(workdir, exist_ok=True)
    out = {}
    wait_quiet(quiet_load)
    out["fdatasync_overwrite_4k"] = fdatasync_probe(os.path.join(workdir, "fsync.bin"), n)
    out["fdatasync_append_4k"] = fdatasync_probe(os.path.join(workdir, "fsync.bin"), n, append=True)
    wait_quiet(quiet_load)
    out["ping_core0_cores1"] = ping_probe(binary, workdir, port, n, 1, "creating", False, 0)
    wait_quiet(quiet_load)
    out["ping_core0_cores2_rotate"] = ping_probe(binary, workdir, port + 1, n, 2, "rotate", True, 0)
    wait_quiet(quiet_load)
    out["ping_core1_cores2_rotate"] = ping_probe(binary, workdir, port + 2, n, 2, "rotate", True, 1)
    for label, cores, placement, listeners, core, p in (
            ("core0_cores1", 1, "creating", False, 0, port + 3),
            ("core1_cores2_rotate", 2, "rotate", True, 1, port + 4)):
        wait_quiet(quiet_load)
        r = read_under_writer_probe(binary, workdir, p, cores, placement, listeners, core, n)
        for k in ("insert_alone", "select_alone", "select_under_writer", "writer_during"):
            out[f"{k}_{label}"] = r[k] | {"connects": r["connects"]}
    for k, v in out.items():
        print(f"   {k:<40} n={v['n']:>5} mean={v['mean']:>8.1f} p0={v['p0']:>7.1f} p25={v['p25']:>7.1f} "
              f"p50={v['p50']:>7.1f} p75={v['p75']:>7.1f} p90={v['p90']:>7.1f} p95={v['p95']:>7.1f} "
              f"p99={v['p99']:>8.1f} p100={v['p100']:>8.1f}"
              + (f" connects={v['connects']}" if "connects" in v else "")
              + (f" errors={v['errors']}" if "errors" in v else "")
              + (f" retries={v['retries']}" if "retries" in v else ""))
    with open(os.path.join(workdir, "probes.json"), "w") as f:
        json.dump(out, f)
    return out


# ---- the report --------------------------------------------------------------

def load_results(workdir):
    results = []
    for path in sorted(glob.glob(os.path.join(workdir, "*", "result.json"))):
        with open(path) as f:
            r = json.load(f)
        r["_dir"] = os.path.basename(os.path.dirname(path))
        results.append(r)
    return results


def fmt_pcts(p):
    return " | ".join(f"{p[k]:,.0f}" for k in ("p0", "p25", "p50", "p75", "p90", "p95", "p99", "p100"))


def report(workdir):
    results = load_results(workdir)
    by_cell = {}
    for r in results:
        by_cell.setdefault(r["cell"], []).append(r)
    for cell, runs in by_cell.items():
        print(f"\n### {cell}  ({len(runs)} run(s))\n")
        print("| run | config | started (UTC) | load at start | wall s | stmt/s | ddl connects | "
              "writer connects | retries | errors |")
        print("|---|---|---|---:|---:|---:|---:|---:|---|---:|")
        for r in runs:
            for tag, cfg in r["configs"].items():
                errors = sum(p["errors"] for p in cfg["phases"].values())
                print(f"| {r['_dir']} | {tag} | {cfg['started_utc']} | {cfg['load_at_start']:.2f} | "
                      f"{cfg['wall_s']:.2f} | {cfg['stmt_per_s']:,.0f} | {cfg.get('ddl_connects')} | "
                      f"{cfg.get('writer_connects')} | "
                      f"{' '.join(f'{k}={v}' for k, v in cfg['retries'].items() if v) or 'none'} | "
                      f"{errors} |")
        if all("ratio_multi_over_single" in r for r in runs):
            ratios = [r["ratio_multi_over_single"] for r in runs]
            print(f"\nmulti/single per run: {', '.join(f'{x:.3f}' for x in ratios)}  "
                  f"(mean {statistics.fmean(ratios):.3f})")
        # Pooled percentiles across runs, per config and phase.
        tags = list(runs[0]["configs"].keys())
        for ph in PHASES:
            print(f"\n**{ph}** - pooled over {len(runs)} run(s), µs\n")
            print("| config | n | rate stmt/s (derived) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |")
            print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
            for tag in tags:
                pooled = [x for r in runs for x in r["configs"][tag]["raw_us"][ph]]
                rate = statistics.fmean(r["configs"][tag]["phases"][ph]["rate_stmt_s"] for r in runs)
                p = percentiles(pooled)
                print(f"| {tag} | {p['n']:,} | {rate:,.0f} | {p['mean']:,.1f} | {fmt_pcts(p)} |")
        # Per-run per-phase p50/p99 so the run-to-run spread is visible.
        print("\n**per run, p50 / p99 µs**\n")
        print("| run | config | " + " | ".join(PHASES[:4]) + " |")
        print("|---|---|" + "---:|" * 4)
        for r in runs:
            for tag in tags:
                cells = [f"{r['configs'][tag]['phases'][ph]['p50']:,.0f} / "
                         f"{r['configs'][tag]['phases'][ph]['p99']:,.0f}" for ph in PHASES[:4]]
                print(f"| {r['_dir']} | {tag} | " + " | ".join(cells) + " |")
        # First INSERT per relation: where the lease refill wait lands.
        print("\n**first INSERT per relation, µs (retries counted per run above)**\n")
        print("| run | config | relation | owner core | first | second | insert max | scan |")
        print("|---|---|---|---:|---:|---:|---:|---:|")
        for r in runs:
            for tag in tags:
                for t, pt in r["configs"][tag]["per_table"].items():
                    print(f"| {r['_dir']} | {tag} | {t} | {pt['owner_core']} | "
                          f"{pt['first_insert_us']:,.0f} | {pt['second_insert_us']:,.0f} | "
                          f"{pt['insert_max_us']:,.0f} | {pt['scan_us']:,.0f} |")
    probes = glob.glob(os.path.join(workdir, "*", "probes.json"))
    for path in probes:
        with open(path) as f:
            pr = json.load(f)
        print(f"\n### probes ({os.path.basename(os.path.dirname(path))}), µs\n")
        print("| probe | n | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for k, v in pr.items():
            print(f"| {k} | {v['n']:,} | {v['mean']:,.1f} | {fmt_pcts(v)} |")


# ---- main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", help="the COPIED kds_server (never build-release's own)")
    ap.add_argument("--workdir", required=True, help="under $HOME - a block device, never tmpfs")
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--port", type=int, default=15470, help="first port; each invocation takes two")
    ap.add_argument("--quiet-load", type=float, default=0.5)
    ap.add_argument("--max-connects", type=int, default=256)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--matrix", action="store_true")
    mode.add_argument("--cell", choices=sorted(CELLS), help="one ckdbs cell, once")
    mode.add_argument("--pg", action="store_true")
    mode.add_argument("--probes", action="store_true")
    mode.add_argument("--report", action="store_true")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--tables", type=int, default=2, help="--pg only")
    ap.add_argument("--pg-port", type=int, default=15433)
    ap.add_argument("--pg-user", default=os.environ.get("PGUSER") or os.environ.get("USER"))
    ap.add_argument("--pg-database", default="bench")
    ap.add_argument("--probe-n", type=int, default=2000)
    ap.add_argument("--tag", default="", help="suffix for the invocation's directory name")
    args = ap.parse_args()

    if args.report:
        report(args.workdir)
        return
    if args.pg:
        run_pg(os.path.join(args.workdir, f"PG-t{args.tables}{args.tag}"), args.tables, args.rows,
               args.pg_port, args.pg_user, args.pg_database, args.quiet_load)
        return
    if not args.binary:
        ap.error("--binary is required for ckdbs cells and probes")
    binary = os.path.abspath(args.binary)
    if args.probes:
        run_probes(binary, os.path.join(args.workdir, f"probes{args.tag}"), args.port,
                   args.probe_n, args.quiet_load)
        return
    if args.cell:
        run_cell(binary, os.path.join(args.workdir, f"{args.cell}{args.tag}"), args.cell,
                 args.rows, args.port, args.quiet_load, args.max_connects)
        return
    port = args.port
    for rep in range(1, args.reps + 1):
        for cell in CELLS:
            run_cell(binary, os.path.join(args.workdir, f"{cell}-r{rep}{args.tag}"), cell,
                     args.rows, port, args.quiet_load, args.max_connects)
            port += 2


if __name__ == "__main__":
    main()
