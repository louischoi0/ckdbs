#!/usr/bin/env python3
"""Cell 1 driver (scratch; tools/ untouched): the per-statement cost of the statement-boundary
ceiling check, cores = 1, relaxed, one connection per server.

One process per (rows, run): two servers side by side (slot A, slot B), identical data, every arm run
in blocks that alternate which server goes first, an equal count of statements per server.

Relation t (id int64, g int64, v int64) BTREE with --rows rows, ids 1..rows.
Arms (in this order):
  select        SELECT * FROM t WHERE id = k              pk point read
  insert        INSERT INTO t VALUES (id, g, 0)           single-row, named ascending id above the loaded ids
  insert-again  the arm above repeated                    noise floor from inside the run
  update        UPDATE t SET v = k WHERE id = k           one-row update by pk
  ping          SHOW NAMESPACES                           client/socket floor (still passes the boundary)
"""
import argparse, gzip, json, sys, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-strict-marker-other-session/tools")
from ckdbs_cli import DEFAULT_HOST  # noqa
from fk_overhead_benchmark import Server, host_state, meta, must  # noqa

BATCH = 250


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--label", default="B")
    p.add_argument("--ab-port", type=int, required=True)
    p.add_argument("--ab-label", default="A")
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--ops", type=int, default=16000)
    p.add_argument("--blocks", type=int, default=16)
    p.add_argument("--json", required=True)
    a = p.parse_args()
    n = a.rows
    servers = [Server(a.label, a.host, a.port), Server(a.ab_label, a.host, a.ab_port)]
    labels = [s.label for s in servers]
    before = host_state()
    info = {}
    for s in servers:
        c = s.conn
        c.send_command("DROP TABLE t")
        must(c, "CREATE TABLE t (id int64, g int64, v int64) BTREE", "CREATED")
        for lo in range(1, n + 1, BATCH):
            rows = ",".join(f"({i},{i},0)" for i in range(lo, min(n, lo + BATCH - 1) + 1))
            must(c, f"INSERT INTO t VALUES {rows}", "INSERTED")
        info[s.label] = c.send_command("SHOW META").replace("\\n", " ")[:2000]
    per_block = max(1, a.ops // a.blocks)
    ctr = {"id": 10_000_000}

    def nid():
        ctr["id"] += 1
        return ctr["id"]

    arms = [("select", lambda k: f"SELECT * FROM t WHERE id = {1 + (k % n)}"),
            ("insert", lambda k: f"INSERT INTO t VALUES ({nid()}, {1 + (k % n)}, 0)"),
            ("insert-again", lambda k: f"INSERT INTO t VALUES ({nid()}, {1 + (k % n)}, 0)"),
            ("update", lambda k: f"UPDATE t SET v = {k + 1} WHERE id = {1 + (k % n)}"),
            ("ping", lambda k: "SHOW NAMESPACES")]
    raw = {l: {} for l in labels}
    engine = {l: {} for l in labels}
    errors = {l: 0 for l in labels}
    clock = time.perf_counter
    for name, fn in arms:
        for s in servers:
            raw[s.label][name] = []
            engine[s.label][name] = {"polls": 0.0, "us": 0.0}
        for block in range(a.blocks):
            order = servers if block % 2 == 0 else list(reversed(servers))
            for s in order:
                m0 = meta(s.conn)
                lat = raw[s.label][name]
                send = s.conn.send_command
                for k in range(per_block):
                    sql = fn(block * per_block + k)
                    t0 = clock()
                    r = send(sql)
                    lat.append((clock() - t0) * 1e6)
                    if r.startswith("ERR"):
                        errors[s.label] += 1
                m1 = meta(s.conn)
                engine[s.label][name]["polls"] += m1["sched_foreground_polls"] - m0["sched_foreground_polls"]
                engine[s.label][name]["us"] += m1["sched_foreground_polled_us"] - m0["sched_foreground_polled_us"]
    verify = {}
    for s in servers:
        verify[s.label] = {"count": s.conn.send_command("SELECT COUNT(*) FROM t"),
                           "meta_end": s.conn.send_command("SHOW META").replace("\\n", " ")[:2000]}
    ok = verify[labels[0]]["count"] == verify[labels[1]]["count"] and all(e == 0 for e in errors.values())
    payload = {"driver": "bw_ab.py", "args": vars(a), "host_before": before, "host_after": host_state(),
               "meta_start": info, "verify": verify, "verify_ok": ok, "errors": errors, "engine": engine}
    with open(a.json, "w") as f:
        json.dump(payload, f)
    with gzip.open(a.json[:-5] + ".lat.json.gz", "wt") as f:
        json.dump({l: {nm: [round(x, 1) for x in v] for nm, v in r.items()} for l, r in raw.items()}, f)
    for l in labels:
        print("==", l, "rows", n, verify[l]["count"])
        for nm, v in raw[l].items():
            sv = sorted(v)
            print(f"  {nm:<14} ops={len(sv)} p50={sv[len(sv)//2]:.1f} p99={sv[int(len(sv)*.99)]:.1f}")
    print("verify_ok", ok)
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())
