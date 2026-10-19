#!/usr/bin/env python3
"""CD close, cells 1-3: tools/bn_census.py with three extra INSERT shapes.

A sibling of the unmodified driver (the driver is imported and its `setup`,
`shapes` and `clean_cell` are wrapped; nothing under tools/ is edited).
Extra relations, all (id int64, v int64, w int64) BTREE:

  ib   no SPARE, never altered               -> shape insert-plain
  ip   SPARE 2, no ALTER                     -> shape insert-spare
  idr  SPARE 2, ADD x1 int64, ADD x2 varchar(16), DROP x1, DROP x2, no clean
                                             -> shape insert-dropped

Each shape is `INSERT INTO <rel> (id, v, w) VALUES (?, ?, ?)`: a column list,
so the dropped slots (which a positional INSERT would have to count) are not
in the way. The key carries the cell attempt number, so a cell run again
inserts no duplicate. Every other flag is bn_census's.
"""
import os, sys
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/tools")
import bn_census as bc  # noqa: E402
import kwp  # noqa: E402

attempt = [0]
orig_setup, orig_shapes, orig_clean = bc.setup, bc.shapes, bc.clean_cell


def setup(a, conn):
    orig_setup(a, conn)
    bc.must(conn, "CREATE TABLE ib (id int64, v int64, w int64) BTREE", "CREATE")
    bc.must(conn, "CREATE TABLE ip (id int64, v int64, w int64) BTREE SPARE 2", "CREATE")
    bc.must(conn, "CREATE TABLE idr (id int64, v int64, w int64) BTREE SPARE 2", "CREATE")
    bc.must(conn, "ALTER TABLE idr ADD COLUMN x1 int64 NULL", "ADDED")
    bc.must(conn, "ALTER TABLE idr ADD COLUMN x2 varchar(16) NULL", "ADDED")
    bc.must(conn, "ALTER TABLE idr DROP COLUMN x1", "DROPPED")
    bc.must(conn, "ALTER TABLE idr DROP COLUMN x2", "DROPPED")


def shapes(a):
    i64 = kwp.Connection.int64
    key = lambda q: 30_000_000 + attempt[0] * 2_000_000 + q
    out = list(orig_shapes(a))
    for name, rel in (("insert-plain", "ib"), ("insert-spare", "ip"), ("insert-dropped", "idr")):
        out.append((name, f"INSERT INTO {rel} (id, v, w) VALUES (?, ?, ?)",
                    lambda q: [i64(key(q))] * 3, "INSERT", "point"))
    return out


def clean_cell(a, fn, retriable=True, pids=()):
    def counted():
        attempt[0] += 1
        return fn()
    return orig_clean(a, counted, retriable, pids)


bc.setup, bc.shapes, bc.clean_cell = setup, shapes, clean_cell
if __name__ == "__main__":
    bc.main()
