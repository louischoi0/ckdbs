#!/usr/bin/env python3
"""A/B driver for the CREATE ASSERTION target-resolution change (scratch; tools/ untouched).

One process per (rows, run): two servers side by side (slot A, slot B), identical data, arms run in
blocks that alternate which server goes first, an equal count of statements per server.

  --cell1 (rows=1000 runs):  as_a (id,g,v) BTREE, rows distinct groups, ONE assertion
                             CREATE ASSERTION cap ON as_a GROUP BY (g) CHECK COUNT(*) <= 2000000000
                             as_n same shape, no assertion.
     insert-assert-existing  INSERT into as_a, g drawn from existing groups   (cell 1: write check guard)
     insert-none-existing    control, INSERT into as_n
     insert-none-again       noise floor: the arm above repeated
     select                  control (cell 3): SELECT * FROM as_n WHERE id = ..
     ping                    control: SHOW META
  every run:  cr (id,g,v) BTREE with `rows` rows, rows distinct groups. --creates times, alternating
     server order: time CREATE ASSERTION capcr ON cr ..., then DROP ASSERTION capcr (timed separately).
     (cell 2)
  non-cell1 runs: select-cr  SELECT * FROM cr WHERE id=..   (cell 3 at this size), ping
"""
import argparse, gzip, json, sys, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-assertion-on-invisible-relation/tools")
from bench_common import Phase  # noqa
from ckdbs_cli import DEFAULT_HOST  # noqa
from fk_overhead_benchmark import Server, host_state, meta, must, timed  # noqa

BATCH = 250


def load(conn, table, n):
    for lo in range(1, n + 1, BATCH):
        rows = ",".join(f"({i},{i},0)" for i in range(lo, min(n, lo + BATCH - 1) + 1))
        must(conn, f"INSERT INTO {table} VALUES {rows}", "INSERTED")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, required=True)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--cell1", action="store_true")
    p.add_argument("--ops", type=int, default=16000)
    p.add_argument("--blocks", type=int, default=16)
    p.add_argument("--creates", type=int, default=20)
    p.add_argument("--json", required=True)
    a = p.parse_args()
    n = a.rows
    servers = [Server(a.label, a.host, a.port), Server(a.ab_label, a.host, a.ab_port)]
    labels = [s.label for s in servers]
    before = host_state()
    setup = {}
    for s in servers:
        c = s.conn
        for t in ("as_a", "as_n", "cr"):
            c.send_command(f"DROP TABLE {t}")
        info = {}
        if a.cell1:
            for t in ("as_a", "as_n"):
                must(c, f"CREATE TABLE {t} (id int64, g int64, v int64) BTREE", "CREATED")
                load(c, t, n)
            r = c.send_command("CREATE ASSERTION cap ON as_a GROUP BY (g) CHECK COUNT(*) <= 2000000000")
            info["cap_reply"] = r[:80]
            info["show_assertions"] = c.send_command("SHOW ASSERTIONS")[:300]
        must(c, "CREATE TABLE cr (id int64, g int64, v int64) BTREE", "CREATED")
        load(c, "cr", n)
        setup[s.label] = info

    per_block = max(1, a.ops // a.blocks)
    ctr = {"id": 10_000_000}

    def nid():
        ctr["id"] += 1
        return ctr["id"]

    def ins(table):
        return lambda k: f"INSERT INTO {table} VALUES ({nid()}, {1 + (k % n)}, 0)"

    arms = []
    if a.cell1:
        arms += [("insert-assert-existing", ins("as_a")), ("insert-none-existing", ins("as_n")),
                 ("insert-none-again", ins("as_n")),
                 ("select", lambda k: f"SELECT * FROM as_n WHERE id = {1 + (k % n)}"),
                 ("ping", lambda k: "SHOW META")]
    else:
        arms += [("select-cr", lambda k: f"SELECT * FROM cr WHERE id = {1 + (k % n)}"),
                 ("ping", lambda k: "SHOW META")]
    results = {l: {} for l in labels}
    engine = {l: {} for l in labels}
    blocks = {l: {} for l in labels}
    raw = {l: {} for l in labels}
    for name, fn in arms:
        for s in servers:
            results[s.label][name] = Phase(name)
            engine[s.label][name] = {"polls": 0.0, "us": 0.0}
            blocks[s.label][name] = []
        for block in range(a.blocks):
            order = servers if block % 2 == 0 else list(reversed(servers))
            for s in order:
                ph = results[s.label][name]
                n0 = len(ph.latencies)
                m0 = meta(s.conn)
                t0 = time.perf_counter()
                for k in range(per_block):
                    timed(s.conn, fn(block * per_block + k), ph)
                ph.elapsed += time.perf_counter() - t0
                m1 = meta(s.conn)
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                lat = sorted(ph.latencies[n0:])
                blocks[s.label][name].append({"block": block, "n": len(lat), "p50_us": lat[len(lat) // 2] * 1e6,
                                              "mean_us": sum(lat) / len(lat) * 1e6})
        for s in servers:
            raw[s.label][name] = [round(x * 1e6, 1) for x in results[s.label][name].latencies]

    # cell 2: CREATE ASSERTION latency, DROP between
    cre = {l: Phase("create") for l in labels}
    drp = {l: Phase("drop") for l in labels}
    crmeta = {l: {"polls": 0.0, "us": 0.0} for l in labels}
    creply = {l: [] for l in labels}
    for k in range(a.creates):
        order = servers if k % 2 == 0 else list(reversed(servers))
        for s in order:
            m0 = meta(s.conn)
            r = timed(s.conn, "CREATE ASSERTION capcr ON cr GROUP BY (g) CHECK COUNT(*) <= 2000000000", cre[s.label])
            m1 = meta(s.conn)
            crmeta[s.label]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
            crmeta[s.label]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
            creply[s.label].append(r[:60])
            timed(s.conn, "DROP ASSERTION capcr", drp[s.label])
    # verify: equal state on both servers
    verify = {}
    for s in servers:
        c = s.conn
        v = {"cr_count": c.send_command("SELECT COUNT(*) FROM cr"),
             "assertions_after": c.send_command("SHOW ASSERTIONS")[:300]}
        if a.cell1:
            v["as_a_count"] = c.send_command("SELECT COUNT(*) FROM as_a")
            v["as_n_count"] = c.send_command("SELECT COUNT(*) FROM as_n")
        verify[s.label] = v
    ok = verify[labels[0]] == verify[labels[1]] and all(
        not x.startswith("ERR") for l in labels for x in creply[l])
    ok = ok and all(ph.errors == 0 for l in labels for ph in list(results[l].values()) + [cre[l], drp[l]])
    after = host_state()
    payload = {"driver": "at_ab.py", "args": vars(a), "setup": setup, "host_before": before, "host_after": after,
               "verify": verify, "verify_ok": ok,
               "arms": {l: {nm: ph.summary() for nm, ph in r.items()} for l, r in results.items()},
               "engine": engine, "blocks": blocks,
               "create": {l: cre[l].summary() for l in labels}, "drop": {l: drp[l].summary() for l in labels},
               "create_engine": crmeta, "create_replies": creply,
               "create_lat_us": {l: [round(x * 1e6, 1) for x in cre[l].latencies] for l in labels},
               "drop_lat_us": {l: [round(x * 1e6, 1) for x in drp[l].latencies] for l in labels}}
    with open(a.json, "w") as f:
        json.dump(payload, f)
    with gzip.open(a.json[:-5] + ".lat.json.gz", "wt") as f:
        json.dump(raw, f)
    for l in labels:
        print(f"== {l} rows={n}  create p50={cre[l].summary()['p50_us']} drop p50={drp[l].summary()['p50_us']}")
        for nm, ph in results[l].items():
            s = ph.summary()
            print(f"  {nm:<24} ops={s['ops']} p50={s['p50_us']} p99={s['p99_us']} err={s['errors']}")
    print("verify_ok", ok)
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
