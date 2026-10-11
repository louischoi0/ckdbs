#!/usr/bin/env python3
"""One client connection of cells 2 and 3 (scratch; tools/ untouched).

Roles
  reader  N pk point SELECTs on t                     (cell 2 reader; cell 3b reader)
  mix     N iterations of: pk SELECT on t, then a relaxed single-row INSERT into m   (cell 3's second connection)
  bystander  pk SELECTs on t back to back until the stop file exists; commits nothing (cell 3c)
  writer  single-row INSERTs into w back to back until the stop file exists
            --mode relaxed : autocommit INSERT under the server's `durability = relaxed`
            --mode strict  : BEGIN (durability = 1, strict) / INSERT / COMMIT frames per unit

The connection is opened repeatedly until SHOW META reports `core=<--want-core>` (SO_REUSEPORT spreads accepts),
so the placement is a fact of the run, recorded in the output. After a warm-up the client waits for --start-at
(an epoch time shared by every client of the run) so the clients' measured spans overlap.
"""
import argparse, gzip, json, os, re, sys, time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/fix-strict-marker-other-session/tools")
from ckdbs_cli import ServerConnection  # noqa


def connect(port, want_core):
    for attempt in range(1, 400):
        c = ServerConnection("127.0.0.1", port, timeout=30.0)
        m = c.send_command("SHOW META").replace("\\n", " ")
        mo = re.search(r"\bcore=(\d+)", m)
        core = int(mo.group(1)) if mo else -1
        if core == want_core:
            return c, attempt
        c.close()
    raise SystemExit(f"no connection landed on core {want_core}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--role", choices=["reader", "mix", "writer", "bystander"], required=True)
    p.add_argument("--mode", default="relaxed")
    p.add_argument("--want-core", type=int, required=True)
    p.add_argument("--n", type=int, default=20000)
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--stop", required=True)
    p.add_argument("--start-at", type=float, required=True)
    p.add_argument("--out", required=True)
    a = p.parse_args()
    c, attempts = connect(a.port, a.want_core)
    n = a.rows
    clock = time.perf_counter
    send = c.send_command
    kc = c._conn
    err = {"n": 0, "first": None}

    def chk(r):
        if r.startswith("ERR") or r.lower().startswith("error"):
            err["n"] += 1
            err["first"] = err["first"] or r[:200]

    # unique id spaces: writer 1.., mix 1.. in its own table m
    ctr = {"w": 0, "m": 0}

    def strict_unit(sql):
        kc.begin(1)
        kc.execute(sql)
        kc.commit()

    def w_sql():
        ctr["w"] += 1
        return f"INSERT INTO w VALUES ({ctr['w']}, 1, 0)"

    def m_sql():
        ctr["m"] += 1
        return f"INSERT INTO m VALUES ({ctr['m']}, 1, 0)"

    # warm-up, off the clock
    for k in range(300 if a.role != "writer" else (60 if a.mode == "strict" else 300)):
        if a.role in ("reader", "mix", "bystander"):
            chk(send(f"SELECT * FROM t WHERE id = {1 + (k % n)}"))
            if a.role == "mix":
                chk(send(m_sql()))
        elif a.mode == "strict":
            strict_unit(w_sql())
        else:
            chk(send(w_sql()))
    meta0 = send("SHOW META").replace("\\n", " ")
    while time.time() < a.start_at:
        time.sleep(0.001)
    t_start = time.time()
    lat = {}
    ends = {}
    if a.role == "reader":
        v = lat.setdefault("select", [])
        for k in range(a.n):
            t0 = clock()
            r = send(f"SELECT * FROM t WHERE id = {1 + (k % n)}")
            v.append((clock() - t0) * 1e6)
            chk(r)
        open(a.stop, "w").close()
    elif a.role == "bystander":
        v = lat.setdefault("select", [])
        k = 0
        while True:
            if k % 20 == 0 and os.path.exists(a.stop):
                break
            t0 = clock()
            r = send(f"SELECT * FROM t WHERE id = {1 + (k % n)}")
            v.append((clock() - t0) * 1e6)
            chk(r)
            k += 1
    elif a.role == "mix":
        vs = lat.setdefault("select", [])
        vi = lat.setdefault("insert", [])
        for k in range(a.n):
            t0 = clock()
            r = send(f"SELECT * FROM t WHERE id = {1 + (k % n)}")
            t1 = clock()
            vs.append((t1 - t0) * 1e6)
            chk(r)
            r = send(m_sql())
            vi.append((clock() - t1) * 1e6)
            chk(r)
        open(a.stop, "w").close()
    else:
        v = lat.setdefault("unit", [])
        e = ends.setdefault("unit", [])
        i = 0
        while True:
            if i % 20 == 0 and os.path.exists(a.stop):
                break
            sql = w_sql()
            t0 = clock()
            if a.mode == "strict":
                strict_unit(sql)
            else:
                chk(send(sql))
            t1 = clock()
            v.append((t1 - t0) * 1e6)
            e.append(time.time())
            i += 1
    t_end = time.time()
    meta1 = send("SHOW META").replace("\\n", " ")
    out = {"role": a.role, "mode": a.mode, "want_core": a.want_core, "connect_attempts": attempts,
           "t_start": t_start, "t_end": t_end, "errors": err["n"], "first_error": err["first"],
           "meta_before": meta0, "meta_after": meta1, "ends": {k: [round(x, 4) for x in v] for k, v in ends.items()},
           "lat": {k: [round(x, 1) for x in v] for k, v in lat.items()}}
    with gzip.open(a.out, "wt") as f:
        json.dump(out, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
