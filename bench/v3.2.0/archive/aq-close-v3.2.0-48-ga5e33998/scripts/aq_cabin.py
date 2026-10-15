#!/usr/bin/env python3
"""AQ close: the Cabin cells bn_census has no shape for.

Reuses tools/bn_census.py whole (servers, interleaved blocks, A / A' / B rotation,
quiet and clean gates, server-CPU accounting) and swaps in its own relations and
shapes. Run it exactly like bn_census.py (same flags); `--only` names shapes.

Shapes, all single statements, each over the literal and the prepared path:
  plain-insert  INSERT into pi(id, sym varchar(16), qty), no Cabin
  cab-insert    INSERT into ci (same schema) carrying CREATE CABIN ON ci(sym); the
                symbols inserted are all observed, so every insert appends to a set
  plain-select  SELECT * FROM ps WHERE sym = ?  (1,000 rows, no Cabin: walked)
  cab-select    SELECT * FROM cs WHERE sym = ?  (same data, Cabin on sym, every
                symbol observed before the first timed statement: served)
With AQ_EXPR=1 (B only: A cannot parse the statement) also:
  x-insert-plain  INSERT into xp(id, ts timestamp, qty), no Cabin
  x-insert        INSERT into xi, CREATE CABIN ON xi (DATE(ts)), 10 of its 50 days
                  observed; the rows inserted land in observed days
  x-walk          SELECT * FROM xw WHERE DATE(ts) = ?  (no Cabin: walked)
  x-select        SELECT * FROM xs WHERE DATE(ts) = ?  (same data, expression Cabin,
                  all 50 days observed: served)
"""
import os, struct, sys

TOOLS = os.environ.get("AQ_TOOLS", "/home/cdkbs/ckdbs/.claude/worktrees/aq-expression-cabins/tools")
sys.path.insert(0, TOOLS)
import bn_census as bn  # noqa: E402
import kwp  # noqa: E402

EXPR = os.environ.get("AQ_EXPR") == "1"
DAY0 = 20727  # 2026-10-01
US_DAY = 86_400_000_000
i64 = kwp.Connection.int64
orig_setup, orig_shapes, orig_end, orig_literal = bn.setup, bn.shapes, bn.end_state, bn.literal_of


def vc(s):
    return (kwp.T_VARCHAR, 0, s.encode())


def ts(day, sec):
    return (kwp.T_TIMESTAMP, 0, struct.pack("<q", (DAY0 + day) * US_DAY + sec * 1_000_000))


def dt(day):
    return (kwp.T_DATE, 0, struct.pack("<i", DAY0 + day))


def literal_of(sql, params):
    out, it = [], iter(params)
    for ch in sql:
        if ch != "?":
            out.append(ch)
            continue
        oid, _, raw = next(it)
        v = int.from_bytes(raw, "little", signed=True)
        if oid == kwp.T_VARCHAR:
            out.append("'" + raw.decode() + "'")
        elif oid == kwp.T_TIMESTAMP:
            out.append("'" + kwp.format_timestamp(v) + "'")
        elif oid == kwp.T_DATE:
            out.append("'" + kwp.format_date(v) + "'")
        else:
            out.append(str(v))
    return "".join(out)


def sym(i, n):
    return "S%03d" % (i % n)


def setup(a, conn):
    orig_setup(a, conn)
    for t in ("pi", "ci"):
        bn.must(conn, f"CREATE TABLE {t} (id int64, sym varchar(16), qty int64) BTREE", "CREATE")
    for t in ("cs", "ps"):
        bn.load(conn, t, a.rows, "id int64, sym varchar(16), qty int64",
                lambda i: f"({i}, '{sym(i, 200)}', {i})")
    conn.execute("CREATE CABIN ON ci(sym)")
    conn.execute("CREATE CABIN ON cs(sym)")
    for k in range(200):  # observe every symbol of cs, twice (the second must be a hit)
        for _ in range(2):
            conn.execute(f"SELECT * FROM cs WHERE sym = '{sym(k, 200)}'")
    for k in range(50):  # observe ci's insert symbols
        for _ in range(2):
            conn.execute(f"SELECT * FROM ci WHERE sym = '{sym(k, 50)}'")
    if EXPR:
        for t in ("xp", "xi", "xs", "xw"):
            bn.must(conn, f"CREATE TABLE {t} (id int64, ts timestamp, qty int64) BTREE", "CREATE")
        for t in ("xs", "xw"):
            for lo in range(1, a.rows + 1, 500):
                rows = ",".join(f"({i}, '{kwp.format_timestamp((DAY0 + i % 50) * US_DAY + 3600 * (i % 20) * 1)}', {i})"
                                for i in range(lo, min(a.rows, lo + 499) + 1))
                bn.must(conn, f"INSERT INTO {t} VALUES {rows}", "INSERT")
        conn.execute("CREATE CABIN ON xi (DATE(ts))")
        conn.execute("CREATE CABIN ON xs (DATE(ts))")
        for d in range(50):
            for _ in range(2):
                conn.execute(f"SELECT * FROM xs WHERE DATE(ts) = '{kwp.format_date(DAY0 + d)}'")
        for d in range(10):
            for _ in range(2):
                conn.execute(f"SELECT * FROM xi WHERE DATE(ts) = '{kwp.format_date(DAY0 + d)}'")


def shapes(a):
    n = a.rows
    out = [
        ("plain-insert", "INSERT INTO pi VALUES (?, ?, ?)",
         lambda q: [i64(30_000_000 + q), vc(sym(q, 50)), i64(q)], "INSERT", "point"),
        ("cab-insert", "INSERT INTO ci VALUES (?, ?, ?)",
         lambda q: [i64(40_000_000 + q), vc(sym(q, 50)), i64(q)], "INSERT", "point"),
        ("plain-select", "SELECT * FROM ps WHERE sym = ?",
         lambda q: [vc(sym(q * 7919, 200))], "SELECT", "point"),
        ("cab-select", "SELECT * FROM cs WHERE sym = ?",
         lambda q: [vc(sym(q * 7919, 200))], "SELECT", "point"),
    ]
    if EXPR:
        out += [
            ("x-insert-plain", "INSERT INTO xp VALUES (?, ?, ?)",
             lambda q: [i64(50_000_000 + q), ts(q % 10, q % 80000), i64(q)], "INSERT", "point"),
            ("x-insert", "INSERT INTO xi VALUES (?, ?, ?)",
             lambda q: [i64(60_000_000 + q), ts(q % 10, q % 80000), i64(q)], "INSERT", "point"),
            ("x-walk", "SELECT * FROM xw WHERE DATE(ts) = ?",
             lambda q: [dt((q * 7919) % 50)], "SELECT", "point"),
            ("x-select", "SELECT * FROM xs WHERE DATE(ts) = ?",
             lambda q: [dt((q * 7919) % 50)], "SELECT", "point"),
        ]
    return out


def end_state(conn, a, last):
    st = orig_end(conn, a, last)
    fields, rows, _, _ = conn.execute("SHOW CABINS")
    st["cabins"] = [{f["name"]: (kwp.render_value(f, c) if c is not None else None)
                     for f, c in zip(fields, r)} for r in rows]
    return st


bn.setup, bn.shapes, bn.end_state, bn.literal_of = setup, shapes, end_state, literal_of
if __name__ == "__main__":
    bn.main()
