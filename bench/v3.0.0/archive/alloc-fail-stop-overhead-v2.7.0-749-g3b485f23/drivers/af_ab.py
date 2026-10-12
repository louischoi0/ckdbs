#!/usr/bin/env python3
"""A/B driver for the allocation-failure fail-stop change (scratch; tools/ untouched).

One process per run: two servers side by side (slot A, slot B), identical data, each arm run in
blocks that alternate which server goes first, an equal count of statements per server.

Relations (all BTREE, (id, g, v) / (id, a_id, v)):
  t   --rows rows, id 1..rows      pk point SELECT, INSERT target
  t2  --rows rows, id 1..rows      INSERT control (repeated arm = noise floor)
  a   --sub-rows rows, id 1..N     outer of the correlated EXISTS / join
  b   --sub-rows rows, a_id = id   inner
Arms (in this order):
  select-pk       SELECT * FROM t WHERE id = k
  select-pk-again noise floor: the arm above repeated
  insert          INSERT INTO t VALUES (new id, g, 0)
  insert-again    noise floor: INSERT INTO t2
  exists          SELECT id FROM a WHERE EXISTS (SELECT id FROM b WHERE b.a_id = a.id)   (co_await chain)
  join            SELECT a.id, b.v FROM a JOIN b ON b.a_id = a.id
  ping            SHOW META (control: no storage)
"""
import argparse, gzip, json, sys, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-bad-alloc-reported-as-ok/tools")
from bench_common import Phase  # noqa
from ckdbs_cli import DEFAULT_HOST  # noqa
from fk_overhead_benchmark import Server, host_state, meta, must, timed  # noqa

BATCH = 250


def load(conn, table, n, second="id"):
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
    p.add_argument("--sub-rows", type=int, default=200)
    p.add_argument("--ops", type=int, default=16000)
    p.add_argument("--sub-ops", type=int, default=1600)
    p.add_argument("--blocks", type=int, default=16)
    p.add_argument("--json", required=True)
    a = p.parse_args()
    n, m = a.rows, a.sub_rows
    servers = [Server(a.label, a.host, a.port), Server(a.ab_label, a.host, a.ab_port)]
    labels = [s.label for s in servers]
    before = host_state()
    for s in servers:
        c = s.conn
        for t in ("t", "t2", "a", "b"):
            c.send_command(f"DROP TABLE {t}")
        for t, k in (("t", n), ("t2", n), ("a", m), ("b", m)):
            col = "a_id" if t == "b" else "g"
            must(c, f"CREATE TABLE {t} (id int64, {col} int64, v int64) BTREE", "CREATED")
            load(c, t, k)

    ctr = {"id": 10_000_000}

    def nid():
        ctr["id"] += 1
        return ctr["id"]

    arms = [("select-pk", a.ops, lambda k: f"SELECT * FROM t WHERE id = {1 + (k % n)}"),
            ("select-pk-again", a.ops, lambda k: f"SELECT * FROM t WHERE id = {1 + (k % n)}"),
            ("insert", a.ops, lambda k: f"INSERT INTO t VALUES ({nid()}, {1 + (k % n)}, 0)"),
            ("insert-again", a.ops, lambda k: f"INSERT INTO t2 VALUES ({nid()}, {1 + (k % n)}, 0)"),
            ("exists", a.sub_ops, lambda k: "SELECT id FROM a WHERE EXISTS (SELECT id FROM b WHERE b.a_id = a.id)"),
            ("join", a.sub_ops, lambda k: "SELECT a.id, b.v FROM a JOIN b ON b.a_id = a.id"),
            ("ping", a.ops, lambda k: "SHOW META")]
    results = {l: {} for l in labels}
    engine = {l: {} for l in labels}
    blocks = {l: {} for l in labels}
    raw = {l: {} for l in labels}
    replies = {l: {} for l in labels}
    for name, ops, fn in arms:
        per_block = max(1, ops // a.blocks)
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
                    r = timed(s.conn, fn(block * per_block + k), ph)
                    if block == 0 and k == 0:
                        replies[s.label][name] = r[:80]
                ph.elapsed += time.perf_counter() - t0
                m1 = meta(s.conn)
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
                lat = sorted(ph.latencies[n0:])
                blocks[s.label][name].append({"block": block, "n": len(lat), "p50_us": lat[len(lat) // 2] * 1e6,
                                              "mean_us": sum(lat) / len(lat) * 1e6})
        for s in servers:
            raw[s.label][name] = [round(x * 1e6, 1) for x in results[s.label][name].latencies]

    verify = {}
    for s in servers:
        c = s.conn
        verify[s.label] = {t: c.send_command(f"SELECT COUNT(*) FROM {t}") for t in ("t", "t2", "a", "b")}
        verify[s.label]["exists_rows"] = c.send_command(
            "SELECT COUNT(*) FROM a WHERE EXISTS (SELECT id FROM b WHERE b.a_id = a.id)")
    ok = verify[labels[0]] == verify[labels[1]] and all(
        ph.errors == 0 for l in labels for ph in results[l].values())
    after = host_state()
    payload = {"driver": "af_ab.py", "args": vars(a), "host_before": before, "host_after": after,
               "verify": verify, "verify_ok": ok, "first_replies": replies,
               "arms": {l: {nm: ph.summary() for nm, ph in r.items()} for l, r in results.items()},
               "engine": engine, "blocks": blocks}
    with open(a.json, "w") as f:
        json.dump(payload, f)
    with gzip.open(a.json[:-5] + ".lat.json.gz", "wt") as f:
        json.dump(raw, f)
    for l in labels:
        print(f"== {l} rows={n} sub={m}")
        for nm, ph in results[l].items():
            s = ph.summary()
            print(f"  {nm:<18} ops={s['ops']} p50={s['p50_us']} p99={s['p99_us']} err={s['errors']} first={replies[l][nm]!r}")
    print("verify", verify)
    print("verify_ok", ok)
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
