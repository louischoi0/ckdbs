#!/usr/bin/env python3
"""Writes BS-S5's seed corpora under fuzz/corpus/<target>/ (BS-R3: committed).

    scripts/bs/fuzz_seeds.py

fuzz_sql:    every statement of tests/testdata/parser_corpus.txt, and the
             forms the grammar gained after V07 that the corpus does not
             carry (SET expressions, PURGE, the scalar functions, the query
             layer's forms, DDL since V07).
fuzz_kwp:    each golden session's client bytes (tests/testdata/kwp_golden.txt),
             under both of the target's paths (no gate, SCRAM) and two chunk
             sizes.
fuzz_volume: no edit, and one edit in each file.

A file is named by its content's sha1, so a re-run writes nothing new.
"""
import hashlib
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

AFTER_V07 = [
    "UPDATE t SET v = v * 2 + 1, s = COALESCE(s, 'x') WHERE id = 1",
    "UPDATE t SET d = ROUND(d / 3, 2) WHERE v BETWEEN 10 AND 20",
    "UPDATE t SET s = CASE WHEN v > 15 THEN UPPER(s) ELSE NULL END",
    "UPDATE t SET v = (SELECT w FROM c WHERE id = 2) WHERE id = 3",
    "PURGE FROM t WHERE id BETWEEN 1 AND 3",
    "SELECT id FROM t WHERE DATE(ts) = '2026-10-01'",
    "SELECT id FROM t WHERE ts < NOW()",
    "SELECT DISTINCT v % 3 AS m FROM t ORDER BY m DESC",
    "SELECT t.id, c.w FROM t LEFT JOIN c ON t.id = c.pid WHERE c.w IS NULL",
    "SELECT v, COUNT(*) FROM t GROUP BY v HAVING COUNT(*) > 1 ORDER BY 2 LIMIT 5",
    "SELECT id FROM t WHERE v IN (10, 30) OR NOT (s LIKE 'a%')",
    "SELECT id FROM t UNION SELECT pid FROM c",
    "SELECT x.n FROM (SELECT COUNT(*) AS n FROM c) AS x",
    "INSERT INTO t (id, v) SELECT id + 10, w FROM c",
    "INSERT INTO t VALUES (9, 90, 'z', NULL, NULL) ON CONFLICT DO NOTHING",
    "UPDATE t SET v = c.w FROM c WHERE c.pid = t.id",
    "DELETE FROM c USING t WHERE c.pid = t.id AND t.v > 20",
    "INSERT INTO t VALUES (8, 80, NULL, NULL, NULL) RETURNING *",
    "CREATE UNIQUE INDEX tu ON t (s)",
    "REINDEX INDEX tv",
    "DROP INDEX tv",
    "ALTER TABLE t ADD COLUMN x int64 NULL",
    "ALTER TABLE t DROP COLUMN s",
    "PURGE DROPPED COLUMNS FROM t",
    "ALTER TABLE t RENAME COLUMN v TO vv",
    "CREATE CABIN ON t(DATE(ts))",
    "CREATE ASSERTION one ON c GROUP BY (pid) CHECK COUNT(*) <= 2",
    "CREATE TRANSITION st ON t(s) ON INSERT ('a', 'b') ON UPDATE ('a' => 'b')",
    "CREATE NAMESPACE n2",
    "CREATE TABLE n2.u (id int64, w int64) HEAP",
    "SELECT id FROM t WHERE v NOT BETWEEN 15 AND 25",
    "SELECT id FROM t INTERSECT SELECT pid FROM c EXCEPT SELECT 2",
    "SELECT t.id, c.w FROM t CROSS JOIN c",
    "SELECT * FROM t JOIN c USING (id)",
    "INSERT INTO t VALUES (1, 10, 'a', NULL, NULL) ON CONFLICT (id) DO NOTHING",
    "CREATE TABLE u (id int64, k varchar CABIN AUTO, w int64 NO CABIN) BTREE SPARE 2",
    "DROP CABIN ON t (DATE(ts))",
    "DROP TRANSITION st",
    "DROP NAMESPACE n2",
    "DROP TABLE c",
    "SELECT * FROM sys.tables",
    "SHOW META",
    "BEGIN",
    "SET DURABILITY strict",
    "ANALYZE SELECT id FROM t WHERE v = 10",
]


def write(target, data):
    d = os.path.join(ROOT, "fuzz", "corpus", target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, hashlib.sha1(data).hexdigest()), "wb") as f:
        f.write(data)


def main():
    with open(os.path.join(ROOT, "tests", "testdata", "parser_corpus.txt")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split(None, 4)
            if len(parts) == 5:
                write("fuzz_sql", parts[4].replace("\\n", "\n").encode())
    for sql in AFTER_V07:
        write("fuzz_sql", sql.encode())

    with open(os.path.join(ROOT, "tests", "testdata", "kwp_golden.txt")) as f:
        for line in f:
            if line.startswith("client "):
                blob = bytes.fromhex(line.split()[1])
                for mode in (0, 1):
                    for chunk in (63, 0):
                        write("fuzz_kwp", bytes([mode, chunk]) + blob)

    write("fuzz_volume", b"")
    write("fuzz_volume", bytes([0, 0x10, 0x20, 0, 0, 0xFF]))  # a data page byte
    write("fuzz_volume", bytes([1, 0x40, 0, 0, 0, 0x00]))     # a log byte


if __name__ == "__main__":
    main()
