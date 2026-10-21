#!/usr/bin/env python3
"""BS-S6's differential query test (instructions/v3.0.0/workorder-bs-test-hardening.md;
BS-Q3 (a): the PostgreSQL 18 twin is the reference).

    scripts/bs/difftest.py KDS_SERVER [--seeds 1,2,...] [--queries N] [--pg-port 15434]

Builds one schema and one data set from the seed, in both engines - a
throwaway kds_server over a temporary directory, and a database `bs_diff`
on the twin, dropped and made again - then runs seeded queries and writes
against both and compares the answers: as sorted multisets, unless the
query's order is total, then in order. Everything generated stays inside
the subset `bs-s0-census.md` §4 found safe for both, so a mismatch is
triaged before it is believed (BS-R4): a KDS defect, a documented semantic
difference - recorded once, as a comment at the generator form that avoids
it, with the spec line that states it - or a generator defect. Exits 1 on any untriaged mismatch; each one is printed
with the statement that shows it (BS-R3: the seed reproduces the run).

Never run by the push hook (§6.1); scripts/bs/nightly.sh runs it.
"""
import argparse
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PSQL = os.environ.get("BS_PSQL", "/home/cdkbs/.local/bin/psql")
TABLES = ["t1", "t2", "t3"]

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Kds:
    """A kds_server on a temporary directory, spoken to over the debug text port."""

    def __init__(self, binary):
        self.dir = tempfile.mkdtemp(prefix="kds_difftest_")
        self.port = free_port()
        conf = os.path.join(self.dir, "kds.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/kds.db\nbuffer_pool_frames = 8192\ncores = 1\n"
                    f"port = {free_port()}\ndebug_text_port = {self.port}\ndurability = relaxed\n"
                    f"log_dir = {self.dir}\nlog_level = warn\n")
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=subprocess.DEVNULL,
                                     stderr=open(os.path.join(self.dir, "stderr"), "w"))
        deadline = time.time() + 30
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=60)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    # Never leave a server behind; the directory stays for its stderr.
                    self.proc.kill()
                    self.proc.wait()
                    raise RuntimeError("kds_server did not come up; see " + self.dir)
                time.sleep(0.1)
        self.reader = self.sock.makefile("r", encoding="utf-8", newline="\n")

    def run(self, sql):
        self.sock.sendall((sql + "\n").encode())
        return self.reader.readline().rstrip("\n")

    def rows(self, sql):
        """None for a refusal; else the rows after the header."""
        reply = self.run(sql)
        if reply.startswith("ERR"):
            return None, reply
        parts = reply.split("\\n")
        return parts[1:], reply

    def close(self):
        try:
            self.run("STOP")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


class Pg:
    def __init__(self, port):
        self.port = port
        base = [PSQL, "-h", "127.0.0.1", "-p", str(port), "-X", "-q", "-v", "ON_ERROR_STOP=1"]
        subprocess.run(base + ["-d", "postgres", "-c", "DROP DATABASE IF EXISTS bs_diff"],
                       check=True, capture_output=True)
        subprocess.run(base + ["-d", "postgres", "-c", "CREATE DATABASE bs_diff"],
                       check=True, capture_output=True)
        self.base = base + ["-d", "bs_diff", "-A", "-t", "-F", ",", "-P", "null=NULL"]

    def rows(self, sql):
        p = subprocess.run(self.base + ["-c", sql], capture_output=True, text=True)
        if p.returncode != 0:
            return None, p.stderr.strip()
        out = p.stdout.rstrip("\n")
        return (out.split("\n") if out else []), out


class Gen:
    """The generator: only what census §4 found to mean one thing in both."""

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.next_id = {t: 1 for t in TABLES}

    def value(self, col):
        r = self.r
        if col == "a":
            return str(r.randint(0, 9))
        if col == "b":
            return str(r.randint(-50, 50))
        if col == "c":
            return "NULL" if r.random() < 0.2 else f"{r.randint(-999, 999)}.{r.randint(0, 99):02d}"
        if col == "d":
            return "NULL" if r.random() < 0.2 else f"'2026-{r.randint(1, 12):02d}-{r.randint(1, 28):02d}'"
        if col == "s":
            return "NULL" if r.random() < 0.2 else "'" + r.choice(["ab", "abc", "b", "ba", "zz", "q"]) + "'"
        raise ValueError(col)

    def insert(self, t, n):
        rows = []
        for _ in range(n):
            i = self.next_id[t]
            self.next_id[t] += 1 + self.r.randint(0, 2)
            rows.append("(" + ", ".join([str(i)] + [self.value(c) for c in "abcds"]) + ")")
        return f"INSERT INTO {t} VALUES " + ", ".join(rows)

    def term(self, alias=""):
        r = self.r
        p = alias
        k = r.randint(0, 9)
        choices = [
            f"{p}a = {k}", f"{p}a != {k}", f"{p}b < {r.randint(-50, 50)}",
            f"{p}b BETWEEN {r.randint(-50, 0)} AND {r.randint(0, 50)}",
            f"{p}c IS NULL", f"{p}c IS NOT NULL", f"{p}c > {r.randint(-500, 500)}.00",
            f"{p}d >= '2026-{r.randint(1, 12):02d}-01'", f"{p}s = 'ab'", f"{p}s IS NULL",
            f"{p}id BETWEEN {r.randint(1, 40)} AND {r.randint(40, 120)}",
        ]
        # The query surface BU added after the census (BU-S1, BU-S2): OR and
        # NOT groups, value lists, LIKE.
        if r.random() < 0.25:
            choices += [
                f"({r.choice(choices)} OR {r.choice(choices)})",
                f"NOT ({r.choice(choices)})",
                f"{p}a IN ({', '.join(str(r.randint(0, 9)) for _ in range(3))})",
                f"{p}a NOT IN ({r.randint(0, 9)}, {r.randint(0, 9)})",
                f"{p}s LIKE '{r.choice(['a', 'b', 'z'])}%'",
            ]
        return r.choice(choices)

    def where(self, alias="", most=3):
        n = self.r.randint(0, most)
        return (" WHERE " + " AND ".join(self.term(alias) for _ in range(n))) if n else ""

    def query(self):
        """(sql, ordered)"""
        r = self.r
        t, u = r.sample(TABLES, 2)
        kind = r.randint(0, 11)
        if kind == 0:
            return f"SELECT id, a, b, c, d, s FROM {t}{self.where()}", False
        if kind == 1:
            return (f"SELECT x.id, y.id, x.b, y.c FROM {t} AS x JOIN {u} AS y ON x.a = y.a"
                    f"{self.where('x.', 2)}"), False
        if kind == 2:
            return (f"SELECT a, COUNT(*), MIN(b), MAX(b), SUM(b) FROM {t}{self.where()} "
                    f"GROUP BY a"), False
        if kind == 3:
            lim, off = r.randint(1, 10), r.randint(0, 5)
            return (f"SELECT id, b FROM {t}{self.where()} ORDER BY b DESC, id "
                    f"LIMIT {lim} OFFSET {off}"), True
        if kind == 4:
            neg = r.choice(["", "NOT "])
            return (f"SELECT id FROM {t} WHERE a {neg}IN (SELECT a FROM {u}{self.where('', 1)})"), False
        if kind == 5:
            k = r.randint(1, 60)
            return f"SELECT id, b FROM {t} WHERE b > (SELECT b FROM {u} WHERE id = {k})", False
        if kind == 6:
            return (f"SELECT id FROM {t} AS x WHERE EXISTS (SELECT id FROM {u} AS y "
                    f"WHERE y.a = x.a AND y.b < x.b)"), False
        # The query layer's forms (BU-S4..S7).
        if kind == 7:
            return (f"SELECT x.id, y.id FROM {t} AS x LEFT JOIN {u} AS y "
                    f"ON x.a = y.a AND y.b > 0{self.where('x.', 1)}"), False
        if kind == 8:
            return f"SELECT DISTINCT a FROM {t}{self.where()}", False
        if kind == 9:
            return (f"SELECT a FROM {t}{self.where('', 1)} UNION "
                    f"SELECT a FROM {u}{self.where('', 1)}"), False
        if kind == 10:
            # Within one type family: KDS widens nothing (types.md §3.2b, BJ-R3).
            return f"SELECT id, b * 2 + b, a - a * 3 FROM {t}{self.where()}", False
        return (f"SELECT a, COUNT(*) FROM {t} GROUP BY a HAVING COUNT(*) > {r.randint(0, 3)}"), False

    def write(self):
        r = self.r
        t = r.choice(TABLES)
        kind = r.randint(0, 2)
        if kind == 0:
            return self.insert(t, r.randint(1, 4))
        if kind == 1:
            return f"UPDATE {t} SET b = b + {r.randint(-5, 5)}, s = 'zz'{self.where('', 2)}"
        return f"DELETE FROM {t}{self.where('', 2) or ' WHERE a = ' + str(r.randint(0, 9))}"


def canon(rows):
    # Surrounding whitespace only; no value is rewritten (BS-R4: normalize
    # only what a spec line states, and none is needed yet - both render a
    # decimal at its column's scale, NULL as `NULL`, a date as ISO).
    return [r.strip() for r in rows]


def run_seed(args, seed):
    g = Gen(seed)
    # The twin first: a refusal there must not leave a kds_server running.
    pg = Pg(args.pg_port)
    kds = Kds(args.kds_server)
    mismatches = []
    compared = 0
    nonempty = 0       # answers with a row: what makes a match say something
    both_refused = []  # a statement both refuse is a generator defect to fix
    try:
        for t in TABLES:
            kds_ddl = (f"CREATE TABLE {t} (id int64, a int32, b int64, c decimal(8,2) NULL, "
                       f"d date NULL, s varchar NULL) BTREE")
            pg_ddl = (f"CREATE TABLE {t} (id bigint PRIMARY KEY, a integer NOT NULL, "
                      f"b bigint NOT NULL, c numeric(8,2), d date, s text)")
            if kds.run(kds_ddl).startswith("ERR") or pg.rows(pg_ddl)[0] is None:
                raise RuntimeError("DDL refused: " + t)
            for _ in range(8):
                sql = g.insert(t, 5)
                a, b = kds.run(sql), pg.rows(sql)
                if a.startswith("ERR") or b[0] is None:
                    raise RuntimeError(f"setup insert refused: {sql}: {a} / {b[1]}")
        for i in range(args.queries):
            if g.r.random() < 0.2:
                sql = g.write()
                a, b = kds.run(sql), pg.rows(sql)
                if a.startswith("ERR") != (b[0] is None):
                    # The data sets part here, so every later answer would
                    # differ for this one reason: the seed ends.
                    mismatches.append((sql, "one engine refused a write", a, b[1]))
                    break
                if a.startswith("ERR"):
                    both_refused.append(sql)
                continue
            sql, ordered = g.query()
            ka, kraw = kds.rows(sql)
            pa, praw = pg.rows(sql)
            compared += 1
            if ka is None or pa is None:
                if (ka is None) != (pa is None):
                    mismatches.append((sql, "one engine refused", kraw, praw))
                else:
                    both_refused.append(sql)
                continue
            nonempty += 1 if pa else 0
            ka, pa = canon(ka), canon(pa)
            if not ordered:
                ka, pa = sorted(ka), sorted(pa)
            if ka != pa:
                mismatches.append((sql, "answers differ", kraw[:600], praw[:600]))
    finally:
        kds.close()
    for sql, why, a, b in mismatches:
        print(f"MISMATCH ({why}): {sql}\n  kds: {a}\n  pg:  {b}")
    for sql in both_refused[:5]:
        print(f"BOTH REFUSED (a generator defect): {sql}")
    print(f"difftest: seed={seed} {compared} queries compared, {nonempty} with rows, "
          f"{len(both_refused)} refused by both, {len(mismatches)} mismatch(es)", flush=True)
    return 1 if mismatches or both_refused else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kds_server")
    ap.add_argument("--seeds", default="1", help="comma-separated; each a fresh pair of engines")
    ap.add_argument("--queries", type=int, default=400)
    ap.add_argument("--pg-port", type=int, default=15434)
    args = ap.parse_args()
    failed = [s for s in args.seeds.split(",") if run_seed(args, int(s)) != 0]
    print(f"difftest: {len(args.seeds.split(','))} seed(s), {len(failed)} with a mismatch")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
