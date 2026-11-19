#!/usr/bin/env python3
"""DE close part 2: tools/bn_census.py with one extra relation `x` carrying a
secondary index on w, and the shape `select-index` over it. bn_census itself is
imported unmodified; this wrapper only extends its setup() and shapes()."""
import sys, os
sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor/tools")
import bn_census as B
_setup, _shapes = B.setup, B.shapes
def setup(a, conn):
    B.load(conn, "x", a.rows)
    B.must(conn, "CREATE INDEX x_w ON x (w)", "CREATE")
    r = _setup(a, conn)
    try:
        print("ANALYZE select-index:", conn.execute("ANALYZE SELECT * FROM x WHERE w = 5")[1:3], flush=True)
    except Exception as e:
        print("analyze failed", e, flush=True)
    return r
def shapes(a):
    i64 = B.kwp.Connection.int64
    key = lambda q: 1 + (q * 7919) % a.rows
    out = _shapes(a)
    k = [i for i, s in enumerate(out) if s[0] == "select-pk"][0]
    out.insert(k + 1, ("select-index", "SELECT * FROM x WHERE w = ?", lambda q: [i64(key(q))], "SELECT", "point"))
    return out
B.setup, B.shapes = setup, shapes
B.main()
