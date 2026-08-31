#!/usr/bin/env python3
"""XD2/XD3/XD4/XD5 — one scenario-2 booking cell, bracketed exactly around
the booking phase by per-core `SHOW META` reads, so a cell reports not only
TPS and commit-latency percentiles (what `run_s2_cell.sh` already gives) but
also **which cores took the device syncs a fixed count of bookings cost**.

`instructions/v2.7.1/measurement-xd.md`. This is orchestration only: every
piece of business logic - schema, load, partitioning, one booking attempt,
merging bookers' results - is imported unchanged from `tools/scenario2_freight.py`
and called exactly as its own `main()` calls it, so the workload measured
here is byte-for-byte the one `bench/v2.7.0/results-scenario2-cores-*.md`
measured. What this file adds is two things `main()` cannot give an outside
reader: (1) a `SHOW META` snapshot per core taken the instant before the
booker processes start and the instant after they all join, so the sync
counters XD0 exposed can be read as a delta over *only* the booking phase,
never the DDL+load phase before it; (2) the same bracket for core 0's
`sched_*` and `shipped_*` occupancy fields, for XD4.

The manifest reporter is deliberately left out (`main()`'s default is on):
it never writes, so it cannot move `wal_syncs`, but it is one more
concurrent read consumer this order is not asking about, and cutting it
keeps every cell's engine work identical to every other cell's rather than
also depending on how many reporter passes the wall clock happened to fit.
`--verify` defaults to 0 for the same reason (`results-scenario2-cores`
arms A/B/D/E/F's own rule) - pass `--verify N` to run it as a sanity check
on the wiring, not as a matrix default.

Usage:
    bench/wal_sync_decomposition_probe.py --server /path/to/kds_server-<sha> \\
        --workdir ~/bench-xd/cells --label xd2-pl-c8-b8 --port 15701 \\
        --cores 8 --peer-listeners on --durability group --bookers 8 \\
        --bookings 5000 --json ~/bench-xd/out/xd2-pl-c8-b8.json
"""

import argparse
import json
import multiprocessing
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)

import scenario2_freight as s2  # noqa: E402
from bench_common import Phase  # noqa: E402
from multicore_benchmark import (  # noqa: E402
    Conn, check_host, collect_connections, field, wait_for_port,
)


def meta_fields(meta, prefixes):
    out = {}
    for tok in meta.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        if any(k.startswith(p) for p in prefixes):
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


TRACKED_PREFIXES = ("wal_", "sched_", "shipped_")


def delta(before, after):
    keys = set(before) | set(after)
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys}


def write_conf(cell_dir, port, cores, placement, peer_listeners, durability,
               wal_dir=None):
    lines = [
        f"data_file = {os.path.join(cell_dir, 's.db')}",
        f"cores = {cores}",
        f"placement = {placement}",
        f"peer_listeners = {peer_listeners}",
        f"port = {port}",
        f"durability = {durability}",
        f"log_dir = {cell_dir}",
        "log_file = server.log",
        "log_level = info",
    ]
    if wal_dir:
        lines.append(f"wal_dir = {wal_dir}")
    conf = os.path.join(cell_dir, "kds.conf")
    with open(conf, "w") as f:
        f.write("\n".join(lines) + "\n")
    return conf


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", required=True, help="a COPY of kds_server, never build-release's own")
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cores", type=int, default=8)
    ap.add_argument("--placement", default="creating")
    ap.add_argument("--peer-listeners", choices=("on", "off"), default="on")
    ap.add_argument("--durability", choices=("group", "strict", "relaxed"), default="group")
    ap.add_argument("--wal-dir", default=None,
                    help="XD3's ablation: a tmpfs directory for the WAL segments; "
                         "the data file stays under --workdir")
    ap.add_argument("--organizations", type=int, default=2000)
    ap.add_argument("--ships", type=int, default=200)
    ap.add_argument("--operations", type=int, default=2000)
    ap.add_argument("--cargos", type=int, default=20000)
    ap.add_argument("--capacity-headroom", type=float, default=1.0)
    ap.add_argument("--credit-headroom", type=float, default=1.0)
    ap.add_argument("--hot-routes", type=int, default=6)
    ap.add_argument("--bookings", type=int, default=5000)
    ap.add_argument("--bookers", type=int, default=1)
    ap.add_argument("--seconds-ceiling", type=float, default=600.0)
    ap.add_argument("--contend", action="store_true", default=True)
    ap.add_argument("--no-contend", dest="contend", action="store_false")
    ap.add_argument("--capacity-mode", choices=("cached", "scan"), default="cached")
    ap.add_argument("--txn", action="store_true", default=True)
    ap.add_argument("--no-txn", dest="txn", action="store_false")
    ap.add_argument("--max-retries", type=int, default=5)
    ap.add_argument("--max-fees", type=int, default=0)
    ap.add_argument("--isolation", choices=("read-committed", "repeatable-read"), default=None)
    ap.add_argument("--verify", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--max-connects", type=int, default=512)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--json", default="")
    ap.add_argument("--require-shipped", action="store_true",
                    help="under peer_listeners=on with cores>1, a booker's own "
                         "connection lands on a core the kernel's SO_REUSEPORT "
                         "picked - at bookers=1 that is core 0 itself with "
                         "probability 1/cores, which would silently measure the "
                         "one-owner path under a cross-owner label. When set, exit "
                         "42 (rather than reporting a cell) if core 0's "
                         "shipped_executed delta is 0, so a caller can retry on a "
                         "fresh cell instead of publishing a mislabeled number")
    ap.add_argument("--require-shipped-rate", type=float, default=0.0,
                    help="the same guard, generalised to bookers>1: a fraction "
                         "(e.g. 0.97) of `committed * 6.0` (the empirical shipped-"
                         "statements-per-booking rate a fully cross-owner b=1 run "
                         "measures) that core 0's shipped_executed delta must reach. "
                         "Below it, some booker connections landed on core 0 itself "
                         "and diluted the cell with local commits - exit 42 to retry "
                         "rather than publish a mixed cell under a cross-owner label")
    args = ap.parse_args()

    check_host(args.workdir, args.force)
    cell_dir = os.path.join(args.workdir, args.label)
    os.makedirs(cell_dir, exist_ok=True)
    for stale in ("s.db", "s.db.wal"):
        p = os.path.join(cell_dir, stale)
        if os.path.exists(p):
            if os.path.isdir(p):
                import shutil
                shutil.rmtree(p)
            else:
                os.remove(p)
    wal_dir = None
    if args.wal_dir:
        wal_dir = os.path.join(args.wal_dir, args.label)
        import shutil
        if os.path.isdir(wal_dir):
            shutil.rmtree(wal_dir)
        os.makedirs(wal_dir, exist_ok=True)

    peer_listeners = args.peer_listeners if args.cores > 1 else "off"
    conf = write_conf(cell_dir, args.port, args.cores, args.placement,
                      peer_listeners, args.durability, wal_dir)

    err_path = os.path.join(cell_dir, "s.stderr")
    with open(err_path, "w") as err:
        proc = subprocess.Popen([os.path.abspath(args.server), "--config", conf],
                                stdout=err, stderr=subprocess.STDOUT)

    out = {"label": args.label, "cores": args.cores, "peer_listeners": peer_listeners,
           "durability": args.durability, "bookings": args.bookings, "bookers": args.bookers,
           "wal_dir": wal_dir or "(default, beside data file)"}
    try:
        wait_for_port(args.port, err_path)

        # Relation names are SQL identifiers - no '-' - while a cell's label
        # is free to have one for readability in the matrix; derive one from
        # the other rather than asking for both on every invocation.
        suffix = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in args.label)
        rng = random.Random(args.seed)
        client = s2.Client(args.host, args.port, args.timeout)
        ddl = Phase("ddl", "8 relations")
        s2.create_tables(client, suffix, ddl, cabin=False, fk=False)
        tables = {base: f"{base}_{suffix}" for base in s2.CREATE_ORDER}

        class DemandArgs:
            pass
        dargs = DemandArgs()
        dargs.cargos, dargs.operations, dargs.organizations = (
            args.cargos, args.operations, args.organizations)
        dargs.capacity_headroom, dargs.credit_headroom = (
            args.capacity_headroom, args.credit_headroom)
        demand = s2.demand_of(dargs)

        load_t0 = time.perf_counter()
        orgs = s2.load_organizations(client, tables["organizations"], args.organizations,
                                     demand, rng, Phase("load-organizations"))
        ships = s2.load_ships(client, tables["ships"], args.ships, demand, rng,
                              Phase("load-ships"))
        operations = s2.load_operations(client, tables["operations"], args.operations,
                                        ships, rng, Phase("load-operations"))
        fees = s2.load_fees(client, tables["fees"], Phase("load-fees"))
        hot_routes = [s2.route_code(rng.randrange(s2.PORTS), rng.randrange(s2.PORTS))
                      for _ in range(args.hot_routes)]
        recipes = s2.load_recipes(client, tables["recipes"], fees, hot_routes, rng,
                                  Phase("load-recipes"))
        cargos = s2.load_cargos(client, tables["cargos"], args.cargos, orgs, rng,
                                Phase("load-cargos"))
        load_elapsed = time.perf_counter() - load_t0
        out["load_elapsed_s"] = round(load_elapsed, 3)
        out["loaded"] = {"organizations": len(orgs), "ships": len(ships),
                         "operations": len(operations), "fees": len(fees),
                         "recipes": len(recipes), "cargos": len(cargos)}

        if args.isolation:
            level = args.isolation.replace("-", " ").upper()
            client(f"SET ISOLATION LEVEL {level}")

        slices = s2.partition(operations, orgs, cargos, args.bookers, args.contend)

        # ---- the per-core SHOW META reader pool ---------------------------
        if peer_listeners == "on" and args.cores > 1:
            needed = {c: 1 for c in range(args.cores)}
        else:
            needed = {0: 1}
        readers, connect_attempts = collect_connections(args.port, needed, args.max_connects)
        out["reader_connect_attempts"] = connect_attempts
        reader_of = {c: cs[0] for c, cs in readers.items()}

        # ---- the exact bracket ---------------------------------------------
        target = -(-args.bookings // args.bookers)
        deadline_wall = time.time() + args.seconds_ceiling
        fee_book = s2.fees_by_id(fees)
        result_q = multiprocessing.Queue()

        booker_args = argparse.Namespace(
            host=args.host, port=args.port, timeout=args.timeout,
            isolation=args.isolation, echo=False, seed=args.seed,
            txn=args.txn, capacity_mode=args.capacity_mode,
            max_fees=args.max_fees, max_retries=args.max_retries,
            # `run_bookings` uses this only to pace the cosmetic `day` value
            # charges are dated with; 60s is `main()`'s own default and this
            # cell is bookings-bounded, not seconds-bounded, so the pacing
            # never gates completion.
            seconds=60.0,
        )
        workers = [multiprocessing.Process(
            target=s2.booker_process,
            args=(i, booker_args, suffix, slices[i], fee_book, deadline_wall,
                  target, result_q))
            for i in range(args.bookers)]

        before = {c: meta_fields(rd.cmd("SHOW META"), TRACKED_PREFIXES)
                 for c, rd in reader_of.items()}
        run_started = time.perf_counter()
        for w in workers:
            w.start()
        results = [result_q.get() for _ in workers]
        for w in workers:
            w.join()
        run_elapsed = time.perf_counter() - run_started
        after = {c: meta_fields(rd.cmd("SHOW META"), TRACKED_PREFIXES)
                 for c, rd in reader_of.items()}

        fatal = [r for r in results if "fatal" in r]
        if fatal:
            raise RuntimeError(f"booker {fatal[0]['index']} died: {fatal[0]['fatal']}")

        merged = s2.merge_bookers(results, run_elapsed)
        out["outcomes"] = merged["counts"]
        out["axes"] = merged["axes"]
        out["retries"] = merged["retries"]
        out["abandoned"] = merged["abandoned"]
        out["run_elapsed_s"] = round(run_elapsed, 4)
        committed = merged["counts"][s2.COMMITTED]
        out["tps"] = committed / run_elapsed if run_elapsed > 0 else 0.0

        out["phases"] = {name: p.summary() for name, p in merged["phases"].items()}

        per_core = {}
        total_wal_syncs_delta = 0
        for c in sorted(before):
            d = delta(before[c], after[c])
            per_core[str(c)] = d
            total_wal_syncs_delta += d.get("wal_syncs", 0)
        out["per_core_delta"] = per_core
        out["total_wal_syncs_delta"] = total_wal_syncs_delta
        out["syncs_per_booking"] = (total_wal_syncs_delta / committed) if committed else None

        core0_shipped = per_core.get("0", {}).get("shipped_executed", 0)
        landed_local = (args.cores > 1 and peer_listeners == "on" and core0_shipped == 0)
        out["landed_local"] = landed_local
        if args.require_shipped_rate and committed:
            expected = committed * 6.0
            out["shipped_rate_ratio"] = core0_shipped / expected if expected else None
            out["some_landed_local"] = (core0_shipped / expected) < args.require_shipped_rate
        else:
            out["some_landed_local"] = False

        if args.verify:
            v = s2.verify(client, tables, merged["state"], args.verify, rng,
                          trust_belief=not args.contend)
            out["verify"] = {"checks": v.checks, "failures": v.failures, "first": v.first,
                             "unanswered": v.unanswered, "first_unanswered": v.first_unanswered}

        for rd in reader_of.values():
            rd.close()
        client("STOP")
        client.close()
    finally:
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))
    if out.get("outcomes", {}).get(s2.FAILED, 0):
        print(f"WARNING: {out['outcomes'][s2.FAILED]} FAILED outcomes", file=sys.stderr)
    if args.require_shipped and out.get("landed_local"):
        print(f"landed_local: the booker's own connection was accepted on core 0 "
              f"itself (probability 1/{args.cores} per attempt) - this cell measured "
              f"the one-owner path, not the cross-owner one its label asks for. "
              f"Exiting 42 for the caller to retry on a fresh cell.", file=sys.stderr)
        return 42
    if out.get("some_landed_local"):
        print(f"some_landed_local: core 0's shipped_executed delta is only "
              f"{out.get('shipped_rate_ratio', 0):.3f} of the fully-cross-owner "
              f"expectation - at least one of {args.bookers} bookers' connections "
              f"landed on core 0 itself. Exiting 42 for the caller to retry on a "
              f"fresh cell.", file=sys.stderr)
        return 42
    return 0


if __name__ == "__main__":
    sys.exit(main())
